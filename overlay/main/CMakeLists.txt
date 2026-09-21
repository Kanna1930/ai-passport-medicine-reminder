idf_component_register(
    SRCS "main.c"
         "medicine_model.c"
         "medicine_store.c"
    INCLUDE_DIRS "."
    REQUIRES bsp nvs_flash esp_timer
)
