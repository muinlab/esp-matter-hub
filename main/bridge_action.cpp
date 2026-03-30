#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <esp_log.h>
#include <nvs.h>

#include "app_priv.h"
#include "bridge_action.h"
#include "ir_engine.h"

static const char *TAG = "bridge_action";
static const char *kNvsNamespace = "bridge_map";
static constexpr uint32_t kClusterOnOff = 0x0006;
static constexpr uint32_t kClusterLevelControl = 0x0008;
static constexpr uint32_t kClusterBasicInformation = 0x0028;
static constexpr uint32_t kClusterBridgedDeviceBasicInformation = 0x0039;
static constexpr uint32_t kAttributeOnOff = 0x0000;
static constexpr uint32_t kAttributeCurrentLevel = 0x0000;
static constexpr uint32_t kAttributeNodeLabel = 0x0005;
static constexpr uint32_t kAttributeUniqueID = 0x0012;
static constexpr size_t kMaxNodeLabelLength = 32;

static bridge_slot_state_t s_slots[BRIDGE_SLOT_COUNT];
static uint8_t s_last_level[BRIDGE_SLOT_COUNT];
static bool s_last_level_valid[BRIDGE_SLOT_COUNT];
static size_t s_slot_count = 0;

// ── Helpers ──────────────────────────────────────────────────

static bool read_char_attribute(uint16_t endpoint_id, uint32_t cluster_id, uint32_t attribute_id, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    esp_matter_attr_val_t val = {};
    if (esp_matter::attribute::get_val(endpoint_id, cluster_id, attribute_id, &val) != ESP_OK) {
        return false;
    }
    if (val.type != ESP_MATTER_VAL_TYPE_CHAR_STRING && val.type != ESP_MATTER_VAL_TYPE_LONG_CHAR_STRING) {
        return false;
    }
    if (!val.val.a.b || val.val.a.s == 0) {
        return false;
    }

    size_t copy_len = val.val.a.s;
    if (copy_len >= out_size) {
        copy_len = out_size - 1;
    }
    memcpy(out, val.val.a.b, copy_len);
    out[copy_len] = '\0';
    return true;
}

static esp_err_t write_node_label_with_retry(uint16_t endpoint_id, uint32_t cluster_id, esp_matter_attr_val_t *label_val)
{
    static constexpr uint8_t kMaxAttempts = 6;
    for (uint8_t attempt = 1; attempt <= kMaxAttempts; ++attempt) {
        esp_err_t err = esp_matter::attribute::set_val(endpoint_id, cluster_id, kAttributeNodeLabel, label_val);
        if (err == ESP_OK || err == ESP_ERR_NOT_FINISHED) {
            if (esp_matter::is_started()) {
                (void)esp_matter::attribute::report(endpoint_id, cluster_id, kAttributeNodeLabel, label_val);
            }
            return ESP_OK;
        }
        if (err != ESP_ERR_NO_MEM || attempt == kMaxAttempts) {
            return err;
        }
        vTaskDelay(pdMS_TO_TICKS(40));
    }
    return ESP_ERR_NO_MEM;
}

const char *bridge_action_button_type_name(button_type_t type)
{
    switch (type) {
    case BUTTON_TYPE_ONOFF:  return "ONOFF";
    case BUTTON_TYPE_LEVEL:  return "LEVEL";
    case BUTTON_TYPE_ONLYON: return "ONLYON";
    default:                 return "UNKNOWN";
    }
}

// ── NodeLabel sync ───────────────────────────────────────────

