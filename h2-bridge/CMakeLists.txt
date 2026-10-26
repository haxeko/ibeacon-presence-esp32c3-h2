# ESP32-H2 side of the two-chip iBeacon presence firmware: the Matter over Thread bridge ("iBeacon Bridge", one
# bridged occupancy sensor per slot), fed over a UART link by the ESP32-C3 scanner (c3-scanner).
# Thin project: the code (main), the partition table and the shared sdkconfig defaults are those of ../common, built
# for esp32h2 with CONFIG_PRESENCE_ROLE_MATTER_UART. See ../README.md.
cmake_minimum_required(VERSION 3.16)

set(BLE_PRESENCE_PROJECT_NAME matter_ble_presence_h2)
include(${CMAKE_CURRENT_LIST_DIR}/../common/ble_presence.cmake)

if(NOT IDF_TARGET STREQUAL "esp32h2")
    message(FATAL_ERROR "This project builds the ESP32-H2 Matter node of the two-chip version (idf.py set-target esp32h2)")
endif()

project(${BLE_PRESENCE_PROJECT_NAME})

ble_presence_compile_options()
