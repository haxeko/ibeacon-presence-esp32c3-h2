/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// CONFIG mode list of the iBeacons heard around the device, for the web page.
//
// The NimBLE host task only queues reports (bounded queue, 0 timeout, drops counted): iBeacon frames, and the
// Eddystone-TLM frames and names of the addresses that sent one lately (the names of every other device around are
// dropped right there). The cache task merges them: one entry per address and type, RSSI / data / last seen updated
// by every iBeacon frame, the name filled in from a scan response or an advertisement of the same address, the
// battery voltage from its TLM frames. At most CONFIG_PRESENCE_SCAN_CACHE_SIZE entries (allocated when the CONFIG
// mode starts, nothing in NORMAL mode): a new beacon replaces the one heard least recently, and beacons not heard
// for CONFIG_PRESENCE_SCAN_CACHE_EXPIRE_SEC are removed.

#pragma once

#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#include "ble_scanner.h"

typedef struct {
    uint8_t addr[BLE_ADDR_LEN];
    uint8_t addr_type; // over-the-air type
    int8_t rssi;       // last report
    ibeacon_t ibeacon;
    char name[BLE_SCANNER_NAME_SIZE];
    uint16_t battery_mv; // last Eddystone-TLM frame, 0 if none (or the beacon does not measure)
    uint8_t tlm_order;   // tlm_byte_order_t of the beacon's voltage field
    int64_t battery_ms;  // time of that frame
    int64_t first_seen_ms;
    int64_t last_seen_ms;
    uint32_t count; // iBeacon reports
} scan_entry_t;

typedef struct {
    uint32_t count;
    uint32_t capacity;
    uint32_t queue_drops;
    uint32_t evictions;
} scan_cache_stats_t;

esp_err_t scan_cache_start(void);

// ble_scanner sink (NimBLE host task)
void scan_cache_sink(const ble_adv_report_t *report);

// Copies up to max entries, strongest RSSI first, after removing expired ones. *now_ms: time of the snapshot.
size_t scan_cache_snapshot(scan_entry_t *out, size_t max, int64_t *now_ms);
void scan_cache_clear(void);
void scan_cache_get_stats(scan_cache_stats_t *out);
