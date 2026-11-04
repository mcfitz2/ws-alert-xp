cmake_minimum_required(VERSION 3.16.0)
# The ws6_ptt library and BTstack's ESP32 port (tools/setup_btstack.sh).
list(APPEND EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../../ws6_ptt"
    "${CMAKE_CURRENT_LIST_DIR}/../../btstack/port/esp32/components/btstack"
    "${CMAKE_CURRENT_LIST_DIR}/../../btstack/port/esp32/components/btstack_config")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(radio_combo)
