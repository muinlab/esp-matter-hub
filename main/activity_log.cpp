#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs.h>

#include "activity_log.h"

static const char *TAG = "activity_log";
static const char *kNvsNamespace = "act_log";
static constexpr uint16_t kMaxEntries = 50;
static constexpr uint16_t kMaxDetailLen = 89;

// Entry blob layout: timestamp(4) + action_type(1) + detail_json_len(2) + detail_json(variable)
static constexpr size_t kEntryBlobMaxSize = 4 + 1 + 2 + kMaxDetailLen;

static SemaphoreHandle_t s_log_mutex = nullptr;
static uint16_t s_head  = 0; // next write index (ring buffer write pointer)
static uint16_t s_count = 0; // number of valid entries stored

// ── Internal helpers ──────────────────────────────────────────

static esp_err_t nvs_read_u16(nvs_handle_t h, const char *key, uint16_t *out, uint16_t default_val)
{
    esp_err_t err = nvs_get_u16(h, key, out);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        *out = default_val;
        return ESP_OK;
    }
    return err;
}

// ── Public API ────────────────────────────────────────────────

extern "C" {

esp_err_t activity_log_init()
{
    s_log_mutex = xSemaphoreCreateMutex();
    if (!s_log_mutex) {
        ESP_LOGE(TAG, "Failed to create mutex");
        return ESP_ERR_NO_MEM;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        // namespace not yet created — start fresh
        s_head  = 0;
        s_count = 0;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        s_head  = 0;
        s_count = 0;
        return ESP_OK; // non-fatal: log will start fresh
    }

    nvs_read_u16(handle, "head",  &s_head,  0);
    nvs_read_u16(handle, "count", &s_count, 0);

    // sanity clamp
    if (s_head  >= kMaxEntries) s_head  = 0;
    if (s_count >  kMaxEntries) s_count = kMaxEntries;

    nvs_close(handle);
    ESP_LOGI(TAG, "init: head=%u count=%u", s_head, s_count);
    return ESP_OK;
}

esp_err_t activity_log_append(activity_action_t action, const char *detail_json)
{
    if (!s_log_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        ESP_LOGW(TAG, "mutex timeout on append");
        return ESP_ERR_TIMEOUT;
    }

    uint32_t ts = (uint32_t)time(nullptr);

    // build blob
    uint8_t blob[kEntryBlobMaxSize];
    size_t  detail_len = detail_json ? strnlen(detail_json, kMaxDetailLen) : 0;
    uint16_t dlen16 = (uint16_t)detail_len;

    size_t offset = 0;
    memcpy(blob + offset, &ts,     4); offset += 4;
    memcpy(blob + offset, &action, 1); offset += 1;
    memcpy(blob + offset, &dlen16, 2); offset += 2;
    if (detail_len > 0) {
        memcpy(blob + offset, detail_json, detail_len);
        offset += detail_len;
    }
    size_t blob_size = offset;

    // write index = s_head (current write position)
    uint16_t write_idx = s_head;

    // advance head and count
    s_head = (s_head + 1) % kMaxEntries;
    if (s_count < kMaxEntries) {
        s_count++;
    }

    // persist to NVS
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        xSemaphoreGive(s_log_mutex);
        ESP_LOGW(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    char key[8];
    snprintf(key, sizeof(key), "e%u", (unsigned)write_idx);
    err = nvs_set_blob(handle, key, blob, blob_size);

    if (err == ESP_OK) {
        err = nvs_set_u16(handle, "head",  s_head);
    }
    if (err == ESP_OK) {
        err = nvs_set_u16(handle, "count", s_count);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }

    nvs_close(handle);
    xSemaphoreGive(s_log_mutex);

    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS write failed: %s", esp_err_to_name(err));
    }
    return err;
}

int activity_log_get_count()
{
    if (!s_log_mutex) {
        return 0;
    }
    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return 0;
    }
    int count = (int)s_count;
    xSemaphoreGive(s_log_mutex);
    return count;
}

// index 0 = newest, index count-1 = oldest
// Ring: head is the NEXT write slot, so (head-1) mod 50 is the newest entry.
esp_err_t activity_log_read_entry_json(int index, char *out, size_t out_size)
{
    if (!out || out_size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!s_log_mutex) {
        return ESP_ERR_INVALID_STATE;
    }
    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }

    if (index < 0 || index >= (int)s_count) {
        xSemaphoreGive(s_log_mutex);
        return ESP_ERR_INVALID_ARG;
    }

    // newest entry is at (head - 1) mod kMaxEntries
    // index-th entry from newest = (head - 1 - index + N*kMaxEntries) mod kMaxEntries
    uint16_t ring_idx = (uint16_t)((s_head + kMaxEntries - 1 - (uint16_t)index) % kMaxEntries);

    xSemaphoreGive(s_log_mutex);

    // read blob from NVS (outside mutex — per-key NVS ops are atomic)
    nvs_handle_t handle;
    esp_err_t err = nvs_open(kNvsNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err;
    }

    char key[8];
    snprintf(key, sizeof(key), "e%u", (unsigned)ring_idx);

    uint8_t blob[kEntryBlobMaxSize];
    size_t  blob_size = sizeof(blob);
    err = nvs_get_blob(handle, key, blob, &blob_size);
    nvs_close(handle);

    if (err != ESP_OK) {
        return err;
    }
    if (blob_size < 7) { // minimum: ts(4)+action(1)+dlen(2)
        return ESP_ERR_INVALID_SIZE;
    }

    uint32_t ts;
    uint8_t  action_byte;
    uint16_t dlen;
    memcpy(&ts,          blob,     4);
    memcpy(&action_byte, blob + 4, 1);
    memcpy(&dlen,        blob + 5, 2);

    if (dlen > kMaxDetailLen || (size_t)(7 + dlen) > blob_size) {
        dlen = 0;
    }

    char detail[kMaxDetailLen + 1] = "{}";
    if (dlen > 0) {
        memcpy(detail, blob + 7, dlen);
        detail[dlen] = '\0';
    }

    snprintf(out, out_size, "{\"ts\":%" PRIu32 ",\"act\":%u,\"d\":%s}",
             ts, (unsigned)action_byte, detail);
    return ESP_OK;
}

} // extern "C"
