idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES ssd1306 common esp_timer
)
