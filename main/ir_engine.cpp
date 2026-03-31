#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <driver/rmt.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <nvs.h>

#include "bridge_action.h"
#include "ir_engine.h"
#include "ir_mgmt_cluster.h"
#include "status_led.h"

static const char *TAG = "ir_engine";
static const char *kNvsCacheNamespace = "ir_cache";
static constexpr uint8_t kRxCount = 2;
static constexpr gpio_num_t kTxGpio = GPIO_NUM_4;
static constexpr gpio_num_t kRxGpios[kRxCount] = { GPIO_NUM_5, GPIO_NUM_6 };
static constexpr rmt_channel_t kTxChannel = RMT_CHANNEL_2;
static constexpr rmt_channel_t kRxChannels[kRxCount] = { RMT_CHANNEL_4, RMT_CHANNEL_5 };
static constexpr uint32_t kRmtClkDiv = 80; // 1 tick = 1us on 80MHz APB
static constexpr uint32_t kRxIdleThresholdUs = 12000;
static constexpr uint32_t kRxFilterThresholdUs = 100;
static constexpr uint8_t kMinCapturePayloadLen[kRxCount] = { 4, 4 };
static constexpr uint32_t kMinCaptureTotalUs[kRxCount] = { 2000, 3000 };
static constexpr uint64_t kNoiseLogIntervalUs = 1000000;
static constexpr uint8_t kMaxRepeatCount = 5;
static constexpr uint32_t kRepeatGapUs = 12000;

static ir_learning_status_t s_learning = {
    .state = IR_LEARNING_IDLE,
    .elapsed_ms = 0,
    .timeout_ms = 0,
    .last_signal_id = 0,
    .rx_source = 0,
    .captured_len = 0,
    .quality_score = 0,
};
static uint64_t s_learning_started_us = 0;
static bool s_has_pending_learning = false;
static bool s_hw_initialized = false;
static RingbufHandle_t s_rx_ringbufs[kRxCount] = { nullptr, nullptr };
static TaskHandle_t s_learning_task = nullptr;
static uint16_t s_pending_payload[128] = { 0 };
static uint8_t s_pending_payload_len = 0;
static uint8_t s_pending_rx_source = 0;
static uint16_t s_pending_quality = 0;
static uint32_t s_noise_reject_count[kRxCount] = { 0 };
static uint64_t s_noise_last_log_us[kRxCount] = { 0 };

static SemaphoreHandle_t s_tx_mutex = nullptr;

static size_t ticks_to_rmt_items(const uint16_t *ticks, size_t tick_count, rmt_item32_t *out_items, size_t max_items)
{
    const size_t item_count = (tick_count + 1U) / 2U;
    if (item_count > max_items) {
        return 0;
    }
    size_t tick_idx = 0;
    for (size_t i = 0; i < item_count; ++i) {
        out_items[i].level0 = 1;
        out_items[i].duration0 = ticks[tick_idx++];
        out_items[i].level1 = 0;
        if (tick_idx < tick_count) {
            out_items[i].duration1 = ticks[tick_idx++];
        } else {
            out_items[i].duration1 = 0;
        }
    }
    return item_count;
}

static void make_binding_key(uint8_t slot_id, const char *suffix, char *out_key, size_t out_size)
{
    snprintf(out_key, out_size, "s%u_%s", static_cast<unsigned>(slot_id), suffix);
}

static void stop_rx_capture()
{
    for (uint8_t i = 0; i < kRxCount; ++i) {
        rmt_rx_stop(kRxChannels[i]);
    }
}

static void drain_rx_ringbuffers()
{
    for (uint8_t i = 0; i < kRxCount; ++i) {
        if (!s_rx_ringbufs[i]) {
            continue;
        }
        size_t item_size = 0;
        void *items = nullptr;
        do {
            items = xRingbufferReceive(s_rx_ringbufs[i], &item_size, 0);
            if (items) {
                vRingbufferReturnItem(s_rx_ringbufs[i], items);
            }
        } while (items != nullptr);
    }
}

