idf_component_register(
    SRCS "ble_service.c"
    INCLUDE_DIRS "include"
    REQUIRES
        ble
        rtc
        nvs_flash
        esp_timer
        common
)
