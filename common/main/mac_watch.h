/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// Does the beacon of a slot keep its MAC address? A slot follows its beacon by MAC only, so a beacon that changes its
// address (privacy, "resolvable private address", usually every 15 minutes) is lost. The address bits are no proof
// (many beacons flag a fixed address as random with "private" bits): the beacon is watched instead, for 30 minutes of
// hearing it after it is assigned to the slot, in the NORMAL mode.
//
//   - Its iBeacon identity (UUID, major, minor) is taken from the scan list when the configuration is saved, or from
//     its first frame.
//   - Changed: that identity comes from another random address that appeared right when the slot's address went
//     silent (within MAC_WATCH_SEAMLESS_MS; or right when the previous such address went silent, for a beacon that
//     changes its address faster than the confirmation), the chain of them was heard for MAC_WATCH_CONFIRM_MS, and
//     the slot's address stayed silent for MAC_WATCH_GONE_MS. A beacon that changes its address never uses two at
//     once: an address heard before a frame of the slot's own address, or appearing long after the address before it
//     went silent, is another beacon with the same identity and never counts (the caller also leaves out the addresses
//     of the other slots). The own address heard again after a change: not a change after all, the watch goes on.
//   - Fixed: its address heard for MAC_WATCH_TIME_MS (gaps longer than MAC_WATCH_GONE_MS not counted) without a
//     change, or a public address (fixed by definition). The verdict is kept: the warning never shows again for this
//     MAC in this slot. A new MAC in the slot starts a new watch.
//
// Times are in scanning time (ble_scanner_scan_clock_ms()): a pause of the scanner is no silence. The records are
// kept in flash beside the configuration (config_store), so the watch survives restarts (switching to the CONFIG mode
// restarts the chip). No ESP-IDF dependency (host tests).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ble_addr.h"
#include "ibeacon_parser.h"

#define MAC_WATCH_TIME_MS (30u * 60u * 1000u)
#define MAC_WATCH_GONE_MS 60000
#define MAC_WATCH_SEAMLESS_MS 60000
#define MAC_WATCH_CONFIRM_MS 60000
#define MAC_WATCH_CANDIDATES 4 // other addresses with the identity tracked at once

typedef enum {
    MAC_WATCH_NONE = 0, // no beacon assigned
    MAC_WATCH_RUNNING,
    MAC_WATCH_FIXED,
    MAC_WATCH_CHANGED,
} mac_watch_state_t;

// Another address sending the identity (this boot)
typedef struct {
    uint8_t addr[BLE_ADDR_LEN];
    bool used;
    bool lookalike; // another beacon: heard before an own frame, or appeared long after the address before it went
                    // silent. Kept for the whole boot (its entry is never reused).
    int64_t last_ms;
    int64_t chain_ms; // first frame of the first address of its chain (each one following on from the one before)
} mac_watch_candidate_t;

typedef struct {
    // Kept in flash
    uint8_t mac[BLE_ADDR_LEN]; // the slot's beacon this record is about
    uint8_t state;             // mac_watch_state_t
    bool has_id;               // the identity below was read from its first iBeacon frame
    uint8_t uuid[IBEACON_UUID_LEN];
    uint16_t major;
    uint16_t minor;
    uint32_t watched_ms;           // time the beacon was heard
    uint8_t new_mac[BLE_ADDR_LEN]; // MAC_WATCH_CHANGED: the address its identity came from
    // This boot only
    int64_t last_own_ms; // its last frame from mac, 0: none yet
    mac_watch_candidate_t cand[MAC_WATCH_CANDIDATES];
} mac_watch_t;

typedef enum {
    MAC_WATCH_EV_NONE = 0,
    MAC_WATCH_EV_PROGRESS, // identity or time updated: saved now and then
    MAC_WATCH_EV_STATE,    // new state: logged and saved at once
} mac_watch_event_t;

// The slot is active with this MAC: a new watch unless the record is about this MAC already. True if reset.
bool mac_watch_assign(mac_watch_t *w, const uint8_t mac[BLE_ADDR_LEN]);
// The identity of the slot's beacon from the scan list, before any frame was heard in the NORMAL mode. True if set.
bool mac_watch_seed(mac_watch_t *w, const ibeacon_t *id);

// An iBeacon frame from the slot's own address (addr_type: over-the-air type)
mac_watch_event_t mac_watch_own(mac_watch_t *w, uint8_t addr_type, const ibeacon_t *id, int64_t now_ms);

// An iBeacon frame from another address that is no other slot's. since_ms: start of this boot's watching (the own
// address counts as heard then, so a restart is not a silence).
mac_watch_event_t mac_watch_other(mac_watch_t *w, const uint8_t addr[BLE_ADDR_LEN], uint8_t addr_type,
                                  const ibeacon_t *id, int64_t now_ms, int64_t since_ms);

// Needs the frames of every address: watching, identity known
bool mac_watch_needs_all(const mac_watch_t *w);

const char *mac_watch_state_name(uint8_t state); // "none", "running", "fixed", "changed"

// Stored form of n records. encode returns the size, 0 if buf is too small; decode resets records it cannot read and
// returns false when the whole blob is unusable (every record reset).
#define MAC_WATCH_BLOB_SIZE(n) ((size_t)8 + (size_t)(n) * 38 + 4)
size_t mac_watch_encode(const mac_watch_t *w, size_t n, uint8_t *buf, size_t size);
bool mac_watch_decode(const uint8_t *buf, size_t len, mac_watch_t *w, size_t n);
