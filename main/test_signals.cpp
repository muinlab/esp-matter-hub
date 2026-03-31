#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs.h>

#include "test_signals.h"

static const char *TAG = "test_signals";
static const char *kNvsNamespace = "test_sig";
static const char *kNvsKeyCount  = "count";
static constexpr uint8_t  kMaxEntries   = 16;
static constexpr uint32_t kMutexTimeout = pdMS_TO_TICKS(100);

// ── Shared mutex (protects both signal buffer and test signal NVS state) ─────

static SemaphoreHandle_t s_sig_mutex = nullptr;

// ── Signal Buffer (RAM) ───────────────────────────────────────────────────────

static signal_buffer_entry_t s_buf[kMaxEntries];
static int s_buf_count = 0;  // number of valid entries (0..16)

extern "C" {

void signal_buffer_insert(uint32_t signal_id, uint32_t carrier_hz, uint8_t repeat,
                          const uint16_t *ticks, size_t tick_count)
{
    if (!s_sig_mutex) {
        return;
    }
    if (xSemaphoreTake(s_sig_mutex, kMutexTimeout) != pdTRUE) {
        ESP_LOGW(TAG, "signal_buffer_insert: mutex timeout");
        return;
    }

    // Clamp tick_count to max we can encode
    if (tick_count > 128) {
        tick_count = 128;
    }

    // Check if signal_id already exists; if so, find its index
    int existing_idx = -1;
    for (int i = 0; i < s_buf_count; ++i) {
        if (s_buf[i].signal_id == signal_id) {
            existing_idx = i;
            break;
        }
    }

    if (existing_idx == 0) {
        // Already at front — just update in place
        signal_buffer_entry_t &e = s_buf[0];
        e.carrier_hz = carrier_hz;
        e.repeat     = repeat;
        e.tick_count = static_cast<uint16_t>(tick_count);
        // Encode ticks as hex
        size_t pos = 0;
        for (size_t i = 0; i < tick_count && pos + 4 < sizeof(e.ticks_hex); ++i) {
            pos += snprintf(e.ticks_hex + pos, sizeof(e.ticks_hex) - pos, "%04X", ticks[i]);
        }
        e.ticks_hex[pos] = '\0';
        xSemaphoreGive(s_sig_mutex);
        return;
    }

    if (existing_idx > 0) {
        // Move existing entry to front: shift [0..existing_idx-1] right by one
        signal_buffer_entry_t tmp = s_buf[existing_idx];
        memmove(&s_buf[1], &s_buf[0], sizeof(signal_buffer_entry_t) * static_cast<size_t>(existing_idx));
        s_buf[0] = tmp;
        // Update values
        signal_buffer_entry_t &e = s_buf[0];
        e.carrier_hz = carrier_hz;
        e.repeat     = repeat;
        e.tick_count = static_cast<uint16_t>(tick_count);
        size_t pos = 0;
        for (size_t i = 0; i < tick_count && pos + 4 < sizeof(e.ticks_hex); ++i) {
            pos += snprintf(e.ticks_hex + pos, sizeof(e.ticks_hex) - pos, "%04X", ticks[i]);
        }
        e.ticks_hex[pos] = '\0';
        xSemaphoreGive(s_sig_mutex);
        return;
    }

    // New entry: evict last if full
    if (s_buf_count == kMaxEntries) {
        // Shift everything right by one, dropping last (LRU eviction)
        memmove(&s_buf[1], &s_buf[0], sizeof(signal_buffer_entry_t) * (kMaxEntries - 1));
    } else {
        // Shift existing entries right to make room at front
        if (s_buf_count > 0) {
            memmove(&s_buf[1], &s_buf[0], sizeof(signal_buffer_entry_t) * static_cast<size_t>(s_buf_count));
        }
        s_buf_count++;
    }

    signal_buffer_entry_t &e = s_buf[0];
    e.signal_id  = signal_id;
    e.carrier_hz = carrier_hz;
    e.repeat     = repeat;
    e.tick_count = static_cast<uint16_t>(tick_count);
    size_t pos = 0;
    for (size_t i = 0; i < tick_count && pos + 4 < sizeof(e.ticks_hex); ++i) {
        pos += snprintf(e.ticks_hex + pos, sizeof(e.ticks_hex) - pos, "%04X", ticks[i]);
    }
    e.ticks_hex[pos] = '\0';

    xSemaphoreGive(s_sig_mutex);
}

int signal_buffer_get_count()
{
    if (!s_sig_mutex) {
        return 0;
    }
    if (xSemaphoreTake(s_sig_mutex, kMutexTimeout) != pdTRUE) {
        return 0;
    }
    int count = s_buf_count;
    xSemaphoreGive(s_sig_mutex);
    return count;
}

esp_err_t signal_buffer_read_entry_json(int index, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_sig_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_sig_mutex, kMutexTimeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (index < 0 || index >= s_buf_count) {
        xSemaphoreGive(s_sig_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    const signal_buffer_entry_t &e = s_buf[index];

    // First 40 chars of ticks_hex
    char ticks_preview[41];
    strncpy(ticks_preview, e.ticks_hex, 40);
    ticks_preview[40] = '\0';

    snprintf(out, out_size,
             "{\"signal_id\":%" PRIu32 ",\"carrier_hz\":%" PRIu32
             ",\"repeat\":%u,\"tick_count\":%u,\"ticks_hex\":\"%s\"}",
             e.signal_id, e.carrier_hz, e.repeat, e.tick_count, ticks_preview);

    xSemaphoreGive(s_sig_mutex);
    return ESP_OK;
}

// ── Saved Test Signals (NVS) ──────────────────────────────────────────────────

// NVS blob layout:
//   [0]        name_len (uint8_t, 0..31)
//   [1..31]    name (up to 31 bytes, NOT null-terminated in blob)
//   [32..35]   signal_id (uint32_t, little-endian)
//   [36..39]   carrier_hz (uint32_t, little-endian)
//   [40]       repeat (uint8_t)
//   [41..42]   tick_count (uint16_t, little-endian)
//   [43..]     ticks_hex (variable length, null-terminated)

static constexpr size_t kBlobNameOffset      = 0;
static constexpr size_t kBlobNameMaxLen      = 31;
static constexpr size_t kBlobSignalIdOffset  = 32;
static constexpr size_t kBlobCarrierHzOffset = 36;
static constexpr size_t kBlobRepeatOffset    = 40;
static constexpr size_t kBlobTickCountOffset = 41;
static constexpr size_t kBlobTicksHexOffset  = 43;
static constexpr size_t kBlobFixedSize       = 43;  // bytes before ticks_hex

static uint8_t s_ts_count = 0;  // number of saved test signals

esp_err_t test_signals_init()
{
    s_sig_mutex = xSemaphoreCreateMutex();
    if (!s_sig_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_ts_count = 0;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        s_ts_count = 0;
        return ESP_OK;
    }

    uint8_t count = 0;
    err = nvs_get_u8(handle, kNvsKeyCount, &count);
    if (err == ESP_OK && count <= kMaxEntries) {
        s_ts_count = count;
    } else {
        s_ts_count = 0;
    }
    nvs_close(handle);
    ESP_LOGI(TAG, "init: loaded %u test signals", s_ts_count);
    return ESP_OK;
}

int test_signals_get_count()
{
    if (!s_sig_mutex) {
        return 0;
    }
    if (xSemaphoreTake(s_sig_mutex, kMutexTimeout) != pdTRUE) {
        return 0;
    }
    int count = static_cast<int>(s_ts_count);
    xSemaphoreGive(s_sig_mutex);
    return count;
}

// Build NVS key for entry N: "t0".."t15"
static void make_entry_key(uint8_t index, char *key_out, size_t key_size)
{
    snprintf(key_out, key_size, "t%u", static_cast<unsigned>(index));
}

// Read blob for entry at index into a stack buffer, parse fields.
// Returns ESP_OK on success. out_* params may be NULL if not needed.
static esp_err_t read_entry_blob(uint8_t index,
                                 char *name_out, size_t name_size,
                                 uint32_t *signal_id_out,
                                 uint32_t *carrier_hz_out,
                                 uint8_t  *repeat_out,
                                 uint16_t *tick_count_out,
                                 char *ticks_hex_out, size_t ticks_hex_size)
{
    char key[8];
    make_entry_key(index, key, sizeof(key));

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    // First get blob size
    size_t blob_size = 0;
    err = nvs_get_blob(handle, key, nullptr, &blob_size);
    if (err != ESP_OK || blob_size < kBlobFixedSize) {
        nvs_close(handle);
        return (err != ESP_OK) ? err : ESP_ERR_INVALID_SIZE;
    }

    // Allocate on stack — max blob: 43 + 512 + 1 = 556 bytes, use 600 for safety
    constexpr size_t kMaxBlobSize = 600;
    if (blob_size > kMaxBlobSize) {
        nvs_close(handle);
        return ESP_ERR_INVALID_SIZE;
    }
    uint8_t blob[kMaxBlobSize];
    err = nvs_get_blob(handle, key, blob, &blob_size);
    nvs_close(handle);
    if (err != ESP_OK) {
        return err;
    }

    // Parse name
    uint8_t name_len = blob[kBlobNameOffset];
    if (name_len > kBlobNameMaxLen) {
        name_len = kBlobNameMaxLen;
    }
    if (name_out && name_size > 0) {
        size_t copy_len = (name_len < name_size - 1) ? name_len : name_size - 1;
        memcpy(name_out, &blob[1], copy_len);
        name_out[copy_len] = '\0';
    }

    // Parse fixed fields
    uint32_t sig_id, carrier;
    uint8_t  rep;
    uint16_t tc;
    memcpy(&sig_id,  &blob[kBlobSignalIdOffset],  4);
    memcpy(&carrier, &blob[kBlobCarrierHzOffset], 4);
    memcpy(&rep,     &blob[kBlobRepeatOffset],     1);
    memcpy(&tc,      &blob[kBlobTickCountOffset],  2);

    if (signal_id_out)   *signal_id_out   = sig_id;
    if (carrier_hz_out)  *carrier_hz_out  = carrier;
    if (repeat_out)      *repeat_out      = rep;
    if (tick_count_out)  *tick_count_out  = tc;

    if (ticks_hex_out && ticks_hex_size > 0 && blob_size > kBlobFixedSize) {
        size_t ticks_len = blob_size - kBlobFixedSize;
        if (ticks_len >= ticks_hex_size) {
            ticks_len = ticks_hex_size - 1;
        }
        memcpy(ticks_hex_out, &blob[kBlobTicksHexOffset], ticks_len);
        ticks_hex_out[ticks_len] = '\0';
    }

    return ESP_OK;
}

esp_err_t test_signals_read_entry_json(int index, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_sig_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_sig_mutex, kMutexTimeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (index < 0 || index >= static_cast<int>(s_ts_count)) {
        xSemaphoreGive(s_sig_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    char name[32];
    uint32_t signal_id = 0, carrier_hz = 0;
    uint8_t  repeat = 0;

    esp_err_t err = read_entry_blob(static_cast<uint8_t>(index),
                                    name, sizeof(name),
                                    &signal_id, &carrier_hz, &repeat,
                                    nullptr, nullptr, 0);
    xSemaphoreGive(s_sig_mutex);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read_entry_json[%d] failed: %s", index, esp_err_to_name(err));
        snprintf(out, out_size, "{\"name\":\"\",\"signal_id\":0,\"carrier_hz\":0,\"repeat\":0}");
        return err;
    }

    snprintf(out, out_size,
             "{\"name\":\"%s\",\"signal_id\":%" PRIu32 ",\"carrier_hz\":%" PRIu32 ",\"repeat\":%u}",
             name, signal_id, carrier_hz, repeat);
    return ESP_OK;
}

esp_err_t test_signals_save(const char *name, uint32_t signal_id, uint32_t carrier_hz,
                             uint8_t repeat, const char *ticks_hex)
{
    if (!name || name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    size_t name_len = strlen(name);
    if (name_len > kBlobNameMaxLen) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_sig_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_sig_mutex, kMutexTimeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (s_ts_count >= kMaxEntries) {
        xSemaphoreGive(s_sig_mutex);
        return ESP_ERR_NO_MEM;
    }

    // Check for duplicate name
    for (uint8_t i = 0; i < s_ts_count; ++i) {
        char existing_name[32];
        esp_err_t err = read_entry_blob(i, existing_name, sizeof(existing_name),
                                        nullptr, nullptr, nullptr, nullptr, nullptr, 0);
        if (err == ESP_OK && strcmp(existing_name, name) == 0) {
            xSemaphoreGive(s_sig_mutex);
            return ESP_ERR_INVALID_STATE;  // duplicate name
        }
    }

    // Build blob
    size_t ticks_len = ticks_hex ? strlen(ticks_hex) : 0;
    if (ticks_len > 512) {
        ticks_len = 512;  // truncate ticks_hex to max (128 ticks * 4 hex chars)
    }
    size_t blob_size = kBlobFixedSize + ticks_len + 1;  // +1 for null terminator

    // Max blob: 43 + 513 = 556 — fits on stack safely
    uint8_t blob[600];
    memset(blob, 0, sizeof(blob));

    blob[kBlobNameOffset] = static_cast<uint8_t>(name_len);
    memcpy(&blob[1], name, name_len);

    memcpy(&blob[kBlobSignalIdOffset],  &signal_id,  4);
    memcpy(&blob[kBlobCarrierHzOffset], &carrier_hz, 4);
    memcpy(&blob[kBlobRepeatOffset],    &repeat,     1);
    uint16_t tc = static_cast<uint16_t>(ticks_len / 4);  // each tick is 4 hex chars
    memcpy(&blob[kBlobTickCountOffset], &tc,          2);

    if (ticks_hex && ticks_len > 0) {
        memcpy(&blob[kBlobTicksHexOffset], ticks_hex, ticks_len);
    }
    blob[kBlobTicksHexOffset + ticks_len] = '\0';

    // Write to NVS
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_sig_mutex);
        ESP_LOGW(TAG, "test_signals_save: nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    char key[8];
    make_entry_key(s_ts_count, key, sizeof(key));
    err = nvs_set_blob(handle, key, blob, blob_size);
    if (err == ESP_OK) {
        err = nvs_set_u8(handle, kNvsKeyCount, s_ts_count + 1);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        s_ts_count++;
        ESP_LOGI(TAG, "saved test signal '%s' at index %u", name, s_ts_count - 1);
    } else {
        ESP_LOGW(TAG, "test_signals_save NVS write failed: %s", esp_err_to_name(err));
    }

    xSemaphoreGive(s_sig_mutex);
    return err;
}

esp_err_t test_signals_delete(uint8_t index)
{
    if (!s_sig_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_sig_mutex, kMutexTimeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (index >= s_ts_count) {
        xSemaphoreGive(s_sig_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_sig_mutex);
        ESP_LOGW(TAG, "test_signals_delete: nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    // Read all blobs above the deleted index and shift them down
    for (uint8_t i = index; i < s_ts_count - 1; ++i) {
        char src_key[8], dst_key[8];
        make_entry_key(i + 1, src_key, sizeof(src_key));
        make_entry_key(i,     dst_key, sizeof(dst_key));

        size_t blob_size = 0;
        esp_err_t read_err = nvs_get_blob(handle, src_key, nullptr, &blob_size);
        if (read_err != ESP_OK || blob_size == 0 || blob_size > 600) {
            continue;
        }
        uint8_t blob[600];
        read_err = nvs_get_blob(handle, src_key, blob, &blob_size);
        if (read_err != ESP_OK) {
            continue;
        }
        nvs_set_blob(handle, dst_key, blob, blob_size);
    }

    // Erase the last entry (now a duplicate after shifting)
    if (s_ts_count > 0) {
        char last_key[8];
        make_entry_key(s_ts_count - 1, last_key, sizeof(last_key));
        nvs_erase_key(handle, last_key);
    }

    // Write count LAST for power-loss safety
    uint8_t new_count = s_ts_count - 1;
    err = nvs_set_u8(handle, kNvsKeyCount, new_count);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err == ESP_OK) {
        s_ts_count = new_count;
        ESP_LOGI(TAG, "deleted test signal at index %u, count now %u", index, s_ts_count);
    } else {
        ESP_LOGW(TAG, "test_signals_delete NVS commit failed: %s", esp_err_to_name(err));
    }

    xSemaphoreGive(s_sig_mutex);
    return err;
}

esp_err_t test_signals_get_by_index(uint8_t index, char *name_out, size_t name_size,
                                    uint32_t *signal_id)
{
    if (!name_out || name_size == 0 || !signal_id) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_sig_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_sig_mutex, kMutexTimeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (index >= s_ts_count) {
        xSemaphoreGive(s_sig_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    uint32_t sig_id = 0;
    esp_err_t err = read_entry_blob(index, name_out, name_size,
                                    &sig_id, nullptr, nullptr, nullptr, nullptr, 0);
    xSemaphoreGive(s_sig_mutex);

    if (err == ESP_OK) {
        *signal_id = sig_id;
    }
    return err;
}

esp_err_t test_signals_save_from_buffer(const char *name, uint8_t buffer_index)
{
    if (!name || !name[0]) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_sig_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_sig_mutex, kMutexTimeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (buffer_index >= s_buf_count) {
        xSemaphoreGive(s_sig_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    // Copy full data from RAM buffer while holding mutex
    const signal_buffer_entry_t &e = s_buf[buffer_index];
    uint32_t sig_id = e.signal_id;
    uint32_t carrier = e.carrier_hz;
    uint8_t rep = e.repeat;
    char full_hex[520];
    strncpy(full_hex, e.ticks_hex, sizeof(full_hex) - 1);
    full_hex[sizeof(full_hex) - 1] = '\0';

    xSemaphoreGive(s_sig_mutex);

    // Delegate to test_signals_save (re-acquires mutex internally)
    return test_signals_save(name, sig_id, carrier, rep, full_hex);
}

esp_err_t test_signals_get_replay_data(uint8_t index, uint32_t *carrier_hz,
                                       uint8_t *repeat, char *ticks_hex_out, size_t hex_size)
{
    if (!carrier_hz || !repeat || !ticks_hex_out || hex_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_sig_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_sig_mutex, kMutexTimeout) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (index >= s_ts_count) {
        xSemaphoreGive(s_sig_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    char name_buf[32];
    uint32_t sig_id = 0;
    uint16_t tick_count = 0;
    esp_err_t err = read_entry_blob(index, name_buf, sizeof(name_buf),
                                    &sig_id, carrier_hz, repeat, &tick_count,
                                    ticks_hex_out, hex_size);
    xSemaphoreGive(s_sig_mutex);
    return err;
}

} // extern "C"
