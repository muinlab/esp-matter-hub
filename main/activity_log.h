#pragma once

#include <esp_err.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum activity_action_t : uint8_t {
    ACT_IR_LEARN        = 0,
    ACT_SEND_SIGNAL_RAW = 1,
    ACT_REPLAY          = 2,
    ACT_SLOT_CONFIG     = 3,
    ACT_COMMISSIONING   = 4,
    ACT_DUMP_NVS        = 5,
    ACT_FACTORY_RESET   = 6,
    ACT_API_KEY_CHANGE  = 7,
};

esp_err_t activity_log_init();
esp_err_t activity_log_append(activity_action_t action, const char *detail_json);
int       activity_log_get_count();
esp_err_t activity_log_read_entry_json(int index, char *out, size_t out_size);

#ifdef __cplusplus
}
#endif
