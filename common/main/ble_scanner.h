/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// BLE scanning (GAP discovery, observer role) on the one NimBLE host of the firmware.
//
// NORMAL mode: the Matter SDK (BLEManagerImpl) initialises NimBLE, owns ble_hs_cfg (sync / reset callbacks) and
// the host task, and advertises for commissioning. This module never initialises a second host or controller and
// never touches ble_hs_cfg: it waits for ble_hs_synced() and runs its own discovery procedure, whose events go to
// its own callback (NimBLE passes discovery events to the callback given to ble_gap_disc(), advertising and
// connection events to Matter's). CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING=n keeps the host after commissioning.
// CONFIG mode: Matter is not started, so this module initialises NimBLE itself.
//
// One task (ble_scan) starts and stops the discovery. It scans while nothing pauses it (SCAN_PAUSE_* reasons,
// hold-off), restarts with exponential backoff (1 s to 30 s) after a failed start or a stop the firmware did not
// ask for (host reset), and restarts a scan that delivered no advertisement at all for
// CONFIG_PRESENCE_SCAN_WATCHDOG_SEC.
//
// Scan clock: milliseconds during which a discovery was actually running. Presence timeouts count on it, so time
// with the scanner paused or stopped can never make a slot look absent (presence_logic.h).
//
// Every advertisement is reported (filter_duplicates = 0): identical repeated advertisements keep refreshing a
// beacon. The callback runs in the NimBLE host task: it only parses the frame (iBeacon, Eddystone-TLM, name) and
// hands it to the sink, which must not block (queue with 0 timeout).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <esp_err.h>

#include "ble_addr.h"
#include "ibeacon_parser.h"

#define BLE_SCANNER_NAME_SIZE 30 // the longest name of a legacy advertisement (29 bytes) and the NUL

typedef struct {
    uint8_t addr[BLE_ADDR_LEN]; // display order
    uint8_t addr_type;          // over-the-air type, BLE_OTA_ADDR_*
    int8_t rssi;                // dBm, 127 if unavailable
    bool scan_response;
    bool has_ibeacon;
    ibeacon_t ibeacon;
    bool has_tlm; // Eddystone-TLM frame (battery voltage of the beacon)
    eddystone_tlm_t tlm;
    char name[BLE_SCANNER_NAME_SIZE]; // "" if absent or not parsed
    int64_t time_ms;                  // esp_timer milliseconds
} ble_adv_report_t;

// Both called in the NimBLE host task
typedef bool (*ble_scanner_filter_t)(const uint8_t addr[BLE_ADDR_LEN], uint8_t addr_type);
typedef void (*ble_scanner_sink_t)(const ble_adv_report_t *report);

// Why the scanner is paused (any bit set: no scanning)
#define SCAN_PAUSE_MATTER_STARTING (1u << 0)  // until the Matter server is up
#define SCAN_PAUSE_NOT_COMMISSIONED (1u << 1) // no fabric yet: the radio is left to commissioning
#define SCAN_PAUSE_COMMISSIONING (1u << 2)    // PASE session (commissioning in progress)
#define SCAN_PAUSE_BLE_CONNECTION (1u << 3)   // a controller is connected over BLE (CHIPoBLE)
#define SCAN_PAUSE_NETWORK_DOWN (1u << 4)     // Thread detached / Wi-Fi disconnected: the network comes first
#define SCAN_PAUSE_USER (1u << 5)             // "stop scanning" on the configuration page
#define SCAN_PAUSE_NO_STATION (1u << 6)       // CONFIG mode: no phone on the SoftAP, Wi-Fi gets the radio alone

typedef struct {
    bool own_host; // true: this module initialises NimBLE (CONFIG mode)
    bool active;   // active scanning (scan requests, names from scan responses); passive otherwise
    uint16_t interval_ms;
    uint16_t window_ms;
    bool parse_names;
    uint32_t initial_pause; // SCAN_PAUSE_* bits set from the start
    uint32_t watchdog_s;    // 0: off
    ble_scanner_filter_t filter; // optional: reports from other addresses are dropped before parsing
    ble_scanner_sink_t sink;     // reports with an iBeacon or Eddystone-TLM frame or a name
} ble_scanner_config_t;

typedef enum {
    BLE_SCANNER_STOPPED = 0,
    BLE_SCANNER_WAIT_HOST, // NimBLE not (yet, or no longer) synced with the controller
    BLE_SCANNER_PAUSED,
    BLE_SCANNER_HOLD_OFF, // resuming after a short delay (network just up, commissioning just ended)
    BLE_SCANNER_BACKOFF,  // waiting before retrying after an error
    BLE_SCANNER_RUNNING,
} ble_scanner_state_t;

typedef struct {
    ble_scanner_state_t state;
    uint32_t pause_mask;
    uint32_t adv_reports;     // every advertising report received
    uint32_t ibeacon_reports; // well-formed iBeacon frames among them (after the filter)
    uint32_t tlm_reports;     // Eddystone-TLM frames among them (after the filter)
    uint32_t starts;
    uint32_t start_failures;
    uint32_t unexpected_stops; // discovery ended by the stack (host reset)
    uint32_t watchdog_restarts;
    int last_error; // NimBLE return code of the last failed start
    uint64_t scan_clock_ms;
    uint16_t interval_ms;
    uint16_t window_ms;
    bool active;
} ble_scanner_stats_t;

esp_err_t ble_scanner_start(const ble_scanner_config_t *cfg);

// Thread-safe, never block: callable from the Matter event callback and the web server.
void ble_scanner_pause(uint32_t reasons);
void ble_scanner_resume(uint32_t reasons);
// No scanning for the next ms milliseconds (extends, never shortens, a hold-off already set)
void ble_scanner_hold_off(uint32_t ms);

uint64_t ble_scanner_scan_clock_ms(void);
void ble_scanner_get_stats(ble_scanner_stats_t *out);
const char *ble_scanner_state_name(ble_scanner_state_t state);
// "not_commissioned,network_down" into buf
const char *ble_scanner_pause_text(uint32_t mask, char *buf, size_t size);
