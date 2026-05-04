cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "../../components/drivers/rtc"
                         "../../components/common")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_rtc)
