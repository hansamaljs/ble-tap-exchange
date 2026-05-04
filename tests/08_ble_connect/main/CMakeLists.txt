idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "." "../../"
    REQUIRES ble ble_service common rtc esp_timer nvs_flash
)

# Expose tests/ to ble_service so its __has_include("test_config.h") resolves
idf_component_get_property(ble_svc_lib ble_service COMPONENT_LIB)
target_include_directories(${ble_svc_lib} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