static bool copy_rx_items_to_payload(const rmt_item32_t *items, size_t item_count, uint16_t *out_payload, uint8_t *out_len)
{
    if (!items || !out_payload || !out_len) {
        return false;
    }

    size_t write_index = 0;
    const size_t max_len = sizeof(s_pending_payload) / sizeof(s_pending_payload[0]);
    const uint32_t mark_level = 0; // VS1838B output is typically active-low during IR burst
    bool started = false;
    bool expect_mark = true;

    for (size_t i = 0; i < item_count && write_index < max_len; ++i) {
        const uint32_t levels[2] = { items[i].level0, items[i].level1 };
        const uint32_t durations[2] = { items[i].duration0, items[i].duration1 };

        for (int edge = 0; edge < 2 && write_index < max_len; ++edge) {
            const uint32_t level = levels[edge];
            const uint32_t duration = durations[edge];
            if (duration == 0) {
                continue;
            }

            const bool is_mark = (level == mark_level);
            if (!started) {
                if (!is_mark) {
                    continue;
                }
                started = true;
                expect_mark = true;
            }

            if (expect_mark != is_mark) {
                continue;
            }

            out_payload[write_index++] = static_cast<uint16_t>(duration);
            expect_mark = !expect_mark;
        }
    }

    if (write_index == 0) {
        return false;
    }

    *out_len = static_cast<uint8_t>(write_index);
    return true;
}

static bool try_capture_from_rx(uint8_t rx_index)
{
    if (rx_index >= kRxCount || !s_rx_ringbufs[rx_index]) {
        return false;
    }

    size_t item_size = 0;
    rmt_item32_t *items = static_cast<rmt_item32_t *>(xRingbufferReceive(s_rx_ringbufs[rx_index], &item_size, 0));
    if (!items) {
        return false;
    }
    if (item_size < sizeof(rmt_item32_t)) {
        vRingbufferReturnItem(s_rx_ringbufs[rx_index], items);
        return false;
    }

    uint8_t payload_len = 0;
    const size_t item_count = item_size / sizeof(rmt_item32_t);
    bool ok = copy_rx_items_to_payload(items, item_count, s_pending_payload, &payload_len);
    vRingbufferReturnItem(s_rx_ringbufs[rx_index], items);

    if (!ok) {
        return false;
    }

    uint32_t total_us = 0;
    for (uint8_t i = 0; i < payload_len; ++i) {
        total_us += s_pending_payload[i];
    }
    const uint8_t min_len = kMinCapturePayloadLen[rx_index];
    const uint32_t min_total_us = kMinCaptureTotalUs[rx_index];
    if (payload_len < min_len || total_us < min_total_us) {
        s_noise_reject_count[rx_index]++;
        const uint64_t now_us = esp_timer_get_time();
        if ((now_us - s_noise_last_log_us[rx_index]) >= kNoiseLogIntervalUs) {
            ESP_LOGW(TAG,
                     "Ignored noisy RX%u capture x%lu (last len=%u total_us=%lu, threshold len>=%u total_us>=%lu)",
                     static_cast<unsigned>(rx_index + 1), static_cast<unsigned long>(s_noise_reject_count[rx_index]),
                     payload_len, static_cast<unsigned long>(total_us), static_cast<unsigned>(min_len),
                     static_cast<unsigned long>(min_total_us));
            s_noise_reject_count[rx_index] = 0;
            s_noise_last_log_us[rx_index] = now_us;
        }
        return false;
    }

    s_pending_payload_len = payload_len;
    s_pending_rx_source = static_cast<uint8_t>(rx_index + 1);
    s_pending_quality = static_cast<uint16_t>(payload_len);
    return true;
}

