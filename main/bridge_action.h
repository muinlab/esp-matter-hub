#pragma once

#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>
#include <esp_matter.h>

typedef enum button_type {
    BUTTON_TYPE_ONOFF  = 0,
    BUTTON_TYPE_LEVEL  = 1,
    BUTTON_TYPE_ONLYON = 2,
} button_type_t;

typedef struct bridge_slot_state {
    uint8_t slot_id;
    uint16_t endpoint_id;
    button_type_t button_type;
    char display_name[40];
    uint32_t signal_id_a;
    uint32_t signal_id_b;
} bridge_slot_state_t;

esp_err_t bridge_action_init(const uint16_t *endpoint_ids, size_t count);
esp_err_t bridge_action_execute(uint8_t slot_id, uint32_t cluster_id, uint32_t attribute_id, const esp_matter_attr_val_t *val);
esp_err_t bridge_action_configure_slot(uint8_t slot_id, button_type_t type, uint32_t signal_id_a, uint32_t signal_id_b, const char *display_name);
esp_err_t bridge_action_unbind_signal_references(uint32_t signal_id);
esp_err_t bridge_action_sync_all_node_labels();
void bridge_action_log_slot_identity_dump();
const bridge_slot_state_t *bridge_action_get_slots(size_t *count);
const char *bridge_action_button_type_name(button_type_t type);
