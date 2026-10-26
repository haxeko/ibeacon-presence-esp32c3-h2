/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// Presence decision of the slots. No ESP-IDF dependency (host tests); presence_manager.cpp runs it.
//
// Time is the scan clock (ble_scanner_scan_clock_ms()): milliseconds during which the radio was actually scanning.
// It stops while the firmware pauses the scanner (commissioning, network down, host reset, backoff), so an
// absence timeout only counts time in which the beacon could have been heard: pausing never turns a slot off.
//
//   - a valid packet (matching address and type, passing the RSSI filter when enabled) turns the slot on at once
//     and refreshes last_seen, identical repeated advertisements included
//   - no valid packet for timeout_ms of scan clock turns it off
//   - RSSI filter (off by default): a packet counts only if rssi >= threshold while the slot is off, and
//     rssi >= threshold - hysteresis while it is on. RSSI is not a distance: it varies by +-10 dB with the body,
//     orientation and walls, so the threshold only separates "near" from "far away" roughly.
//   - at boot every slot is off until a new packet arrives (nothing is restored from flash)

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "presence_config.h"

#define PRESENCE_LOGIC_SLOTS PRESENCE_SLOT_COUNT // at most 32: presence_logic_tick() returns a bit mask

typedef struct {
    bool active; // slot enabled and an address assigned; an inactive slot is always off
    uint32_t timeout_ms;
    bool rssi_filter;
    int8_t rssi_threshold;   // dBm
    uint8_t rssi_hysteresis; // dB
} presence_rule_t;

typedef struct {
    bool present;
    bool seen; // a valid packet arrived since boot
    uint64_t last_seen_ms; // scan clock
    int8_t last_rssi;
    uint32_t accepted;      // packets that refreshed the slot
    uint32_t rssi_rejected; // packets ignored by the RSSI filter
} presence_slot_state_t;

typedef struct {
    presence_rule_t rules[PRESENCE_LOGIC_SLOTS];
    presence_slot_state_t state[PRESENCE_LOGIC_SLOTS];
} presence_logic_t;

void presence_logic_init(presence_logic_t *pl, const presence_rule_t rules[PRESENCE_LOGIC_SLOTS]);

// A packet of the slot's beacon at scan clock now_ms. Returns true when the slot turned on.
bool presence_logic_on_packet(presence_logic_t *pl, int slot, int8_t rssi, uint64_t now_ms);

// Applies the absence timeouts at scan clock now_ms. Returns the bit mask of the slots that turned off.
uint32_t presence_logic_tick(presence_logic_t *pl, uint64_t now_ms);

// Scan clock time since the last valid packet (0 if never seen)
uint64_t presence_logic_age_ms(const presence_logic_t *pl, int slot, uint64_t now_ms);