static void update_learning_capture()
{
    if (s_learning.state != IR_LEARNING_IN_PROGRESS) {
        return;
    }

    if (s_learning_started_us != 0) {
        uint64_t now_us = esp_timer_get_time();
        s_learning.elapsed_ms = static_cast<uint32_t>((now_us - s_learning_started_us) / 1000);
    }

    for (uint8_t i = 0; i < kRxCount; ++i) {
        if (try_capture_from_rx(i)) {
            stop_rx_capture();
            drain_rx_ringbuffers();
            s_has_pending_learning = true;
            s_learning.state = IR_LEARNING_READY;
            s_learning.rx_source = s_pending_rx_source;
            s_learning.captured_len = s_pending_payload_len;
            s_learning.quality_score = s_pending_quality;
            status_led_set_learning(IR_LEARNING_READY);
            ESP_LOGI(TAG, "IR learning captured from RX%u len=%u", s_pending_rx_source, s_pending_payload_len);
            ir_mgmt_refresh_attributes();
            return;
        }
    }

    if (s_learning.timeout_ms > 0 && s_learning.elapsed_ms >= s_learning.timeout_ms) {
        stop_rx_capture();
        drain_rx_ringbuffers();
        s_learning.state = IR_LEARNING_FAILED;
        s_has_pending_learning = false;
        s_pending_payload_len = 0;
        s_pending_quality = 0;
        s_pending_rx_source = 0;
        status_led_set_learning(IR_LEARNING_FAILED);
        ESP_LOGW(TAG, "IR learning timeout after %" PRIu32 "ms", s_learning.elapsed_ms);
        ir_mgmt_refresh_attributes();
    }
}

