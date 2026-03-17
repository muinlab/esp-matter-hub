#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <driver/rmt.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/ringbuf.h>
#include <freertos/task.h>
#include <nvs.h>

#include "ir_engine.h"
#include "status_led.h"

static const char *TAG = "ir_engine";
static const char *kNvsNamespace = "ir_signals";
static const char *kNvsKeyTable = "table";
static constexpr uint32_t kSignalTableVersion = 2;
static constexpr size_t kMaxSignals = 64;
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

typedef struct ir_signal_table {
    uint32_t version;
    uint32_t next_signal_id;
    uint32_t count;
    ir_signal_record_t records[kMaxSignals];
} ir_signal_table_t;

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
static ir_signal_table_t s_signal_table = {
    .version = kSignalTableVersion,
    .next_signal_id = 1,
    .count = 0,
};
static bool s_hw_initialized = false;
static RingbufHandle_t s_rx_ringbufs[kRxCount] = { nullptr, nullptr };
static TaskHandle_t s_learning_task = nullptr;
static uint16_t s_pending_payload[128] = { 0 };
static uint8_t s_pending_payload_len = 0;
static uint8_t s_pending_rx_source = 0;
static uint16_t s_pending_quality = 0;
static uint32_t s_noise_reject_count[kRxCount] = { 0 };
static uint64_t s_noise_last_log_us[kRxCount] = { 0 };

static void make_payload_key(uint32_t signal_id, char *out_key, size_t out_size)
{
    snprintf(out_key, out_size, "p%" PRIu32, signal_id);
}

static esp_err_t erase_signal_namespace()
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_erase_all(handle);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t save_signal_payload(uint32_t signal_id, const uint16_t *payload, uint8_t payload_len)
{
    if (!payload || payload_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    char key[16];
    make_payload_key(signal_id, key, sizeof(key));
    err = nvs_set_blob(handle, key, payload, payload_len * sizeof(uint16_t));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t load_signal_payload(uint32_t signal_id, uint16_t *payload, size_t payload_cap, uint8_t *out_payload_len)
{
    if (!payload || payload_cap == 0 || !out_payload_len) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    char key[16];
    make_payload_key(signal_id, key, sizeof(key));
    size_t blob_size = 0;
    err = nvs_get_blob(handle, key, nullptr, &blob_size);
    if (err != ESP_OK) {
        nvs_close(handle);
        return err;
    }
    if (blob_size == 0 || (blob_size % sizeof(uint16_t)) != 0 || (blob_size / sizeof(uint16_t)) > payload_cap) {
        nvs_close(handle);
        return ESP_ERR_INVALID_SIZE;
    }

    err = nvs_get_blob(handle, key, payload, &blob_size);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }

    *out_payload_len = static_cast<uint8_t>(blob_size / sizeof(uint16_t));
    return ESP_OK;
}

static esp_err_t erase_signal_payload(uint32_t signal_id)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    char key[16];
    make_payload_key(signal_id, key, sizeof(key));
    err = nvs_erase_key(handle, key);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        return ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t signal_table_save()
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace %s: %s", kNvsNamespace, esp_err_to_name(err));
        return err;
    }

    err = nvs_set_blob(handle, kNvsKeyTable, &s_signal_table, sizeof(s_signal_table));
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save signal table: %s", esp_err_to_name(err));
    }
    return err;
}

static esp_err_t signal_table_load()
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No signal table found in NVS; starting empty");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS namespace %s: %s", kNvsNamespace, esp_err_to_name(err));
        return err;
    }

    size_t required_size = 0;
    err = nvs_get_blob(handle, kNvsKeyTable, nullptr, &required_size);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(handle);
        ESP_LOGI(TAG, "No signal table key found; starting empty");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        ESP_LOGE(TAG, "Failed reading signal table size: %s", esp_err_to_name(err));
        return err;
    }
    if (required_size != sizeof(s_signal_table)) {
        nvs_close(handle);
        ESP_LOGW(TAG, "Signal table size mismatch (%u), resetting table", static_cast<unsigned>(required_size));
        esp_err_t erase_err = erase_signal_namespace();
        if (erase_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to erase signal namespace: %s", esp_err_to_name(erase_err));
        }
        memset(&s_signal_table, 0, sizeof(s_signal_table));
        s_signal_table.version = kSignalTableVersion;
        s_signal_table.next_signal_id = 1;
        s_signal_table.count = 0;
        return ESP_OK;
    }

    err = nvs_get_blob(handle, kNvsKeyTable, &s_signal_table, &required_size);
    nvs_close(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed loading signal table: %s", esp_err_to_name(err));
        return err;
    }

    if (s_signal_table.version != kSignalTableVersion || s_signal_table.count > kMaxSignals) {
        ESP_LOGW(TAG, "Invalid signal table version/count, resetting");
        esp_err_t erase_err = erase_signal_namespace();
        if (erase_err != ESP_OK) {
            ESP_LOGW(TAG, "Failed to erase signal namespace: %s", esp_err_to_name(erase_err));
        }
        memset(&s_signal_table, 0, sizeof(s_signal_table));
        s_signal_table.version = kSignalTableVersion;
        s_signal_table.next_signal_id = 1;
        s_signal_table.count = 0;
        return ESP_OK;
    }

    if (s_signal_table.next_signal_id == 0) {
        s_signal_table.next_signal_id = 1;
    }

    return ESP_OK;
}

