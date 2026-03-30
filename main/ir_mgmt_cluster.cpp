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

static constexpr size_t kJsonBufSignals       = 8192;
static constexpr size_t kJsonBufPayload       = 512;
static constexpr size_t kJsonBufSignalPayload = 1024;

static char s_signals_json[kJsonBufSignals];
static char s_signal_payload_json[kJsonBufSignalPayload];
static char s_payload_json[kJsonBufPayload];

// ── JSON serialization ──────────────────────────────────────────────────

static int serialize_signals_json(char *buf, size_t cap)
{
    const ir_signal_record_t *signals = nullptr;
    size_t count = 0;
    if (ir_engine_get_signals(&signals, &count) != ESP_OK) {
        return snprintf(buf, cap, "[]");
    }

    int off = snprintf(buf, cap, "[");
    for (size_t i = 0; i < count && static_cast<size_t>(off) < cap - 2; ++i) {
        off += snprintf(buf + off, cap - off,
                        "%s{\"id\":%lu,\"name\":\"%s\",\"type\":\"%s\",\"carrier\":%lu,\"repeat\":%u,\"len\":%u}",
                        (i == 0) ? "" : ",",
                        static_cast<unsigned long>(signals[i].signal_id),
                        signals[i].name, signals[i].device_type,
                        static_cast<unsigned long>(signals[i].carrier_hz),
                        signals[i].repeat, signals[i].payload_len);
    }
    off += snprintf(buf + off, cap - off, "]");
    return off;
}

static int serialize_learned_payload_json(char *buf, size_t cap)
{
    ir_learning_status_t status;
    ir_engine_get_learning_status(&status);

    return snprintf(buf, cap,
                    "{\"state\":%u,\"elapsed\":%lu,\"timeout\":%lu,"
                    "\"last_id\":%lu,\"rx\":%u,\"len\":%u,\"quality\":%u}",
                    static_cast<unsigned>(status.state),
                    static_cast<unsigned long>(status.elapsed_ms),
                    static_cast<unsigned long>(status.timeout_ms),
                    static_cast<unsigned long>(status.last_signal_id),
                    status.rx_source, status.captured_len, status.quality_score);
}

// ── CacheStatus bitmap ─────────────────────────────────────────────────
// 1 bit per slot; bit=1 if the slot has at least one cached signal
// Each slot has up to 4 signal references (on/off/up/down) — we check via
// bridge_action_get_slots() then ir_engine_buffer_lookup() for each signal_id.

static uint8_t build_cache_status_bitmap()
{
    size_t slot_count = 0;
    const bridge_slot_state_t *slots = bridge_action_get_slots(&slot_count);
    if (!slots || slot_count == 0) return 0;

    uint8_t bitmap = 0;
    for (size_t i = 0; i < slot_count && i < 8; ++i) {
        const bridge_slot_state_t &s = slots[i];
        uint32_t sigs[2] = { s.signal_id_a, s.signal_id_b };
        for (int j = 0; j < 2; ++j) {
            if (sigs[j] != 0 && ir_engine_buffer_lookup(sigs[j]) != nullptr) {
                bitmap |= static_cast<uint8_t>(1u << i);
                break;
            }
        }
    }
    return bitmap;
}

// ── Attribute push refresh ──────────────────────────────────────────────

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

    len = serialize_signals_json(s_signals_json, sizeof(s_signals_json));
    esp_matter_attr_val_t v_signals = esp_matter_long_char_str(s_signals_json, static_cast<uint16_t>(len));
    attribute::set_val(s_ir_mgmt_endpoint_id, IR_MGMT_CLUSTER_ID,
                       IR_MGMT_ATTR_SAVED_SIGNALS_LIST, &v_signals);

    uint8_t cache_bm = build_cache_status_bitmap();
    esp_matter_attr_val_t v_cache = esp_matter_uint8(cache_bm);
    attribute::set_val(s_ir_mgmt_endpoint_id, IR_MGMT_CLUSTER_ID,
                       IR_MGMT_ATTR_CACHE_STATUS, &v_cache);
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

static esp_err_t cmd_save_signal(const ConcreteCommandPath &path, TLVReader &tlv, void *opaque)
{
    char name[48] = {0};
    char device_type[24] = {0};

    chip::TLV::TLVType outer;
    if (tlv_enter_struct(tlv, outer)) {
        while (tlv.Next() == CHIP_NO_ERROR) {
            uint32_t tag = chip::TLV::TagNumFromTag(tlv.GetTag());
            if (tag == 0) { tlv_read_string(tlv, name, sizeof(name)); }
            else if (tag == 1) { tlv_read_string(tlv, device_type, sizeof(device_type)); }
        }
        tlv.ExitContainer(outer);
    }

    uint32_t signal_id = 0;
    esp_err_t err = ir_engine_commit_learning(name, device_type, &signal_id);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "SaveSignal ok signal_id=%lu", static_cast<unsigned long>(signal_id));
        refresh_all_attributes();
    }
    return err;
}

