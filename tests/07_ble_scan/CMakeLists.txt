cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS
    "../../components/drivers"
    "../../components/services"
    "../../components"
)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(07_ble_scan)