static const ir_signal_record_t *find_signal(uint32_t signal_id)
{
    if (signal_id == 0) {
        return nullptr;
    }

    for (uint32_t i = 0; i < s_signal_table.count; ++i) {
        if (s_signal_table.records[i].signal_id == signal_id) {
            return &s_signal_table.records[i];
        }
    }
    return nullptr;
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

    memset(&s_signal_table, 0, sizeof(s_signal_table));
    s_signal_table.version = kSignalTableVersion;
    s_signal_table.next_signal_id = 1;
    s_signal_table.count = 0;

    esp_err_t err = signal_table_load();
    if (err != ESP_OK) {
        return err;
    }

    err = init_rmt_hw();
    if (err != ESP_OK) {
        return err;
    }

    if (!s_learning_task) {
        if (xTaskCreate(learning_task, "ir_learning", 3072, nullptr, 5, &s_learning_task) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create IR learning task");
            return ESP_ERR_NO_MEM;
        }
    }

    ESP_LOGI(TAG, "IR engine initialized (signals=%lu, next_id=%lu)", static_cast<unsigned long>(s_signal_table.count),
             static_cast<unsigned long>(s_signal_table.next_signal_id));
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

    const ir_signal_record_t *signal = find_signal(signal_id);
    if (!signal) {
        ESP_LOGW(TAG, "Signal %" PRIu32 " not found in NVS", signal_id);
        return ESP_ERR_NOT_FOUND;
    }

    if (signal->payload_len == 0 || signal->payload_len > (sizeof(s_pending_payload) / sizeof(s_pending_payload[0]))) {
        ESP_LOGW(TAG, "Signal %" PRIu32 " has invalid payload_len=%u", signal_id, signal->payload_len);
        return ESP_ERR_INVALID_STATE;
    }

    uint16_t payload_ticks[128] = { 0 };
    uint8_t loaded_len = 0;
    esp_err_t load_err = load_signal_payload(signal_id, payload_ticks, sizeof(payload_ticks) / sizeof(payload_ticks[0]), &loaded_len);
    if (load_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to load payload for signal %" PRIu32 ": %s", signal_id, esp_err_to_name(load_err));
        return load_err;
    }
    if (loaded_len == 0 || loaded_len != signal->payload_len) {
        ESP_LOGW(TAG, "Payload length mismatch for signal %" PRIu32 " meta=%u actual=%u", signal_id, signal->payload_len,
                 loaded_len);
        return ESP_ERR_INVALID_STATE;
    }

    rmt_item32_t items[64] = {};
    const size_t item_count = (loaded_len + 1U) / 2U;
    if (item_count > (sizeof(items) / sizeof(items[0]))) {
        ESP_LOGE(TAG, "payload too long for TX buffer: %u", signal->payload_len);
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t payload_index = 0;
    for (size_t i = 0; i < item_count; ++i) {
        items[i].level0 = 1;
        items[i].duration0 = payload_ticks[payload_index++];
        items[i].level1 = 0;
        if (payload_index < loaded_len) {
            items[i].duration1 = payload_ticks[payload_index++];
        } else {
            items[i].duration1 = 0;
        }
    }

    uint8_t repeat = signal->repeat == 0 ? 1 : signal->repeat;
    if (repeat > kMaxRepeatCount) {
        ESP_LOGW(TAG, "Clamping repeat from %u to %u", repeat, kMaxRepeatCount);
        repeat = kMaxRepeatCount;
    }

    status_led_notify_ir_tx();

    for (uint8_t i = 0; i < repeat; ++i) {
        esp_err_t err = rmt_write_items(kTxChannel, items, item_count, true);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "TX failed for signal_id=%" PRIu32 ": %s", signal_id, esp_err_to_name(err));
            return err;
        }
        if (i + 1U < repeat) {
            esp_rom_delay_us(kRepeatGapUs);
        }
    }

    ESP_LOGI(TAG,
             "TX signal_id=%" PRIu32 " name=%s slot=%u cluster=0x%04" PRIx32 " attr=0x%04" PRIx32 " len=%u repeat=%u carrier=%lu",
             signal_id, signal->name, slot_id, cluster_id, attribute_id, loaded_len, repeat,
             static_cast<unsigned long>(signal->carrier_hz));
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

