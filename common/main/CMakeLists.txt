# Sources by role (Kconfig "Role of this chip"):
#   one chip       Matter + BLE scanning + web configuration (ESP32-C6 Thread, ESP32-C3 Wi-Fi)
#   scanner        two-chip version, ESP32-C3: BLE scanning + web configuration, presence sent over UART, no Matter
#   Matter node    two-chip version, ESP32-H2: Matter over Thread fed by the UART link
set(common_srcs "crc32.cpp" "ble_addr.cpp" "json_util.cpp" "presence_link_proto.cpp")

if(CONFIG_PRESENCE_ROLE_MATTER_UART)
    set(srcs ${common_srcs} "app_main_h2.cpp" "matter_device.cpp" "matter_bridge.cpp" "presence_link.cpp"
        "status_ui.cpp" "click_detector.cpp")
    set(embed "")
else()
    set(srcs ${common_srcs}
        "app_main.cpp" "app_mode.cpp" "app_mode_logic.cpp" "button_handler.cpp" "click_detector.cpp"
        "ble_scanner.cpp" "ibeacon_parser.cpp" "presence_logic.cpp" "presence_manager.cpp" "presence_config.cpp"
        "presence_battery.cpp" "config_store.cpp" "scan_cache.cpp" "wifi_config.cpp" "web_server.cpp"
        "http_guard.cpp" "diag.cpp" "mac_watch.cpp")
    if(CONFIG_PRESENCE_ROLE_SCANNER)
        list(APPEND srcs "presence_link.cpp")
    else()
        list(APPEND srcs "matter_device.cpp")
    endif()
    # Configuration page (HTML, CSS and JavaScript in one file, no CDN): served from flash
    set(embed "web/index.html")
endif()

idf_component_register(SRCS              ${srcs}
                       PRIV_INCLUDE_DIRS "."
                       EMBED_FILES       ${embed})

set_property(TARGET ${COMPONENT_LIB} PROPERTY CXX_STANDARD 17)
target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
