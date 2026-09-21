#include "medicine_store.h"

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "medicine_store";
static const char *NAMESPACE = "med_rem";
static bool s_ready;

esp_err_t medicine_store_init(medicine_model_t *model) {
    if (!model) return ESP_ERR_INVALID_ARG;

    esp_err_t err = nvs_flash_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS init failed; defaults will be used: %s", esp_err_to_name(err));
        return err;
    }

    nvs_handle_t handle;
    err = nvs_open(NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        s_ready = true;
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NVS open failed: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t hour = model->reminder_hour;
    uint8_t minute = model->reminder_minute;
    uint8_t enabled = model->reminder_enabled ? 1 : 0;
    (void)nvs_get_u8(handle, "hour", &hour);
    (void)nvs_get_u8(handle, "minute", &minute);
    (void)nvs_get_u8(handle, "enabled", &enabled);
    nvs_close(handle);

    if (!medicine_model_set_reminder(model, hour, minute, enabled != 0)) {
        ESP_LOGW(TAG, "Stored reminder is invalid; using defaults");
        medicine_model_set_reminder(model, 8, 0, true);
    }
    s_ready = true;
    return ESP_OK;
}

esp_err_t medicine_store_save(const medicine_model_t *model) {
    if (!model) return ESP_ERR_INVALID_ARG;
    if (!s_ready) return ESP_ERR_INVALID_STATE;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;

    if ((err = nvs_set_u8(handle, "hour", model->reminder_hour)) == ESP_OK &&
        (err = nvs_set_u8(handle, "minute", model->reminder_minute)) == ESP_OK &&
        (err = nvs_set_u8(handle, "enabled", model->reminder_enabled ? 1 : 0)) == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
