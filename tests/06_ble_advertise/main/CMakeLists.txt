idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "." "../../"
    REQUIRES ble ble_service common rtc esp_timer nvs_flash driver
)
