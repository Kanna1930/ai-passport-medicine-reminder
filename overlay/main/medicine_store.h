#pragma once

#include "esp_err.h"
#include "medicine_model.h"

esp_err_t medicine_store_init(medicine_model_t *model);
esp_err_t medicine_store_save(const medicine_model_t *model);
