idf_component_register(
    SRCS "stopwatch_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer common
)
