idf_component_register(
    SRCS "time_service.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_timer common
)
