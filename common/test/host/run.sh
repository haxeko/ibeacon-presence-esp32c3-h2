#!/bin/sh
# Host tests of the firmware logic: no ESP32 or ESP-IDF needed, only a C++17 compiler (g++, or clang++ via CXX).
# The few ESP-IDF headers the store needs are stubbed in stubs/ (NVS is simulated in fake_nvs.cpp).
# Every test runs with AddressSanitizer and UndefinedBehaviorSanitizer, so a read past the end of a malformed
# advertising frame fails the test. Run: test/host/run.sh
set -e
cd "$(dirname "$0")"
CXX=${CXX:-g++}
MAIN=../../main
OUT=$(mktemp -d)
trap 'rm -rf "$OUT"' EXIT
# Warnings as errors, as in the ESP-IDF build of the main component
FLAGS="-std=gnu++17 -g -O1 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Werror
       -fsanitize=address,undefined -fno-sanitize-recover=all -I stubs -I . -I $MAIN"

$CXX $FLAGS -o "$OUT/ibeacon" test_ibeacon.cpp $MAIN/ibeacon_parser.cpp $MAIN/json_util.cpp
$CXX $FLAGS -o "$OUT/ble_addr" test_ble_addr.cpp $MAIN/ble_addr.cpp
$CXX $FLAGS -o "$OUT/presence_battery" test_presence_battery.cpp $MAIN/presence_battery.cpp
$CXX $FLAGS -o "$OUT/presence_logic" test_presence_logic.cpp $MAIN/presence_logic.cpp
$CXX $FLAGS -DCONFIG_PRESENCE_ROLE_SCANNER=1 -o "$OUT/presence_logic_20" test_presence_logic.cpp $MAIN/presence_logic.cpp
$CXX $FLAGS -o "$OUT/click_detector" test_click_detector.cpp $MAIN/click_detector.cpp
$CXX $FLAGS -o "$OUT/app_mode" test_app_mode.cpp $MAIN/app_mode_logic.cpp $MAIN/crc32.cpp
$CXX $FLAGS -o "$OUT/presence_config" test_presence_config.cpp $MAIN/presence_config.cpp $MAIN/ble_addr.cpp \
    $MAIN/json_util.cpp $MAIN/crc32.cpp
# Two-chip version: 20 slots
$CXX $FLAGS -DCONFIG_PRESENCE_ROLE_SCANNER=1 -o "$OUT/presence_config_20" test_presence_config.cpp \
    $MAIN/presence_config.cpp $MAIN/ble_addr.cpp $MAIN/json_util.cpp $MAIN/crc32.cpp
$CXX $FLAGS -o "$OUT/mac_watch" test_mac_watch.cpp $MAIN/mac_watch.cpp $MAIN/crc32.cpp
$CXX $FLAGS -o "$OUT/json_util" test_json_util.cpp $MAIN/json_util.cpp
$CXX $FLAGS -o "$OUT/http_guard" test_http_guard.cpp $MAIN/http_guard.cpp
$CXX $FLAGS -o "$OUT/presence_link" test_presence_link_proto.cpp $MAIN/presence_link_proto.cpp $MAIN/crc32.cpp \
    $MAIN/json_util.cpp
if [ -f test_config_store.cpp ]; then
    $CXX $FLAGS -o "$OUT/config_store" test_config_store.cpp fake_nvs.cpp $MAIN/config_store.cpp \
        $MAIN/presence_config.cpp $MAIN/ble_addr.cpp $MAIN/json_util.cpp $MAIN/crc32.cpp $MAIN/mac_watch.cpp
    $CXX $FLAGS -DCONFIG_PRESENCE_ROLE_SCANNER=1 -o "$OUT/config_store_20" test_config_store.cpp fake_nvs.cpp \
        $MAIN/config_store.cpp $MAIN/presence_config.cpp $MAIN/ble_addr.cpp $MAIN/json_util.cpp $MAIN/crc32.cpp \
        $MAIN/mac_watch.cpp
fi

status=0
for t in "$OUT"/*; do
    "$t" || status=1
done
[ $status -eq 0 ] && echo "All host tests passed" || echo "Host tests FAILED"
exit $status
