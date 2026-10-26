# Build setup shared by the two projects of the two-chip iBeacon presence firmware, included before project() by
# their top-level CMakeLists.txt:
#   - c3-scanner: ESP32-C3, BLE scanning + web configuration, no Matter (idf.py set-target esp32c3)
#   - h2-bridge: ESP32-H2, Matter over Thread bridge fed by the scanner over UART (idf.py set-target esp32h2)
# Both build the code (main), the partition table and the sdkconfig defaults of this directory. Before including it
# a project sets BLE_PRESENCE_PROJECT_NAME (name of the build output) and may set BLE_PRESENCE_WITH_MATTER to OFF (no
# esp-matter components). Defines ble_presence_compile_options() to call after project().

if(NOT DEFINED BLE_PRESENCE_WITH_MATTER)
    set(BLE_PRESENCE_WITH_MATTER ON)
endif()
if(BLE_PRESENCE_WITH_MATTER AND NOT DEFINED ENV{ESP_MATTER_PATH})
    message(FATAL_ERROR "Please set ESP_MATTER_PATH to the path of esp-matter repo")
endif()

set(BLE_PRESENCE_DIR "${CMAKE_CURRENT_LIST_DIR}")

# Firmware version, shown by controllers as "Firmware" (SoftwareVersionString): 1.0.<build number>, the build number
# being FIRMWARE_BUILD_OFFSET + the git commit count. PROJECT_VER_NUMBER (SoftwareVersion) is the same number. The
# firmware was developed in another repository up to 1.0.204: the offset carries the numbering on from there, so a
# device updated from an earlier build always sees a higher version.
set(FIRMWARE_VERSION_BASE "1.0")
set(FIRMWARE_BUILD_OFFSET 204)
execute_process(COMMAND git rev-list --count HEAD
                WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                OUTPUT_VARIABLE FIRMWARE_BUILD_NUMBER
                OUTPUT_STRIP_TRAILING_WHITESPACE
                RESULT_VARIABLE git_result
                ERROR_QUIET)
if(NOT git_result EQUAL 0 OR FIRMWARE_BUILD_NUMBER STREQUAL "")
    message(WARNING "Not a git checkout, firmware build number set to 0")
    set(FIRMWARE_BUILD_NUMBER 0)
else()
    math(EXPR FIRMWARE_BUILD_NUMBER "${FIRMWARE_BUILD_OFFSET} + ${FIRMWARE_BUILD_NUMBER}")
    # Re-run CMake (and pick up the new version) after every commit or checkout.
    execute_process(COMMAND git rev-parse --absolute-git-dir
                    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}
                    OUTPUT_VARIABLE git_dir
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(EXISTS "${git_dir}/logs/HEAD")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${git_dir}/logs/HEAD")
    endif()
endif()
set(PROJECT_VER "${FIRMWARE_VERSION_BASE}.${FIRMWARE_BUILD_NUMBER}")
set(PROJECT_VER_NUMBER ${FIRMWARE_BUILD_NUMBER})
message(STATUS "Firmware version ${PROJECT_VER} (${PROJECT_VER_NUMBER})")

if(BLE_PRESENCE_WITH_MATTER)
    set(ESP_MATTER_PATH $ENV{ESP_MATTER_PATH})
    set(MATTER_SDK_PATH ${ESP_MATTER_PATH}/connectedhomeip/connectedhomeip)
endif()

# sdkconfig defaults, in this order: this directory's sdkconfig.defaults (shared by both chips),
# sdkconfig.defaults.matter for the project with Matter, the sdkconfig.defaults of the project (target, role, UART
# pins, product ID, paths of the shared partition table and CHIP project config), then any file given with
# -D SDKCONFIG_DEFAULTS=... or the SDKCONFIG_DEFAULTS environment variable.
set(ble_presence_defaults "${BLE_PRESENCE_DIR}/sdkconfig.defaults")
if(BLE_PRESENCE_WITH_MATTER)
    list(APPEND ble_presence_defaults "${BLE_PRESENCE_DIR}/sdkconfig.defaults.matter")
endif()
set(ble_presence_main_component "${BLE_PRESENCE_DIR}/main")
list(APPEND ble_presence_defaults "${CMAKE_SOURCE_DIR}/sdkconfig.defaults")
if(SDKCONFIG_DEFAULTS)
    set(ble_presence_given "${SDKCONFIG_DEFAULTS}")
elseif(DEFINED ENV{SDKCONFIG_DEFAULTS})
    set(ble_presence_given "$ENV{SDKCONFIG_DEFAULTS}")
else()
    set(ble_presence_given "")
endif()
foreach(ble_presence_file ${ble_presence_given})
    get_filename_component(ble_presence_file_real "${ble_presence_file}" REALPATH BASE_DIR "${CMAKE_SOURCE_DIR}")
    set(ble_presence_known OFF)
    foreach(ble_presence_listed ${ble_presence_defaults})
        get_filename_component(ble_presence_listed_real "${ble_presence_listed}" REALPATH)
        if(ble_presence_file_real STREQUAL ble_presence_listed_real)
            set(ble_presence_known ON)
        endif()
    endforeach()
    if(NOT ble_presence_known)
        list(APPEND ble_presence_defaults "${ble_presence_file}")
    endif()
endforeach()
set(SDKCONFIG_DEFAULTS ${ble_presence_defaults})

# This should be done before using the IDF_TARGET variable.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if(BLE_PRESENCE_WITH_MATTER)
    set(EXTRA_COMPONENT_DIRS
        "${MATTER_SDK_PATH}/config/esp32/components"
        "${ESP_MATTER_PATH}/components"
        ${ble_presence_main_component})
else()
    set(EXTRA_COMPONENT_DIRS ${ble_presence_main_component})
endif()

# The build output is named after the project: build/<name>.bin
if(NOT DEFINED BLE_PRESENCE_PROJECT_NAME)
    message(FATAL_ERROR "Build the ESP32-C3 scanner in c3-scanner or the ESP32-H2 bridge in h2-bridge")
endif()

macro(ble_presence_compile_options)
    idf_build_set_property(CXX_COMPILE_OPTIONS "-std=gnu++17;-Os;-Wno-overloaded-virtual" APPEND)
    if(BLE_PRESENCE_WITH_MATTER)
        idf_build_set_property(CXX_COMPILE_OPTIONS "-DCHIP_HAVE_CONFIG_H" APPEND)
    endif()
    idf_build_set_property(C_COMPILE_OPTIONS "-Os" APPEND)
    # For RISC-V chips, project_include.cmake of esp-matter sets -Wno-format, but does not clear the flags that depend
    # on -Wformat
    idf_build_set_property(COMPILE_OPTIONS "-Wno-format-nonliteral;-Wno-format-security" APPEND)
endmacro()
