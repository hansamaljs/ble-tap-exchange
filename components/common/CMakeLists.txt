idf_component_register(
    SRCS "wifi_sync.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_wifi lwip nvs_flash
)
