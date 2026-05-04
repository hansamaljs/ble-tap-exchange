idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "." "../../"
    REQUIRES ble ble_service ssd1306 button buzzer rtc common nvs_flash esp_timer
)

# Expose tests/ to ble_service so its __has_include("test_config.h") resolves
idf_component_get_property(ble_svc_lib ble_service COMPONENT_LIB)
target_include_directories(${ble_svc_lib} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
