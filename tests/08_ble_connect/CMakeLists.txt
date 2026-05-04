cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS
    "../../components/drivers"
    "../../components/services"
    "../../components"
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(08_ble_connect)

# Make test_config.h visible to every component (ble_service needs it for
# first-boot profile defaults — its own include path doesn't reach tests/).
include_directories(${CMAKE_CURRENT_LIST_DIR}/../../)