static esp_err_t cmd_delete_signal(const ConcreteCommandPath &path, TLVReader &tlv, void *opaque)
{
    uint32_t signal_id = 0;

    chip::TLV::TLVType outer;
    if (tlv_enter_struct(tlv, outer)) {
        while (tlv.Next() == CHIP_NO_ERROR) {
            if (chip::TLV::TagNumFromTag(tlv.GetTag()) == 0) {
                tlv_read_u32(tlv, signal_id);
            }
        }
        tlv.ExitContainer(outer);
    }

    if (signal_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = bridge_action_unbind_signal_references(signal_id);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "DeleteSignal id=%lu", static_cast<unsigned long>(signal_id));
    err = ir_engine_delete_signal(signal_id);
    if (err == ESP_OK) refresh_all_attributes();
    return err;
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

static esp_err_t cmd_get_signal_payload(const ConcreteCommandPath &path, TLVReader &tlv, void *opaque)
{
    uint32_t signal_id = 0;

    chip::TLV::TLVType outer;
    if (tlv_enter_struct(tlv, outer)) {
        while (tlv.Next() == CHIP_NO_ERROR) {
            if (chip::TLV::TagNumFromTag(tlv.GetTag()) == 0) {
                tlv_read_u32(tlv, signal_id);
            }
        }
        tlv.ExitContainer(outer);
    }

    if (signal_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    uint16_t ticks[128] = { 0 };
    uint8_t tick_len = 0;
    esp_err_t err = ir_engine_get_signal_payload(signal_id, ticks, 128, &tick_len);
    if (err != ESP_OK) {
        s_signal_payload_json[0] = '\0';
        return err;
    }

    const ir_signal_record_t *signals = nullptr;
    size_t count = 0;
    ir_engine_get_signals(&signals, &count);
    const char *name = "";
    uint32_t carrier = 38000;
    uint8_t repeat = 1;
    for (size_t i = 0; i < count; ++i) {
        if (signals[i].signal_id == signal_id) {
            name = signals[i].name;
            carrier = signals[i].carrier_hz;
            repeat = signals[i].repeat;
            break;
        }
    }

    int off = snprintf(s_signal_payload_json, kJsonBufSignalPayload,
                       "{\"id\":%lu,\"name\":\"%s\",\"carrier\":%lu,\"repeat\":%u,\"ticks\":[",
                       static_cast<unsigned long>(signal_id), name,
                       static_cast<unsigned long>(carrier), repeat);
    for (uint8_t i = 0; i < tick_len && static_cast<size_t>(off) < kJsonBufSignalPayload - 10; ++i) {
        off += snprintf(s_signal_payload_json + off, kJsonBufSignalPayload - off,
                        "%s%u", (i == 0) ? "" : ",", ticks[i]);
    }
    off += snprintf(s_signal_payload_json + off, kJsonBufSignalPayload - off, "]}");

    ESP_LOGI(TAG, "GetSignalPayload id=%lu len=%u json=%d bytes",
             static_cast<unsigned long>(signal_id), tick_len, off);

    esp_matter_attr_val_t val = esp_matter_long_char_str(s_signal_payload_json, static_cast<uint16_t>(strlen(s_signal_payload_json)));
    attribute::set_val(s_ir_mgmt_endpoint_id, IR_MGMT_CLUSTER_ID,
                       IR_MGMT_ATTR_SIGNAL_PAYLOAD_DATA, &val);

    return ESP_OK;
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

    size_t tick_count = ticks_bytes_len / 2;
    uint16_t ticks[128];
    if (tick_count > 128) tick_count = 128;
    memcpy(ticks, ticks_bytes, tick_count * 2);

    ESP_LOGI(TAG, "SendSignalWithRaw sig=%lu carrier=%lu repeat=%u ticks=%u",
             static_cast<unsigned long>(signal_id),
             static_cast<unsigned long>(carrier_hz), repeat, (unsigned)tick_count);

    esp_err_t err = ir_engine_send_raw(signal_id, carrier_hz, repeat, ticks, tick_count);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SendSignalWithRaw: send_raw failed: %d", err);
        return err;
    }

    refresh_all_attributes();
    return ESP_OK;
}

static esp_err_t cmd_sync_buffer(const ConcreteCommandPath &path, TLVReader &tlv, void *opaque)
{
    ESP_LOGI(TAG, "SyncBuffer: flushing buffer to NVS");
    ir_engine_flush_buffer_to_nvs();

    // Build JSON snapshot of all valid buffer entries
    size_t count = 0;
    const signal_buffer_entry_t *entries = ir_engine_buffer_get_all(&count);

    char buf[1024];
    int off = 0;
    off += snprintf(buf + off, sizeof(buf) - off, "[");
    bool first = true;
    for (size_t i = 0; i < count; ++i) {
        const signal_buffer_entry_t &e = entries[i];
        if (!e.valid) continue;
        off += snprintf(buf + off, sizeof(buf) - off,
                        "%s{\"signal_id\":%lu,\"carrier_hz\":%lu,\"repeat\":%u,\"item_count\":%u,\"ref_count\":%lu,\"last_seen_at\":%lld}",
                        first ? "" : ",",
                        static_cast<unsigned long>(e.signal_id),
                        static_cast<unsigned long>(e.carrier_hz),
                        e.repeat,
                        static_cast<unsigned>(e.item_count),
                        static_cast<unsigned long>(e.ref_count),
                        static_cast<long long>(e.last_seen_at));
        first = false;
        if (off >= static_cast<int>(sizeof(buf) - 2)) break;
    }
    off += snprintf(buf + off, sizeof(buf) - off, "]");

    esp_matter_attr_val_t val = esp_matter_long_char_str(buf, static_cast<uint16_t>(off));
    esp_matter::attribute::set_val(s_ir_mgmt_endpoint_id, IR_MGMT_CLUSTER_ID, IR_MGMT_ATTR_BUFFER_SNAPSHOT, &val);

    ESP_LOGI(TAG, "SyncBuffer: snapshot written (%d bytes, %zu entries)", off, count);
    refresh_all_attributes();
    return ESP_OK;
}

static esp_err_t cmd_factory_reset(const ConcreteCommandPath &path, TLVReader &tlv, void *opaque)
{
    ESP_LOGW(TAG, "FactoryReset: initiating factory reset via Matter command");
    chip::DeviceLayer::ConfigurationMgr().InitiateFactoryReset();
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

    if (!attribute::create(cl, IR_MGMT_ATTR_LEARNED_PAYLOAD,    ATTRIBUTE_FLAG_NONE, val_empty_obj, static_cast<uint16_t>(kJsonBufPayload)))       { return ESP_FAIL; }
    if (!attribute::create(cl, IR_MGMT_ATTR_SAVED_SIGNALS_LIST, ATTRIBUTE_FLAG_NONE, val_empty_arr, static_cast<uint16_t>(kJsonBufSignals)))        { return ESP_FAIL; }
    if (!attribute::create(cl, IR_MGMT_ATTR_SIGNAL_PAYLOAD_DATA, ATTRIBUTE_FLAG_NONE, val_empty_obj, static_cast<uint16_t>(kJsonBufSignalPayload))) { return ESP_FAIL; }

    esp_matter_attr_val_t val_cache = esp_matter_uint8(0);
    if (!attribute::create(cl, IR_MGMT_ATTR_CACHE_STATUS, ATTRIBUTE_FLAG_NONE, val_cache)) { return ESP_FAIL; }

    if (!attribute::create(cl, IR_MGMT_ATTR_BUFFER_SNAPSHOT, ATTRIBUTE_FLAG_NONE, val_empty_arr, 1024)) { return ESP_FAIL; }

    // ── Commands (all custom + accepted) ──

    constexpr uint8_t kCmdFlags = COMMAND_FLAG_ACCEPTED | COMMAND_FLAG_CUSTOM;

    if (!command::create(cl, IR_MGMT_CMD_START_LEARNING,       kCmdFlags, cmd_start_learning))      { return ESP_FAIL; }
    if (!command::create(cl, IR_MGMT_CMD_CANCEL_LEARNING,      kCmdFlags, cmd_cancel_learning))     { return ESP_FAIL; }
    if (!command::create(cl, IR_MGMT_CMD_SAVE_SIGNAL,          kCmdFlags, cmd_save_signal))         { return ESP_FAIL; }
    if (!command::create(cl, IR_MGMT_CMD_DELETE_SIGNAL,        kCmdFlags, cmd_delete_signal))       { return ESP_FAIL; }
    if (!command::create(cl, IR_MGMT_CMD_OPEN_COMMISSIONING,   kCmdFlags, cmd_open_commissioning))  { return ESP_FAIL; }
    if (!command::create(cl, IR_MGMT_CMD_GET_SIGNAL_PAYLOAD,   kCmdFlags, cmd_get_signal_payload))  { return ESP_FAIL; }
    if (!command::create(cl, IR_MGMT_CMD_SEND_SIGNAL_WITH_RAW, kCmdFlags, cmd_send_signal_with_raw)) { return ESP_FAIL; }
    if (!command::create(cl, IR_MGMT_CMD_SYNC_BUFFER,          kCmdFlags, cmd_sync_buffer))          { return ESP_FAIL; }
    if (!command::create(cl, IR_MGMT_CMD_FACTORY_RESET,       kCmdFlags, cmd_factory_reset))       { return ESP_FAIL; }

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