static esp_err_t sync_slot_node_label(uint8_t slot_id)
{
    if (slot_id >= s_slot_count) {
        return ESP_ERR_INVALID_ARG;
    }

    const bridge_slot_state_t *slot = &s_slots[slot_id];
    uint16_t endpoint_id = slot->endpoint_id;
    size_t label_len = strnlen(slot->display_name, sizeof(slot->display_name));
    if (label_len > kMaxNodeLabelLength) {
        label_len = kMaxNodeLabelLength;
    }
    esp_matter_attr_val_t label_val = esp_matter_char_str(const_cast<char *>(slot->display_name), static_cast<uint16_t>(label_len));

    esp_matter::attribute_t *attr =
        esp_matter::attribute::get(endpoint_id, kClusterBridgedDeviceBasicInformation, kAttributeNodeLabel);
    if (attr) {
        char before_label[48] = { 0 };
        bool has_before = read_char_attribute(endpoint_id, kClusterBridgedDeviceBasicInformation, kAttributeNodeLabel,
                                              before_label, sizeof(before_label));
        if (has_before && strcmp(before_label, slot->display_name) == 0) {
            return ESP_OK;
        }

        esp_err_t err = write_node_label_with_retry(endpoint_id, kClusterBridgedDeviceBasicInformation, &label_val);
        if (err == ESP_ERR_NO_MEM) {
            ESP_LOGW(TAG, "NodeLabel write OOM slot=%u endpoint=%u", static_cast<unsigned>(slot_id),
                     static_cast<unsigned>(endpoint_id));
        }
        return err;
    }

    attr = esp_matter::attribute::get(endpoint_id, kClusterBasicInformation, kAttributeNodeLabel);
    if (attr) {
        char before_label[48] = { 0 };
        bool has_before = read_char_attribute(endpoint_id, kClusterBasicInformation, kAttributeNodeLabel, before_label,
                                              sizeof(before_label));
        if (has_before && strcmp(before_label, slot->display_name) == 0) {
            return ESP_OK;
        }

        esp_err_t err = write_node_label_with_retry(endpoint_id, kClusterBasicInformation, &label_val);
        if (err == ESP_ERR_NO_MEM) {
            ESP_LOGW(TAG, "NodeLabel write OOM slot=%u endpoint=%u", static_cast<unsigned>(slot_id),
                     static_cast<unsigned>(endpoint_id));
        }
        return err;
    }

    ESP_LOGW(TAG, "NodeLabel cluster missing for slot=%u endpoint=%u", static_cast<unsigned>(slot_id),
             static_cast<unsigned>(endpoint_id));
    return ESP_ERR_NOT_FOUND;
}

// ── NVS slot config persistence ──────────────────────────────

static esp_err_t save_slot_config(uint8_t slot_id)
{
    if (slot_id >= s_slot_count) {
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    const bridge_slot_state_t *slot = &s_slots[slot_id];
    char key[20];

    snprintf(key, sizeof(key), "s%u_type", slot_id);
    err = nvs_set_u8(handle, key, static_cast<uint8_t>(slot->button_type));

    if (err == ESP_OK) {
        snprintf(key, sizeof(key), "s%u_a", slot_id);
        err = nvs_set_u32(handle, key, slot->signal_id_a);
    }
    if (err == ESP_OK) {
        snprintf(key, sizeof(key), "s%u_b", slot_id);
        err = nvs_set_u32(handle, key, slot->signal_id_b);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    return err;
}

static void load_slot_config()
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return;
    }

    for (size_t i = 0; i < s_slot_count; ++i) {
        char key[20];
        uint8_t type_val = 0;
        uint32_t sig_a = 0, sig_b = 0;

        snprintf(key, sizeof(key), "s%u_type", static_cast<unsigned>(i));
        if (nvs_get_u8(handle, key, &type_val) == ESP_OK && type_val <= BUTTON_TYPE_ONLYON) {
            s_slots[i].button_type = static_cast<button_type_t>(type_val);
        }

        snprintf(key, sizeof(key), "s%u_a", static_cast<unsigned>(i));
        if (nvs_get_u32(handle, key, &sig_a) == ESP_OK) {
            s_slots[i].signal_id_a = sig_a;
        }

        snprintf(key, sizeof(key), "s%u_b", static_cast<unsigned>(i));
        if (nvs_get_u32(handle, key, &sig_b) == ESP_OK) {
            s_slots[i].signal_id_b = sig_b;
        }
    }

    nvs_close(handle);
}

static void erase_legacy_registry()
{
    nvs_handle_t handle;
    if (nvs_open(kNvsNamespace, NVS_READWRITE, &handle) != ESP_OK) {
        return;
    }
    if (nvs_erase_key(handle, "registry_v1") == ESP_OK) {
        nvs_commit(handle);
        ESP_LOGI(TAG, "Erased legacy registry_v1 from NVS");
    }
    nvs_close(handle);
}

// ── Init ─────────────────────────────────────────────────────