static void learning_task(void *arg)
{
    (void)arg;
    while (true) {
        if (s_learning.state == IR_LEARNING_IN_PROGRESS) {
            update_learning_capture();
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static esp_err_t init_rmt_hw()
{
    if (s_hw_initialized) {
        return ESP_OK;
    }

    rmt_config_t tx_cfg = {};
    tx_cfg.rmt_mode = RMT_MODE_TX;
    tx_cfg.channel = kTxChannel;
    tx_cfg.gpio_num = kTxGpio;
    tx_cfg.mem_block_num = 1;
    tx_cfg.clk_div = kRmtClkDiv;
    tx_cfg.tx_config.loop_en = false;
    tx_cfg.tx_config.carrier_en = true;
    tx_cfg.tx_config.carrier_freq_hz = 38000;
    tx_cfg.tx_config.carrier_duty_percent = 33;
    tx_cfg.tx_config.carrier_level = RMT_CARRIER_LEVEL_HIGH;
    tx_cfg.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
    tx_cfg.tx_config.idle_output_en = true;
    esp_err_t err = rmt_config(&tx_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to configure TX channel: %s", esp_err_to_name(err));
        return err;
    }
    err = rmt_driver_install(kTxChannel, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to install TX driver: %s", esp_err_to_name(err));
        return err;
    }

    for (uint8_t i = 0; i < kRxCount; ++i) {
        rmt_config_t rx_cfg = {};
        rx_cfg.rmt_mode = RMT_MODE_RX;
        rx_cfg.channel = kRxChannels[i];
        rx_cfg.gpio_num = kRxGpios[i];
        rx_cfg.mem_block_num = 1;
        rx_cfg.clk_div = kRmtClkDiv;
        rx_cfg.rx_config.filter_en = true;
        rx_cfg.rx_config.filter_ticks_thresh = kRxFilterThresholdUs;
        rx_cfg.rx_config.idle_threshold = kRxIdleThresholdUs;
        err = rmt_config(&rx_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to configure RX%u: %s", i + 1, esp_err_to_name(err));
            goto cleanup_rmt;
        }
        err = rmt_driver_install(kRxChannels[i], 2048, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to install RX%u: %s", i + 1, esp_err_to_name(err));
            goto cleanup_rmt;
        }
        err = rmt_get_ringbuf_handle(kRxChannels[i], &s_rx_ringbufs[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed ringbuffer RX%u: %s", i + 1, esp_err_to_name(err));
            goto cleanup_rmt;
        }
    }

    s_hw_initialized = true;
    ESP_LOGI(TAG, "IR RMT initialized TX=%d RX={%d,%d}", static_cast<int>(kTxGpio), static_cast<int>(kRxGpios[0]),
             static_cast<int>(kRxGpios[1]));
    return ESP_OK;

cleanup_rmt:
    for (uint8_t j = 0; j < kRxCount; ++j) {
        rmt_driver_uninstall(kRxChannels[j]);
        s_rx_ringbufs[j] = nullptr;
    }
    rmt_driver_uninstall(kTxChannel);
    return err;
}

esp_err_t ir_engine_init()
{
    s_learning.state = IR_LEARNING_IDLE;
    s_learning.elapsed_ms = 0;
    s_learning.timeout_ms = 0;
    s_learning.last_signal_id = 0;
    s_learning.rx_source = 0;
    s_learning.captured_len = 0;
    s_learning.quality_score = 0;
    s_learning_started_us = 0;
    s_has_pending_learning = false;
    s_pending_payload_len = 0;
    s_pending_rx_source = 0;
    s_pending_quality = 0;

    if (!s_tx_mutex) {
        s_tx_mutex = xSemaphoreCreateMutex();
        if (!s_tx_mutex) {
            ESP_LOGE(TAG, "Failed to create TX mutex");
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = init_rmt_hw();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_learning_task) {
        if (xTaskCreate(learning_task, "ir_learning", 3072, nullptr, 5, &s_learning_task) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create IR learning task");
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "IR engine initialized");
    return ESP_OK;
}

esp_err_t ir_engine_send_signal(uint32_t signal_id, uint8_t slot_id, uint32_t cluster_id, uint32_t attribute_id,
                                const esp_matter_attr_val_t *val)
{
    if (!s_hw_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (signal_id == 0) {
        ESP_LOGW(TAG, "No signal mapped for slot %u cluster=0x%04" PRIx32 " attr=0x%04" PRIx32, slot_id, cluster_id, attribute_id);
        return ESP_ERR_NOT_FOUND;
    }

    // Read signal directly from NVS
    char ckey[16];
    snprintf(ckey, sizeof(ckey), "c%" PRIu32, signal_id);

    nvs_handle_t nvs_handle;
    esp_err_t nvs_err = nvs_open(kNvsCacheNamespace, NVS_READONLY, &nvs_handle);
    if (nvs_err != ESP_OK) {
        ESP_LOGW(TAG, "Signal %" PRIu32 " not found in NVS (open failed: %s)", signal_id, esp_err_to_name(nvs_err));
        return ESP_ERR_NOT_FOUND;
    }

    // New format: header=56 bytes (4+4+1+2+4+1+40), old format: header=23 bytes (4+4+1+2+4+8)
    static constexpr size_t kOldHeaderSize = 4 + 4 + 1 + 2 + 4 + 8;       // 23
    static constexpr size_t kNewHeaderSize = 4 + 4 + 1 + 2 + 4 + 1 + 40;  // 56
    uint8_t blob[kNewHeaderSize + 128 * 2];
    size_t blob_size = sizeof(blob);
    nvs_err = nvs_get_blob(nvs_handle, ckey, blob, &blob_size);
    nvs_close(nvs_handle);

    if (nvs_err != ESP_OK || blob_size < 11) {
        ESP_LOGW(TAG, "Signal %" PRIu32 " not found in NVS", signal_id);
        return ESP_ERR_NOT_FOUND;
    }

    size_t off = 0;
    uint32_t sig_id_stored, carrier_hz;
    uint8_t repeat;
    uint16_t tc16;
    memcpy(&sig_id_stored, blob + off, 4); off += 4;
    memcpy(&carrier_hz,    blob + off, 4); off += 4;
    memcpy(&repeat,        blob + off, 1); off += 1;
    memcpy(&tc16,          blob + off, 2); off += 2;
    // Detect format by blob size and skip header fields accordingly
    if (blob_size >= kNewHeaderSize + tc16 * sizeof(uint16_t)) {
        off += 4 + 1 + 40; // skip ref_count + history_count + history[5]
    } else if (blob_size >= kOldHeaderSize + tc16 * sizeof(uint16_t)) {
        off += 4 + 8; // skip ref_count + last_seen_at (old format)
    }
    if (tc16 == 0 || tc16 > 128 || blob_size < off + tc16 * sizeof(uint16_t)) {
        ESP_LOGW(TAG, "Signal %" PRIu32 " blob corrupt", signal_id);
        return ESP_ERR_NOT_FOUND;
    }

    uint16_t ticks[128];
    memcpy(ticks, blob + off, tc16 * sizeof(uint16_t));

    rmt_item32_t items[64] = {};
    size_t item_count = ticks_to_rmt_items(ticks, tc16, items, 64);
    if (item_count == 0) {
        ESP_LOGW(TAG, "Signal %" PRIu32 " ticks_to_rmt failed", signal_id);
        return ESP_ERR_INVALID_SIZE;
    }

    if (repeat > kMaxRepeatCount) {
        ESP_LOGW(TAG, "Clamping repeat from %u to %u", repeat, kMaxRepeatCount);
        repeat = kMaxRepeatCount;
    }

    status_led_notify_ir_tx();

    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    for (uint8_t i = 0; i < repeat; ++i) {
        esp_err_t err = rmt_write_items(kTxChannel, items, item_count, true);
        if (err != ESP_OK) {
            xSemaphoreGive(s_tx_mutex);
            ESP_LOGE(TAG, "TX failed for signal_id=%" PRIu32 ": %s", signal_id, esp_err_to_name(err));
            return err;
        }
        if (i + 1U < repeat) {
            esp_rom_delay_us(kRepeatGapUs);
        }
    }
    xSemaphoreGive(s_tx_mutex);

    ESP_LOGI(TAG,
             "TX signal_id=%" PRIu32 " slot=%u cluster=0x%04" PRIx32 " attr=0x%04" PRIx32 " items=%u repeat=%u carrier=%" PRIu32,
             signal_id, slot_id, cluster_id, attribute_id, static_cast<unsigned>(item_count), repeat, carrier_hz);
    (void)val;
    return ESP_OK;
}

esp_err_t ir_engine_start_learning(uint32_t timeout_ms)
{
    if (!s_hw_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_learning.state == IR_LEARNING_IN_PROGRESS) {
        return ESP_ERR_INVALID_STATE;
    }

    s_pending_payload_len = 0;
    s_pending_rx_source = 0;
    s_pending_quality = 0;
    memset(s_noise_reject_count, 0, sizeof(s_noise_reject_count));
    memset(s_noise_last_log_us, 0, sizeof(s_noise_last_log_us));
    s_has_pending_learning = false;
    drain_rx_ringbuffers();

    for (uint8_t i = 0; i < kRxCount; ++i) {
        esp_err_t err = rmt_rx_start(kRxChannels[i], true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Failed to start RX%u: %s", i + 1, esp_err_to_name(err));
            for (uint8_t j = 0; j < i; ++j) {
                rmt_rx_stop(kRxChannels[j]);
            }
            return err;
        }
    }

    s_learning.state = IR_LEARNING_IN_PROGRESS;
    s_learning.timeout_ms = timeout_ms;
    s_learning.elapsed_ms = 0;
    s_learning.rx_source = 0;
    s_learning.captured_len = 0;
    s_learning.quality_score = 0;
    s_learning_started_us = esp_timer_get_time();
    status_led_set_learning(IR_LEARNING_IN_PROGRESS);
    ESP_LOGI(TAG, "IR learning started timeout=%" PRIu32 "ms (RX=2)", timeout_ms);
    return ESP_OK;
}

void ir_engine_get_learning_status(ir_learning_status_t *status)
{
    if (!status) {
        return;
    }

    update_learning_capture();
    *status = s_learning;
}

const uint16_t *ir_engine_get_learned_ticks(uint8_t *out_len, uint32_t *out_carrier)
{
    if (s_learning.state != IR_LEARNING_READY || s_pending_payload_len == 0) {
        if (out_len) *out_len = 0;
        if (out_carrier) *out_carrier = 0;
        return nullptr;
    }
    if (out_len) *out_len = s_pending_payload_len;
    if (out_carrier) *out_carrier = 38000;  // IR learning uses default carrier
    return s_pending_payload;
}

int ir_engine_read_all_nvs_signals(char *out_json, size_t out_size)
{
    if (!out_json || out_size < 3) return 0;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsCacheNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        snprintf(out_json, out_size, "[]");
        return 2;
    }

    int off = 0;
    off += snprintf(out_json + off, out_size - off, "[");
    bool first = true;

    // Iterate NVS entries starting with 'c' in ir_cache namespace
    nvs_iterator_t it = nullptr;
    err = nvs_entry_find("nvs", kNvsCacheNamespace, NVS_TYPE_BLOB, &it);
    while (err == ESP_OK && it != nullptr) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);

        // Only process signal keys (c{id})
        if (info.key[0] == 'c' && info.key[1] >= '0' && info.key[1] <= '9') {
            size_t blob_size = 0;
            // New header=56, old header=23; max ticks = 128*2 = 256
            static constexpr size_t kOldHdr = 4 + 4 + 1 + 2 + 4 + 8;      // 23
            static constexpr size_t kNewHdr = 4 + 4 + 1 + 2 + 4 + 1 + 40; // 56
            if (nvs_get_blob(handle, info.key, nullptr, &blob_size) == ESP_OK && blob_size >= 11) {
                uint8_t blob[kNewHdr + 128 * 2];
                if (blob_size <= sizeof(blob) && nvs_get_blob(handle, info.key, blob, &blob_size) == ESP_OK) {
                    uint32_t sig_id, carr;
                    uint8_t rep;
                    uint16_t tc;
                    uint32_t ref_count = 0;
                    uint8_t  hist_count = 0;
                    int64_t  history[5] = {};
                    size_t p = 0;
                    memcpy(&sig_id, blob + p, 4); p += 4;
                    memcpy(&carr,   blob + p, 4); p += 4;
                    memcpy(&rep,    blob + p, 1); p += 1;
                    memcpy(&tc,     blob + p, 2); p += 2;
                    if (blob_size >= kNewHdr) {
                        // New format: ref_count(4) + history_count(1) + history[5*8]
                        memcpy(&ref_count,  blob + p, 4); p += 4;
                        memcpy(&hist_count, blob + p, 1); p += 1;
                        if (hist_count > 5) hist_count = 5;
                        for (uint8_t hi = 0; hi < hist_count; ++hi) {
                            memcpy(&history[hi], blob + p, 8); p += 8;
                        }
                    } else if (blob_size >= kOldHdr) {
                        // Old format: ref_count(4) + last_seen_at(8)
                        int64_t last_seen = 0;
                        memcpy(&ref_count, blob + p, 4); p += 4;
                        memcpy(&last_seen, blob + p, 8); p += 8;
                        if (last_seen != 0) {
                            hist_count = 1;
                            history[0] = last_seen;
                        }
                    }

                    // Build history JSON array — budget: 5 * (20 digits + separator) ~ 110 chars
                    // plus outer fields ~ 120 chars; reserve 250 total
                    if (off < (int)(out_size - 250)) {
                        off += snprintf(out_json + off, out_size - off,
                                        "%s{\"signal_id\":%lu,\"carrier_hz\":%lu,\"repeat\":%u,\"tick_count\":%u,\"ref_count\":%lu,\"history\":[",
                                        first ? "" : ",",
                                        static_cast<unsigned long>(sig_id),
                                        static_cast<unsigned long>(carr),
                                        rep, tc,
                                        static_cast<unsigned long>(ref_count));
                        for (uint8_t hi = 0; hi < hist_count; ++hi) {
                            off += snprintf(out_json + off, out_size - off,
                                            "%s%lld",
                                            hi == 0 ? "" : ",",
                                            static_cast<long long>(history[hi]));
                        }
                        off += snprintf(out_json + off, out_size - off, "]}");
                        first = false;
                    }
                }
            }
        }
        err = nvs_entry_next(&it);
    }
    if (it) nvs_release_iterator(it);

    off += snprintf(out_json + off, out_size - off, "]");
    nvs_close(handle);
    return off;
}

esp_err_t ir_engine_send_raw(uint32_t signal_id, uint32_t carrier_hz, uint8_t repeat, const uint16_t *ticks,
                              size_t tick_count)
{
    if (!s_hw_initialized) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_learning.state == IR_LEARNING_IN_PROGRESS) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!ticks || tick_count == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Safe copy to avoid Xtensa alignment fault on unaligned byte pointer
    uint16_t local_ticks[128];
    const size_t copy_count = tick_count < 128 ? tick_count : 128;
    memcpy(local_ticks, ticks, copy_count * sizeof(uint16_t));

    rmt_item32_t items[64] = {};
    size_t item_count = ticks_to_rmt_items(local_ticks, copy_count, items, sizeof(items) / sizeof(items[0]));
    if (item_count == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t actual_repeat = repeat == 0 ? 1 : repeat;
    if (actual_repeat > kMaxRepeatCount) {
        actual_repeat = kMaxRepeatCount;
    }

    status_led_notify_ir_tx();

    xSemaphoreTake(s_tx_mutex, portMAX_DELAY);
    for (uint8_t i = 0; i < actual_repeat; ++i) {
        esp_err_t err = rmt_write_items(kTxChannel, items, item_count, true);
        if (err != ESP_OK) {
            xSemaphoreGive(s_tx_mutex);
            ESP_LOGE(TAG, "TX raw failed signal_id=%" PRIu32 ": %s", signal_id, esp_err_to_name(err));
            return err;
        }
        if (i + 1U < actual_repeat) {
            esp_rom_delay_us(kRepeatGapUs);
        }
    }
    xSemaphoreGive(s_tx_mutex);

    ESP_LOGI(TAG, "TX raw signal_id=%" PRIu32 " items=%u repeat=%u carrier=%" PRIu32, signal_id,
             static_cast<unsigned>(item_count), actual_repeat, carrier_hz);

    // Save to NVS immediately after TX (direct NVS store — no RAM buffer)
    if (signal_id != 0) {
        char ckey[16];
        snprintf(ckey, sizeof(ckey), "c%" PRIu32, signal_id);

        nvs_handle_t nvs_h;
        esp_err_t nvs_err = nvs_open(kNvsCacheNamespace, NVS_READWRITE, &nvs_h);
        if (nvs_err == ESP_OK) {
            // Read existing blob to get ref_count and history
            uint32_t existing_ref_count = 0;
            uint8_t  existing_hist_count = 0;
            int64_t  existing_history[5] = {};
            static constexpr size_t kOldHdrSave = 4 + 4 + 1 + 2 + 4 + 8;      // 23
            static constexpr size_t kNewHdrSave = 4 + 4 + 1 + 2 + 4 + 1 + 40; // 56
            {
                size_t existing_size = 0;
                if (nvs_get_blob(nvs_h, ckey, nullptr, &existing_size) == ESP_OK &&
                    existing_size >= kOldHdrSave) {
                    uint8_t existing_blob[kNewHdrSave + 128 * 2];
                    if (existing_size <= sizeof(existing_blob) &&
                        nvs_get_blob(nvs_h, ckey, existing_blob, &existing_size) == ESP_OK) {
                        memcpy(&existing_ref_count, existing_blob + 11, 4);
                        if (existing_size >= kNewHdrSave) {
                            // New format: history_count at offset 15
                            memcpy(&existing_hist_count, existing_blob + 15, 1);
                            if (existing_hist_count > 5) existing_hist_count = 5;
                            for (uint8_t hi = 0; hi < existing_hist_count; ++hi) {
                                memcpy(&existing_history[hi], existing_blob + 16 + hi * 8, 8);
                            }
                        } else {
                            // Old format: last_seen_at at offset 15
                            int64_t last_seen = 0;
                            memcpy(&last_seen, existing_blob + 15, 8);
                            if (last_seen != 0) {
                                existing_hist_count = 1;
                                existing_history[0] = last_seen;
                            }
                        }
                    }
                }
            }

            uint32_t new_ref_count = existing_ref_count + 1;
            int64_t now = static_cast<int64_t>(time(nullptr));

            // Shift history right and prepend current timestamp (newest-first ring buffer)
            uint8_t new_hist_count = existing_hist_count < 5 ? existing_hist_count + 1 : 5;
            int64_t new_history[5] = {};
            new_history[0] = now;
            for (uint8_t hi = 1; hi < new_hist_count; ++hi) {
                new_history[hi] = existing_history[hi - 1];
            }

            // Blob: sig_id(4)+carrier(4)+repeat(1)+tc16(2)+ref_count(4)+hist_count(1)+hist[5*8]+ticks
            const size_t blob_size = kNewHdrSave + copy_count * sizeof(uint16_t);
            uint8_t blob[kNewHdrSave + 128 * 2];
            size_t boff = 0;
            uint16_t tc16 = static_cast<uint16_t>(copy_count);
            memcpy(blob + boff, &signal_id,       4); boff += 4;
            memcpy(blob + boff, &carrier_hz,      4); boff += 4;
            memcpy(blob + boff, &repeat,          1); boff += 1;
            memcpy(blob + boff, &tc16,            2); boff += 2;
            memcpy(blob + boff, &new_ref_count,   4); boff += 4;
            memcpy(blob + boff, &new_hist_count,  1); boff += 1;
            memcpy(blob + boff, new_history,     40); boff += 40; // always write all 5 slots
            memcpy(blob + boff, local_ticks, copy_count * sizeof(uint16_t));

            nvs_set_blob(nvs_h, ckey, blob, blob_size);
            nvs_commit(nvs_h);
            nvs_close(nvs_h);
            ESP_LOGI(TAG, "Saved signal_id=%" PRIu32 " to NVS (ref_count=%" PRIu32 " hist=%u)",
                     signal_id, new_ref_count, new_hist_count);
        } else {
            ESP_LOGW(TAG, "Failed to open NVS for signal save: %s", esp_err_to_name(nvs_err));
        }
    }

    return ESP_OK;
}

esp_err_t ir_engine_persist_binding(uint8_t slot_id, const char *binding_suffix, uint32_t signal_id,
                                    uint32_t carrier_hz, uint8_t repeat, const uint16_t *ticks, size_t tick_count)
{
    if (!binding_suffix || !ticks || tick_count == 0 || tick_count > 128) {
        return ESP_ERR_INVALID_ARG;
    }

    // Build blob: signal_id(4) + carrier_hz(4) + repeat(1) + tick_count(2) + ticks(tick_count*2)
    const size_t blob_size = 4 + 4 + 1 + 2 + tick_count * sizeof(uint16_t);
    uint8_t blob[4 + 4 + 1 + 2 + 128 * 2];
    size_t offset = 0;
    memcpy(blob + offset, &signal_id, 4);   offset += 4;
    memcpy(blob + offset, &carrier_hz, 4);  offset += 4;
    memcpy(blob + offset, &repeat, 1);      offset += 1;
    uint16_t tc16 = static_cast<uint16_t>(tick_count);
    memcpy(blob + offset, &tc16, 2);        offset += 2;
    memcpy(blob + offset, ticks, tick_count * sizeof(uint16_t));

    char key[16];
    make_binding_key(slot_id, binding_suffix, key, sizeof(key));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsCacheNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_blob(handle, key, blob, blob_size);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist binding key=%s: %s", key, esp_err_to_name(err));
    }
    return err;
}

