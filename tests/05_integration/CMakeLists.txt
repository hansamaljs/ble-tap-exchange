cmake_minimum_required(VERSION 3.16)
set(EXTRA_COMPONENT_DIRS "../../components/drivers/ssd1306"
                         "../../components/drivers/button"
                         "../../components/drivers/buzzer"
                         "../../components/drivers/rtc"
                         "../../components/common")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(test_integration)
