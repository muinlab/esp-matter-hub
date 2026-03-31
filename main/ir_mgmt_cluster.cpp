#include <cstdio>
#include <cstring>

#include <esp_err.h>
#include <esp_log.h>
#include <esp_matter.h>
#include <platform/CHIPDeviceLayer.h>

#include "ir_mgmt_cluster.h"
#include "ir_engine.h"
#include "bridge_action.h"
#include "app_priv.h"
#include "activity_log.h"
#include "test_signals.h"

static const char *TAG = "ir_mgmt";

using namespace esp_matter;

static uint16_t s_ir_mgmt_endpoint_id = 0;

extern "C" esp_err_t app_open_commissioning_window(uint16_t timeout_seconds);

// ── bridge_action functions implemented by worker-3 ────────────────────
extern esp_err_t bridge_action_auto_register_and_bind(uint32_t device_id, uint32_t signal_id,
                                                       bool is_level, bool high_low, uint8_t *out_slot_id);
extern esp_err_t bridge_action_check_device_type(uint32_t device_id, bool is_level);
extern int8_t    bridge_action_get_slot_for_device(uint32_t device_id);

// ── JSON buffer sizes (single-threaded Matter stack → static is safe) ───

static constexpr size_t kJsonBufPayload = 1280;

static char s_payload_json[kJsonBufPayload];

// ── JSON serialization ──────────────────────────────────────────────────

static int serialize_learned_payload_json(char *buf, size_t cap)
{
    ir_learning_status_t status;
    ir_engine_get_learning_status(&status);

    int off = snprintf(buf, cap,
                       "{\"state\":%u,\"elapsed\":%lu,\"timeout\":%lu,"
                       "\"last_id\":%lu,\"rx\":%u,\"len\":%u,\"quality\":%u",
                       static_cast<unsigned>(status.state),
                       static_cast<unsigned long>(status.elapsed_ms),
                       static_cast<unsigned long>(status.timeout_ms),
                       static_cast<unsigned long>(status.last_signal_id),
                       status.rx_source, status.captured_len, status.quality_score);

    // Include ticks data when learning is READY
    if (status.state == IR_LEARNING_READY) {
        uint8_t tick_len = 0;
        uint32_t carrier = 0;
        const uint16_t *ticks = ir_engine_get_learned_ticks(&tick_len, &carrier);
        if (ticks && tick_len > 0) {
            off += snprintf(buf + off, cap - off, ",\"carrier\":%lu,\"ticks\":\"",
                            static_cast<unsigned long>(carrier));
            for (uint8_t i = 0; i < tick_len && off < (int)(cap - 6); ++i) {
                // LE byte order — matches SendSignalWithRaw BYTES format
                off += snprintf(buf + off, cap - off, "%02X%02X",
                                ticks[i] & 0xFF, (ticks[i] >> 8) & 0xFF);
            }
            off += snprintf(buf + off, cap - off, "\"");
        }
    }

    off += snprintf(buf + off, cap - off, "}");
    return off;
}

// ── Attribute push refresh ──────────────────────────────────────────────

static void refresh_all_attributes();

void ir_mgmt_refresh_attributes(void)
{
    refresh_all_attributes();
}

static void refresh_all_attributes()
{
    if (s_ir_mgmt_endpoint_id == 0) return;

    ir_learning_status_t st;
    ir_engine_get_learning_status(&st);
    esp_matter_attr_val_t v_u8 = esp_matter_enum8(static_cast<uint8_t>(st.state));
    attribute::set_val(s_ir_mgmt_endpoint_id, IR_MGMT_CLUSTER_ID,
                       IR_MGMT_ATTR_LEARN_STATE, &v_u8);

    int len = serialize_learned_payload_json(s_payload_json, sizeof(s_payload_json));
    esp_matter_attr_val_t v_payload = esp_matter_long_char_str(s_payload_json, static_cast<uint16_t>(len));
    attribute::set_val(s_ir_mgmt_endpoint_id, IR_MGMT_CLUSTER_ID,
                       IR_MGMT_ATTR_LEARNED_PAYLOAD, &v_payload);
}

// ── TLV helpers ─────────────────────────────────────────────────────────

static bool tlv_read_u8(TLVReader &reader, uint8_t &out)
{
    return reader.Get(out) == CHIP_NO_ERROR;
}

static bool tlv_read_u16(TLVReader &reader, uint16_t &out)
{
    return reader.Get(out) == CHIP_NO_ERROR;
}