static void set_default_slot_display_name(bridge_slot_state_t *slot)
{
    if (!slot) {
        return;
    }
    snprintf(slot->display_name, sizeof(slot->display_name), "Slot %u", static_cast<unsigned>(slot->slot_id));
}

esp_err_t bridge_action_init(const uint16_t *endpoint_ids, size_t count)
{
    if (!endpoint_ids || count == 0 || count > BRIDGE_SLOT_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    memset(s_slots, 0, sizeof(s_slots));
    memset(s_last_level, 0, sizeof(s_last_level));
    memset(s_last_level_valid, 0, sizeof(s_last_level_valid));
    s_slot_count = count;
    for (size_t i = 0; i < count; ++i) {
        s_slots[i].slot_id = static_cast<uint8_t>(i);
        s_slots[i].endpoint_id = endpoint_ids[i];
        s_slots[i].button_type = BUTTON_TYPE_ONOFF;
        set_default_slot_display_name(&s_slots[i]);
    }

    load_slot_config();
    erase_legacy_registry();

    for (size_t i = 0; i < count; ++i) {
        esp_err_t sync_err = sync_slot_node_label(static_cast<uint8_t>(i));
        if (sync_err != ESP_OK && sync_err != ESP_ERR_NOT_FOUND) {
            ESP_LOGW(TAG, "NodeLabel sync failed slot=%u: %s", static_cast<unsigned>(i), esp_err_to_name(sync_err));
        }
    }

    ESP_LOGI(TAG, "Initialized bridge actions: slots=%u", static_cast<unsigned>(s_slot_count));
    return ESP_OK;
}

// ── Slot config ──────────────────────────────────────────────

esp_err_t bridge_action_configure_slot(uint8_t slot_id, button_type_t type, uint32_t signal_id_a, uint32_t signal_id_b,
                                       const char *display_name)
{
    if (slot_id >= s_slot_count || type > BUTTON_TYPE_ONLYON) {
        return ESP_ERR_INVALID_ARG;
    }

    bridge_slot_state_t *slot = &s_slots[slot_id];
    slot->button_type = type;
    slot->signal_id_a = signal_id_a;
    slot->signal_id_b = signal_id_b;
    if (display_name && display_name[0]) {
        strlcpy(slot->display_name, display_name, sizeof(slot->display_name));
    }

    esp_err_t err = save_slot_config(slot_id);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save slot config slot=%u: %s", slot_id, esp_err_to_name(err));
        return err;
    }

    sync_slot_node_label(slot_id);

    ESP_LOGI(TAG, "Configured slot=%u type=%s sig_a=%" PRIu32 " sig_b=%" PRIu32 " name='%s'",
             slot_id, bridge_action_button_type_name(type), signal_id_a, signal_id_b, slot->display_name);
    return ESP_OK;
}

// ── Unbind signal references ─────────────────────────────────

