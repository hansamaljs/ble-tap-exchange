idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES ssd1306 button buzzer rtc common driver esp_timer nvs_flash
)
