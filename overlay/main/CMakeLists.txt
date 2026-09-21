idf_component_register(
    SRCS "main.c"
         "medicine_model.c"
         "medicine_store.c"
         "time_sync.c"
         "blufi_security.c"
         "power_manager.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt esp_event esp_hw_support esp_netif esp_timer esp_wifi mbedtls nvs_flash
)