esp_err_t bridge_action_unbind_signal_references(uint32_t signal_id)
{
    if (signal_id == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    bool changed = false;
    for (size_t i = 0; i < s_slot_count; ++i) {
        bridge_slot_state_t *slot = &s_slots[i];
        bool slot_changed = false;
        if (slot->signal_id_a == signal_id) {
            slot->signal_id_a = 0;
            slot_changed = true;
        }
        if (slot->signal_id_b == signal_id) {
            slot->signal_id_b = 0;
            slot_changed = true;
        }
        if (slot_changed) {
            save_slot_config(static_cast<uint8_t>(i));
            changed = true;
        }
    }

    if (changed) {
        ESP_LOGI(TAG, "Unbound signal references signal_id=%" PRIu32, signal_id);
    }
    return ESP_OK;
}

// ── Execute (Matter EP → IR) ─────────────────────────────────

esp_err_t bridge_action_execute(uint8_t slot_id, uint32_t cluster_id, uint32_t attribute_id, const esp_matter_attr_val_t *val)
{
    if (slot_id >= s_slot_count || !val) {
        return ESP_ERR_INVALID_ARG;
    }

    bridge_slot_state_t *slot = &s_slots[slot_id];
    if (slot->signal_id_a == 0 && slot->signal_id_b == 0) {
        ESP_LOGW(TAG, "Slot %u has no signals configured", slot_id);
        return ESP_OK;
    }

    uint32_t signal_id = 0;

    switch (slot->button_type) {
    case BUTTON_TYPE_ONOFF:
        if (cluster_id == kClusterOnOff && attribute_id == kAttributeOnOff) {
            signal_id = val->val.b ? slot->signal_id_a : slot->signal_id_b;
        } else if (cluster_id == kClusterLevelControl && attribute_id == kAttributeCurrentLevel) {
            signal_id = (val->val.u8 > 0) ? slot->signal_id_a : slot->signal_id_b;
        }
        break;

    case BUTTON_TYPE_LEVEL:
        if (cluster_id == kClusterOnOff && attribute_id == kAttributeOnOff) {
            // 2-way: on→signal_a, off→signal_b
            signal_id = val->val.b ? slot->signal_id_a : slot->signal_id_b;
        } else if (cluster_id == kClusterLevelControl && attribute_id == kAttributeCurrentLevel) {
            const uint8_t new_level = val->val.u8;
            bool has_prev = s_last_level_valid[slot_id];
            uint8_t prev_level = has_prev ? s_last_level[slot_id] : new_level;

            if (new_level == 0) {
                signal_id = slot->signal_id_b;      // off/down
            } else if (!has_prev || prev_level == 0) {
                signal_id = slot->signal_id_a;      // on/up
            } else if (new_level > prev_level) {
                signal_id = slot->signal_id_a;      // up
            } else if (new_level < prev_level) {
                signal_id = slot->signal_id_b;      // down
            }

            s_last_level[slot_id] = new_level;
            s_last_level_valid[slot_id] = true;
        }
        break;

    case BUTTON_TYPE_ONLYON:
        // Always send signal_a regardless of on/off
        signal_id = slot->signal_id_a;
        break;

    default:
        break;
    }

    if (signal_id != 0) {
        ir_engine_send_signal(signal_id, slot_id, cluster_id, attribute_id, val);

        if (cluster_id == kClusterOnOff && attribute_id == kAttributeOnOff) {
            s_last_level[slot_id] = val->val.b ? 1 : 0;
            s_last_level_valid[slot_id] = true;
        }
    }

    return ESP_OK;
}

// ── Queries ──────────────────────────────────────────────────

esp_err_t bridge_action_sync_all_node_labels()
{
    esp_err_t last_err = ESP_OK;
    for (size_t i = 0; i < s_slot_count; ++i) {
        esp_err_t err = sync_slot_node_label(static_cast<uint8_t>(i));
        if (err != ESP_OK && err != ESP_ERR_NOT_FOUND) {
            last_err = err;
            ESP_LOGW(TAG, "NodeLabel sync failed for slot %u: %s", static_cast<unsigned>(i), esp_err_to_name(err));
        }
    }
    return last_err;
}

void bridge_action_log_slot_identity_dump()
{
    ESP_LOGI(TAG, "---- Slot identity dump start ----");
    for (size_t i = 0; i < s_slot_count; ++i) {
        const bridge_slot_state_t *slot = &s_slots[i];
        char node_label[48] = { 0 };
        char unique_id[48] = { 0 };

        bool has_node_label = read_char_attribute(slot->endpoint_id, kClusterBridgedDeviceBasicInformation, kAttributeNodeLabel,
                                                  node_label, sizeof(node_label));
        bool has_unique_id = read_char_attribute(slot->endpoint_id, kClusterBridgedDeviceBasicInformation, kAttributeUniqueID,
                                                 unique_id, sizeof(unique_id));

        ESP_LOGI(TAG,
                 "slot=%u endpoint=%u type=%s sig_a=%" PRIu32 " sig_b=%" PRIu32 " name='%s' node_label='%s' unique_id='%s'",
                 static_cast<unsigned>(slot->slot_id), static_cast<unsigned>(slot->endpoint_id),
                 bridge_action_button_type_name(slot->button_type),
                 slot->signal_id_a, slot->signal_id_b,
                 slot->display_name,
                 has_node_label ? node_label : "(missing)", has_unique_id ? unique_id : "(missing)");
    }
    ESP_LOGI(TAG, "---- Slot identity dump end ----");
}

const bridge_slot_state_t *bridge_action_get_slots(size_t *count)
{
    if (count) {
        *count = s_slot_count;
    }
    return s_slots;
}