static bool tlv_read_u32(TLVReader &reader, uint32_t &out)
{
    return reader.Get(out) == CHIP_NO_ERROR;
}

static bool tlv_read_bool(TLVReader &reader, bool &out)
{
    return reader.Get(out) == CHIP_NO_ERROR;
}

static bool tlv_read_string(TLVReader &reader, char *buf, size_t cap)
{
    if (reader.GetLength() >= static_cast<uint32_t>(cap)) {
        return false;
    }
    return reader.GetString(buf, static_cast<uint32_t>(cap)) == CHIP_NO_ERROR;
}

static bool tlv_enter_struct(TLVReader &reader, chip::TLV::TLVType &outer)
{
    return reader.GetType() == chip::TLV::kTLVType_Structure &&
           reader.EnterContainer(outer) == CHIP_NO_ERROR;
}

// ── Command handlers ────────────────────────────────────────────────────

static esp_err_t cmd_start_learning(const ConcreteCommandPath &path, TLVReader &tlv, void *opaque)
{
    uint32_t timeout_ms = 15000;

    chip::TLV::TLVType outer;
    if (tlv_enter_struct(tlv, outer)) {
        while (tlv.Next() == CHIP_NO_ERROR) {
            if (chip::TLV::TagNumFromTag(tlv.GetTag()) == 0) {
                tlv_read_u32(tlv, timeout_ms);
            }
        }
        tlv.ExitContainer(outer);
    }

    ESP_LOGI(TAG, "StartLearning timeout_ms=%lu", static_cast<unsigned long>(timeout_ms));
    esp_err_t err = ir_engine_start_learning(timeout_ms);
    if (err == ESP_OK) refresh_all_attributes();
    return err;
}

static esp_err_t cmd_cancel_learning(const ConcreteCommandPath &path, TLVReader &tlv, void *opaque)
{
    // ir_engine has no explicit cancel API; learning auto-expires via timeout
    ESP_LOGW(TAG, "CancelLearning: not supported, learning will auto-expire");
    return ESP_ERR_NOT_SUPPORTED;
}

static esp_err_t cmd_open_commissioning(const ConcreteCommandPath &path, TLVReader &tlv, void *opaque)
{
    uint16_t timeout_s = 300;

    chip::TLV::TLVType outer;
    if (tlv_enter_struct(tlv, outer)) {
        while (tlv.Next() == CHIP_NO_ERROR) {
            if (chip::TLV::TagNumFromTag(tlv.GetTag()) == 0) {
                tlv_read_u16(tlv, timeout_s);
            }
        }
        tlv.ExitContainer(outer);
    }

    ESP_LOGI(TAG, "OpenCommissioningWindow timeout_s=%u", timeout_s);
    return app_open_commissioning_window(timeout_s);
}

