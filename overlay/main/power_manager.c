#include "power_manager.h"

#include "bsp_audio.h"
#include "bsp_display.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "power";

typedef struct {
    TaskHandle_t task;
    volatile bool busy;
    power_wake_cb_t wake_cb;
    void *wake_user;
} power_ctx_t;

static power_ctx_t s;

static void sleep_task(void *arg) {
    (void)arg;
    for (;;) {
        uint32_t wake_epoch_u32 = 0;
        xTaskNotifyWait(0, UINT32_MAX, &wake_epoch_u32, portMAX_DELAY);
        time_t wake_epoch = (time_t)wake_epoch_u32;
        time_t now = time(NULL);
        int64_t seconds = (int64_t)wake_epoch - (int64_t)now;
        if (seconds < 1) seconds = 1;

        s.busy = true;
        esp_err_t audio_err = bsp_audio_sleep();
        if (audio_err != ESP_OK) {
            ESP_LOGW(TAG, "Audio suspend failed, continuing standby: %s",
                     esp_err_to_name(audio_err));
        }
        bsp_display_backlight(0);

        esp_err_t err = esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
        if (err == ESP_OK) err = esp_light_sleep_start();
        esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_TIMER);

        esp_err_t wake_err = bsp_audio_wake();
        if (wake_err != ESP_OK) {
            ESP_LOGW(TAG, "Audio wake failed: %s", esp_err_to_name(wake_err));
        }
        bsp_display_backlight(72);
        s.busy = false;

        if (err != ESP_OK) {
            ESP_LOGE(TAG, "Light sleep failed: %s", esp_err_to_name(err));
        }
        if (s.wake_cb) s.wake_cb(s.wake_user);
    }
}

esp_err_t power_manager_init(power_wake_cb_t wake_cb, void *user) {
    if (s.task) {
        s.wake_cb = wake_cb;
        s.wake_user = user;
        return ESP_OK;
    }
    s.wake_cb = wake_cb;
    s.wake_user = user;
    if (xTaskCreate(sleep_task, "medicine_sleep", 3072, NULL, 4, &s.task) != pdPASS) {
        s.task = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

esp_err_t power_manager_sleep_until(time_t wake_epoch) {
    if (!s.task || wake_epoch <= 0) return ESP_ERR_INVALID_STATE;
    if (s.busy) return ESP_ERR_INVALID_STATE;
    if ((uint64_t)wake_epoch > UINT32_MAX) return ESP_ERR_INVALID_ARG;
    return xTaskNotify(s.task, (uint32_t)wake_epoch, eSetValueWithOverwrite) == pdPASS
               ? ESP_OK
               : ESP_FAIL;
}

bool power_manager_busy(void) {
    return s.busy;
}
