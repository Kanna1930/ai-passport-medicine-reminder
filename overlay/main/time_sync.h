#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    TIME_SYNC_IDLE = 0,
    TIME_SYNC_CONNECTING,
    TIME_SYNC_PROVISIONING,
    TIME_SYNC_PHONE_CONNECTED,
    TIME_SYNC_WIFI_CONNECTING,
    TIME_SYNC_SNTP,
    TIME_SYNC_DONE,
    TIME_SYNC_FAILED,
} time_sync_state_t;

esp_err_t time_sync_start(bool force_provision);
bool time_sync_busy(void);
time_sync_state_t time_sync_get_state(void);
esp_err_t time_sync_last_error(void);
const char *time_sync_blufi_name(void);
bool time_sync_clock_valid(void);