esp_err_t ir_engine_commit_learning(const char *name, const char *device_type, uint32_t *out_signal_id)
{
    update_learning_capture();

    if (s_learning.state != IR_LEARNING_READY || !s_has_pending_learning || s_pending_payload_len == 0) {
        return ESP_ERR_INVALID_STATE;
    }
    if (s_signal_table.count >= kMaxSignals) {
        return ESP_ERR_NO_MEM;
    }

    ir_signal_record_t *record = &s_signal_table.records[s_signal_table.count];
    memset(record, 0, sizeof(*record));

    const uint32_t new_id = s_signal_table.next_signal_id;
    record->signal_id = new_id;
    record->carrier_hz = 38000;
    record->repeat = 1;
    record->payload_len = s_pending_payload_len;
    record->created_at_ms = static_cast<uint64_t>(esp_timer_get_time() / 1000ULL);

    if (name && name[0] != '\0') {
        strlcpy(record->name, name, sizeof(record->name));
    } else {
        snprintf(record->name, sizeof(record->name), "signal-%" PRIu32, new_id);
    }

    if (device_type && device_type[0] != '\0') {
        strlcpy(record->device_type, device_type, sizeof(record->device_type));
    } else {
        strlcpy(record->device_type, "unknown", sizeof(record->device_type));
    }

    s_signal_table.count++;
    s_signal_table.next_signal_id++;

    esp_err_t err = save_signal_payload(new_id, s_pending_payload, s_pending_payload_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save payload id=%" PRIu32 ": %s", new_id, esp_err_to_name(err));
        s_signal_table.count--;
        s_signal_table.next_signal_id--;
        memset(record, 0, sizeof(*record));
        return err;
    }

    err = signal_table_save();
    if (err != ESP_OK) {
        s_signal_table.count--;
        s_signal_table.next_signal_id--;
        memset(record, 0, sizeof(*record));
        (void)erase_signal_payload(new_id);
        return err;
    }

    s_learning.last_signal_id = new_id;
    s_learning.state = IR_LEARNING_IDLE;
    s_learning.elapsed_ms = 0;
    s_learning.timeout_ms = 0;
    s_learning_started_us = 0;
    s_has_pending_learning = false;
    status_led_set_learning(IR_LEARNING_IDLE);

    if (out_signal_id) {
        *out_signal_id = new_id;
    }

    ESP_LOGI(TAG, "Committed learned signal id=%" PRIu32 " name=%s type=%s rx=%u len=%u", new_id, record->name,
             record->device_type, s_pending_rx_source, s_pending_payload_len);
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

esp_err_t ir_engine_get_signals(const ir_signal_record_t **signals, size_t *count)
{
    if (!signals || !count) {
        return ESP_ERR_INVALID_ARG;
    }

    *signals = s_signal_table.records;
    *count = s_signal_table.count;
    return ESP_OK;
}

esp_err_t ir_engine_get_signal_payload(uint32_t signal_id, uint16_t *payload_ticks, size_t payload_cap, uint8_t *out_payload_len)
{
    const ir_signal_record_t *signal = find_signal(signal_id);
    if (!signal) {
        return ESP_ERR_NOT_FOUND;
    }
    if (signal->payload_len == 0 || payload_cap < signal->payload_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t loaded_len = 0;
    esp_err_t err = load_signal_payload(signal_id, payload_ticks, payload_cap, &loaded_len);
    if (err != ESP_OK) {
        return err;
    }
    if (loaded_len != signal->payload_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    *out_payload_len = loaded_len;
    return ESP_OK;
}

esp_err_t ir_engine_delete_signal(uint32_t signal_id)
{
    if (signal_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t index = UINT32_MAX;
    for (uint32_t i = 0; i < s_signal_table.count; ++i) {
        if (s_signal_table.records[i].signal_id == signal_id) {
            index = i;
            break;
        }
    }
    if (index == UINT32_MAX) {
        return ESP_ERR_NOT_FOUND;
    }

    if (index + 1 < s_signal_table.count) {
        memmove(&s_signal_table.records[index], &s_signal_table.records[index + 1],
                (s_signal_table.count - index - 1) * sizeof(s_signal_table.records[0]));
    }
    s_signal_table.count--;
    memset(&s_signal_table.records[s_signal_table.count], 0, sizeof(s_signal_table.records[s_signal_table.count]));

    esp_err_t err = signal_table_save();
    if (err != ESP_OK) {
        return err;
    }

    err = erase_signal_payload(signal_id);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Deleted signal metadata but failed to erase payload id=%" PRIu32 ": %s", signal_id,
                 esp_err_to_name(err));
    }

    if (s_learning.last_signal_id == signal_id) {
        s_learning.last_signal_id = 0;
    }

    ESP_LOGI(TAG, "Deleted signal id=%" PRIu32, signal_id);
    return ESP_OK;
}
