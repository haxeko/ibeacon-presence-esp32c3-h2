# ESP32-C3 side of the two-chip iBeacon presence firmware: BLE scanning of the iBeacons (20 slots) and the web
# configuration page (triple click on BOOT), no Matter. The presence of the slots goes over a UART link to the
# ESP32-H2 (h2-bridge), which is the Matter over Thread bridge.
# Thin project: the code (main), the partition table and the shared sdkconfig defaults are those of ../common, built
# for esp32c3 with CONFIG_PRESENCE_ROLE_SCANNER. See ../README.md.
cmake_minimum_required(VERSION 3.16)

set(BLE_PRESENCE_WITH_MATTER OFF)
set(BLE_PRESENCE_PROJECT_NAME ble_presence_scanner_c3)
include(${CMAKE_CURRENT_LIST_DIR}/../common/ble_presence.cmake)

if(NOT IDF_TARGET STREQUAL "esp32c3")
    message(FATAL_ERROR "This project builds the ESP32-C3 scanner of the two-chip version (idf.py set-target esp32c3)")
endif()

project(${BLE_PRESENCE_PROJECT_NAME})

ble_presence_compile_options()