static esp_err_t cmd_send_signal_with_raw(const ConcreteCommandPath &path, TLVReader &tlv, void *opaque)
{
    uint32_t signal_id  = 0;
    uint32_t carrier_hz = 38000;
    uint8_t  repeat     = 1;
    const uint8_t *ticks_bytes = nullptr;
    uint32_t       ticks_bytes_len = 0;

    chip::TLV::TLVType outer;
    if (tlv_enter_struct(tlv, outer)) {
        while (tlv.Next() == CHIP_NO_ERROR) {
            uint32_t tag = chip::TLV::TagNumFromTag(tlv.GetTag());
            switch (tag) {
            case 0: tlv_read_u32(tlv, signal_id);   break;
            case 1: tlv_read_u32(tlv, carrier_hz);  break;
            case 2: tlv_read_u8(tlv, repeat);       break;
            case 3:
                ticks_bytes_len = tlv.GetLength();
                tlv.GetDataPtr(ticks_bytes);
                break;
            default:
                break;
            }
        }
        tlv.ExitContainer(outer);
    }

    if (ticks_bytes == nullptr || ticks_bytes_len == 0) {
        ESP_LOGW(TAG, "SendSignalWithRaw: empty ticks");
        return ESP_ERR_INVALID_ARG;
    }
    if ((ticks_bytes_len % 2) != 0) {
        ESP_LOGW(TAG, "SendSignalWithRaw: odd ticks byte count=%lu",
                 static_cast<unsigned long>(ticks_bytes_len));
        return ESP_ERR_INVALID_ARG;
    }

    // chip-tool BYTES sends hex chars as raw ASCII bytes (not hex-decoded)
    // Detect and decode: if bytes look like ASCII hex (all 0-9/A-F/a-f), decode pairs to bytes first
    uint8_t decoded[256];
    size_t decoded_len = ticks_bytes_len;
    const uint8_t *tick_src = ticks_bytes;

    bool is_ascii_hex = (ticks_bytes_len > 0) && (ticks_bytes_len % 2 == 0);
    if (is_ascii_hex) {
        for (size_t i = 0; i < ticks_bytes_len && is_ascii_hex; ++i) {
            uint8_t c = ticks_bytes[i];
            if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))) {
                is_ascii_hex = false;
            }
        }
    }

    if (is_ascii_hex && ticks_bytes_len > 0) {
        decoded_len = 0;
        for (size_t i = 0; i + 1 < ticks_bytes_len && decoded_len < sizeof(decoded); i += 2) {
            auto hex_val = [](uint8_t c) -> uint8_t {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'A' && c <= 'F') return c - 'A' + 10;
                if (c >= 'a' && c <= 'f') return c - 'a' + 10;
                return 0;
            };
            decoded[decoded_len++] = (hex_val(ticks_bytes[i]) << 4) | hex_val(ticks_bytes[i + 1]);
        }
        tick_src = decoded;
    }

    if ((decoded_len % 2) != 0) {
        ESP_LOGW(TAG, "SendSignalWithRaw: odd decoded byte count=%lu", static_cast<unsigned long>(decoded_len));
        return ESP_ERR_INVALID_ARG;
    }

    size_t tick_count = decoded_len / 2;
    uint16_t ticks[128];
    if (tick_count > 128) tick_count = 128;
    memcpy(ticks, tick_src, tick_count * 2);

    ESP_LOGI(TAG, "SendSignalWithRaw sig=%lu carrier=%lu repeat=%u ticks=%u (raw_bytes=%lu decoded=%lu)",
             static_cast<unsigned long>(signal_id),
             static_cast<unsigned long>(carrier_hz), repeat, (unsigned)tick_count,
             static_cast<unsigned long>(ticks_bytes_len), static_cast<unsigned long>(decoded_len));

    esp_err_t err = ir_engine_send_raw(signal_id, carrier_hz, repeat, ticks, tick_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SendSignalWithRaw: send_raw failed: %d", err);
        return err;
    }

    signal_buffer_insert(signal_id, carrier_hz, repeat, ticks, tick_count);
    char act_detail[48];
    snprintf(act_detail, sizeof(act_detail), "{\"sig\":%lu,\"ticks\":%u}",
             static_cast<unsigned long>(signal_id), (unsigned)tick_count);
    (void)activity_log_append(ACT_SEND_SIGNAL_RAW, act_detail);

    refresh_all_attributes();
    return ESP_OK;
}

static esp_err_t cmd_dump_nvs(const ConcreteCommandPath &path, TLVReader &tlv, void *opaque);

static esp_err_t cmd_sync_buffer(const ConcreteCommandPath &path, TLVReader &tlv, void *opaque)
{
    // SyncBuffer is now an alias for DumpNVS — no RAM buffer exists
    ESP_LOGI(TAG, "SyncBuffer: reading all NVS signals (alias for DumpNVS)");
    return cmd_dump_nvs(path, tlv, opaque);
}

static esp_err_t cmd_factory_reset(const ConcreteCommandPath &path, TLVReader &tlv, void *opaque)
{
    ESP_LOGW(TAG, "FactoryReset: initiating factory reset via Matter command");
    (void)activity_log_append(ACT_FACTORY_RESET, "{}");
    chip::DeviceLayer::ConfigurationMgr().InitiateFactoryReset();
    return ESP_OK;
}

static esp_err_t cmd_dump_nvs(const ConcreteCommandPath &path, TLVReader &tlv, void *opaque)
{
    ESP_LOGI(TAG, "DumpNVS: reading all NVS signals");

    // Read all NVS signals into BufferSnapshot attribute
    char buf[2048];
    int len = ir_engine_read_all_nvs_signals(buf, sizeof(buf));

    esp_matter_attr_val_t val = esp_matter_long_char_str(buf, static_cast<uint16_t>(len));
    esp_matter::attribute::set_val(s_ir_mgmt_endpoint_id, IR_MGMT_CLUSTER_ID, IR_MGMT_ATTR_BUFFER_SNAPSHOT, &val);

    ESP_LOGI(TAG, "DumpNVS: wrote %d bytes to BufferSnapshot", len);
    (void)activity_log_append(ACT_DUMP_NVS, "{}");
    refresh_all_attributes();
    return ESP_OK;
}

// ── Cluster init ────────────────────────────────────────────────────────

