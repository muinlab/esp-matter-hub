#pragma once

#include <stddef.h>
#include <stdint.h>

#include <driver/rmt.h>
#include <esp_err.h>
#include <esp_matter.h>

typedef enum ir_learning_state {
    IR_LEARNING_IDLE = 0,
    IR_LEARNING_IN_PROGRESS,
    IR_LEARNING_READY,
    IR_LEARNING_FAILED,
} ir_learning_state_t;

typedef struct ir_learning_status {
    ir_learning_state_t state;
    uint32_t elapsed_ms;
    uint32_t timeout_ms;
    uint32_t last_signal_id;
    uint8_t rx_source;
    uint16_t captured_len;
    uint16_t quality_score;
} ir_learning_status_t;

typedef struct ir_signal_record {
    uint32_t signal_id;
    uint32_t carrier_hz;
    uint8_t repeat;
    uint8_t payload_len;
    uint8_t reserved[2];
    uint64_t created_at_ms;
    char name[48];
    char device_type[24];
} ir_signal_record_t;

#define SIGNAL_BUFFER_SIZE 16

typedef struct signal_buffer_entry {
    uint32_t signal_id;
    uint32_t carrier_hz;
    uint8_t repeat;
    rmt_item32_t items[64];
    size_t item_count;
    uint32_t last_used;
    bool valid;
    uint32_t ref_count;
    int64_t last_seen_at;
} signal_buffer_entry_t;

esp_err_t ir_engine_init();
esp_err_t ir_engine_send_signal(uint32_t signal_id, uint8_t slot_id, uint32_t cluster_id, uint32_t attribute_id,
                                const esp_matter_attr_val_t *val);
esp_err_t ir_engine_send_raw(uint32_t signal_id, uint32_t carrier_hz, uint8_t repeat, const uint16_t *ticks,
                              size_t tick_count);
esp_err_t ir_engine_persist_binding(uint8_t slot_id, const char *binding_suffix, uint32_t signal_id,
                                    uint32_t carrier_hz, uint8_t repeat, const uint16_t *ticks, size_t tick_count);
void ir_engine_load_buffer();
const signal_buffer_entry_t *ir_engine_buffer_lookup(uint32_t signal_id);
const signal_buffer_entry_t *ir_engine_buffer_get_all(size_t *count);
void ir_engine_flush_buffer_to_nvs();

esp_err_t ir_engine_start_learning(uint32_t timeout_ms);
esp_err_t ir_engine_commit_learning(const char *name, const char *device_type, uint32_t *out_signal_id);
void ir_engine_get_learning_status(ir_learning_status_t *status);
esp_err_t ir_engine_get_signals(const ir_signal_record_t **signals, size_t *count);
esp_err_t ir_engine_get_signal_payload(uint32_t signal_id, uint16_t *payload_ticks, size_t payload_cap, uint8_t *out_payload_len);
esp_err_t ir_engine_delete_signal(uint32_t signal_id);
