#pragma once

#include <stdbool.h>
#include <time.h>
#include "esp_err.h"

typedef void (*power_wake_cb_t)(void *user);

esp_err_t power_manager_init(power_wake_cb_t wake_cb, void *user);
esp_err_t power_manager_sleep_until(time_t wake_epoch);
bool power_manager_busy(void);