esp_err_t ir_mgmt_cluster_init(esp_matter::node_t *node)
{
    if (!node) {
        return ESP_ERR_INVALID_ARG;
    }

    endpoint_t *ep = endpoint::create(node, ENDPOINT_FLAG_NONE, nullptr);
    if (!ep) {
        ESP_LOGE(TAG, "Failed to create endpoint");
        return ESP_FAIL;
    }
    s_ir_mgmt_endpoint_id = endpoint::get_id(ep);

    cluster::descriptor::config_t desc_cfg;
    cluster_t *desc_cl = cluster::descriptor::create(ep, &desc_cfg, CLUSTER_FLAG_SERVER);
    if (!desc_cl) {
        ESP_LOGE(TAG, "Failed to create descriptor cluster");
        return ESP_FAIL;
    }

    static constexpr uint32_t kIrMgmtDeviceTypeId = 0xFFF10001;
    esp_err_t dt_err = endpoint::add_device_type(ep, kIrMgmtDeviceTypeId, 1);
    if (dt_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to add device type: %d", dt_err);
        return dt_err;
    }

    cluster_t *cl = cluster::create(ep, IR_MGMT_CLUSTER_ID, CLUSTER_FLAG_SERVER);
    if (!cl) {
        ESP_LOGE(TAG, "Failed to create cluster");
        return ESP_FAIL;
    }

    // ── Attributes ──

    esp_matter_attr_val_t val_u8 = esp_matter_enum8(0);
    if (!attribute::create(cl, IR_MGMT_ATTR_LEARN_STATE, ATTRIBUTE_FLAG_NONE, val_u8)) { return ESP_FAIL; }

    esp_matter_attr_val_t val_empty_obj = esp_matter_long_char_str(const_cast<char *>("{}"), 2);
    esp_matter_attr_val_t val_empty_arr = esp_matter_long_char_str(const_cast<char *>("[]"), 2);

    if (!attribute::create(cl, IR_MGMT_ATTR_LEARNED_PAYLOAD,    ATTRIBUTE_FLAG_NONE, val_empty_obj, 1280))       { return ESP_FAIL; }

    if (!attribute::create(cl, IR_MGMT_ATTR_BUFFER_SNAPSHOT, ATTRIBUTE_FLAG_NONE, val_empty_arr, 2048)) { return ESP_FAIL; }

    // ── Commands (all custom + accepted) ──

    constexpr uint8_t kCmdFlags = COMMAND_FLAG_ACCEPTED | COMMAND_FLAG_CUSTOM;

    if (!command::create(cl, IR_MGMT_CMD_START_LEARNING,       kCmdFlags, cmd_start_learning))      { return ESP_FAIL; }
    if (!command::create(cl, IR_MGMT_CMD_CANCEL_LEARNING,      kCmdFlags, cmd_cancel_learning))     { return ESP_FAIL; }
    // SaveSignal(0x02) removed in v3.2.1
    if (!command::create(cl, IR_MGMT_CMD_OPEN_COMMISSIONING,   kCmdFlags, cmd_open_commissioning))  { return ESP_FAIL; }
    if (!command::create(cl, IR_MGMT_CMD_SEND_SIGNAL_WITH_RAW, kCmdFlags, cmd_send_signal_with_raw)) { return ESP_FAIL; }
    if (!command::create(cl, IR_MGMT_CMD_SYNC_BUFFER,          kCmdFlags, cmd_sync_buffer))          { return ESP_FAIL; }
    if (!command::create(cl, IR_MGMT_CMD_FACTORY_RESET,       kCmdFlags, cmd_factory_reset))       { return ESP_FAIL; }
    if (!command::create(cl, IR_MGMT_CMD_DUMP_NVS,            kCmdFlags, cmd_dump_nvs))            { return ESP_FAIL; }

    // ── Events ──

    if (!event::create(cl, IR_MGMT_EVT_LEARNING_COMPLETED)) { return ESP_FAIL; }

    ESP_LOGI(TAG, "Cluster 0x%08lX on endpoint %u: 5 attrs, 7 cmds, 1 event",
             static_cast<unsigned long>(IR_MGMT_CLUSTER_ID), s_ir_mgmt_endpoint_id);

    ESP_LOGI(TAG, "SyncBuffer command (0x0C) registered, BufferSnapshot attribute (0x0007) created");
    return ESP_OK;
}

uint16_t ir_mgmt_cluster_get_endpoint_id(void)
{
    return s_ir_mgmt_endpoint_id;
}
