#pragma once

#include <stdint.h>
#include <stddef.h>
#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

// ── Signal Buffer (RAM-only, max 16 LRU entries) ─────────────────────────────

struct signal_buffer_entry_t {
    uint32_t signal_id;
    uint32_t carrier_hz;
    uint8_t  repeat;
    uint16_t tick_count;
    char     ticks_hex[520];  // 128*4=512 + null + safety margin
};

// Insert or move-to-front if signal_id already exists. Evicts LRU if full.
void signal_buffer_insert(uint32_t signal_id, uint32_t carrier_hz, uint8_t repeat,
                          const uint16_t *ticks, size_t tick_count);

// Returns number of entries currently in buffer (0..16).
int signal_buffer_get_count();

// Writes single JSON object for entry at index (0=most recent) to out.
// JSON: {"signal_id":N,"carrier_hz":N,"repeat":N,"tick_count":N,"ticks_hex":"...first40chars..."}
// Returns ESP_ERR_INVALID_ARG if index out of range.
esp_err_t signal_buffer_read_entry_json(int index, char *out, size_t out_size);

// ── Saved Test Signals (NVS-backed, max 16 entries) ──────────────────────────

// Reads count from NVS, creates shared mutex. Call once at startup.
esp_err_t test_signals_init();

// Returns number of saved test signals (0..16).
int test_signals_get_count();

// Writes single JSON object for entry at index to out.
// JSON: {"name":"...","signal_id":N,"carrier_hz":N,"repeat":N}
// Returns ESP_ERR_INVALID_ARG if index out of range.
esp_err_t test_signals_read_entry_json(int index, char *out, size_t out_size);

// Save a new test signal. Returns ESP_ERR_INVALID_STATE if name already exists.
// Returns ESP_ERR_NO_MEM if already at max 16 entries.
esp_err_t test_signals_save(const char *name, uint32_t signal_id, uint32_t carrier_hz,
                             uint8_t repeat, const char *ticks_hex);

// Delete entry at index. Compacts array, writes count LAST for power-loss safety.
// Returns ESP_ERR_INVALID_ARG if index out of range.
esp_err_t test_signals_delete(uint8_t index);

// Returns name and signal_id for entry at index (for dropdown display).
// Returns ESP_ERR_INVALID_ARG if index out of range.
esp_err_t test_signals_get_by_index(uint8_t index, char *name_out, size_t name_size,
                                    uint32_t *signal_id);

// Returns full signal data for replay. ticks_hex_out must be at least 520 bytes.
esp_err_t test_signals_get_replay_data(uint8_t index, uint32_t *carrier_hz,
                                       uint8_t *repeat, char *ticks_hex_out, size_t hex_size);

// Save signal buffer entry directly to NVS test signals (avoids JSON hex truncation).
// Returns ESP_ERR_INVALID_STATE if name already exists, ESP_ERR_NO_MEM if at max 16.
esp_err_t test_signals_save_from_buffer(const char *name, uint8_t buffer_index);

#ifdef __cplusplus
}
#endif
