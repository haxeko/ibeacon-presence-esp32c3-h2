/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// Frames of the UART link of the two-chip version (ESP32-C3 scanner <-> ESP32-H2 Matter node). No ESP-IDF
// dependency (host tests).
//
//   0xA5 0x5A | version | type | length | payload (length bytes) | CRC-32 of version..payload (little endian)
//
// The line also carries garbage: each chip's ROM prints its boot messages on its UART0 TX pin, and the link can use
// those pins. The parser hunts for the two magic bytes, checks the length and the CRC, and starts hunting again
// after any error, so garbage only costs counted errors (a damaged length byte can also swallow the next frames, up
// to PLINK_MAX_PAYLOAD + 4 bytes).
// Frames repeat (every second or every few seconds), so none needs to be resent; COMMAND is sent three times and
// acted on once. At 115200 baud the busiest second carries about 250 bytes (STATE, STATUS, a few SLOT frames).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// 2: known_mask in STATE. 3: the Matter node is a bridge (SLOTS, PAIRING, COMMAND, bridged endpoints in STATUS).
// 4: 20 slots (32-bit masks), one SLOT frame per slot with the model and battery of its beacon, STATUS with the
// waiting devices, commissioning window and heap of the Matter node.
// Both chips need firmware of the same version: frames of another version are counted as bad frames and ignored.
#define PLINK_VERSION 5
#define PLINK_MAX_PAYLOAD 64
#define PLINK_MAX_FRAME (2 + 3 + PLINK_MAX_PAYLOAD + 4)
#define PLINK_SLOT_COUNT 20 // = PRESENCE_SLOT_COUNT of the two-chip version (checked where both are known)
#define PLINK_SLOT_MASK ((1u << PLINK_SLOT_COUNT) - 1)
#define PLINK_MAC_LEN 6
#define PLINK_MODEL_MAX 32 // bytes of UTF-8, as Matter's ProductName

typedef enum {
    PLINK_TYPE_STATE = 1,   // scanner -> Matter node, every second and on change
    PLINK_TYPE_STATUS = 2,  // Matter node -> scanner, every second
    PLINK_TYPE_SLOT = 3,    // scanner -> Matter node: one device of the bridge, each slot every 5 s, all after a save
    PLINK_TYPE_PAIRING = 4, // Matter node -> scanner: onboarding codes, every few seconds
    PLINK_TYPE_COMMAND = 5, // scanner -> Matter node: an action asked on the configuration page
} plink_type_t;

#define PLINK_MODE_NORMAL 0 // presence computed: present_mask is valid for the slots of known_mask
#define PLINK_MODE_CONFIG 1 // web configuration running: no presence, the Matter node keeps its states

#define PLINK_STATE_SCANNING 0x01 // the radio is scanning (not paused, not in backoff)

// Sent by the scanner every second and right after a change. Masks: bit n is slot n.
typedef struct {
    uint8_t mode;          // PLINK_MODE_*
    uint8_t flags;         // PLINK_STATE_*
    uint32_t active_mask;  // slot has a beacon assigned and is enabled
    uint32_t present_mask; // slot present (only bits of active slots can be set)
    uint32_t known_mask;   // slot's state is known; the Matter node keeps the state of the others (the bits of
                           // inactive slots always read as known: they are off)
    uint32_t uptime_s;
} plink_state_t;

#define PLINK_STATUS_MATTER_STARTED 0x01
#define PLINK_STATUS_COMMISSIONED 0x02
#define PLINK_STATUS_NETWORK_UP 0x04  // Thread attached
#define PLINK_STATUS_WINDOW_OPEN 0x08 // commissioning window open (pairing mode)

// Sent by the Matter node every second. A device waits for memory: before commissioning the memory is kept for it
// (added once commissioned), after it there is not enough free; error_mask: it could not be created (retried later,
// see the Matter node's log).
typedef struct {
    uint8_t flags; // PLINK_STATUS_*
    uint8_t fabrics;
    uint32_t uptime_s;
    uint32_t bridged_mask;               // slot has its device on the bridge
    uint32_t waiting_mask;               // slot should have a device that is not there yet
    uint32_t error_mask;                 // waiting slots whose device could not be created
    uint16_t heap_free_kb;               // free heap of the Matter node now, and its lowest since boot
    uint16_t heap_min_kb;
    uint16_t endpoint[PLINK_SLOT_COUNT]; // Matter endpoint of the slot's device, 0 if none
} plink_status_t;

#define PLINK_BATTERY_UNKNOWN 0xFF

// One device of the bridge: an active slot (enabled, beacon assigned) is an Occupancy Sensor named after its MAC,
// with the model and battery of its beacon; an inactive slot has none. Sent only from a configuration read from
// flash, so a scanner with nothing saved (or unreadable storage) never empties the bridge; an explicit save does.
typedef struct {
    uint8_t slot;
    bool active;
    // model typed on the configuration page; else it is the name the beacon advertises (or "": not known yet), and
    // an empty one only clears a model that was typed before
    bool model_configured;
    uint8_t mac[PLINK_MAC_LEN];      // display order; zero for an inactive slot
    uint8_t battery_pct;             // 0-100, PLINK_BATTERY_UNKNOWN (no Eddystone-TLM frame heard yet)
    uint8_t battery_level;           // Matter BatChargeLevel: 0 OK, 1 warning, 2 critical
    uint16_t battery_mv;             // 0: unknown
    char model[PLINK_MODEL_MAX + 1]; // UTF-8, "" if unknown
} plink_slot_t;

#define PLINK_MANUAL_CODE_LEN 11 // digits of the Matter manual pairing code
#define PLINK_QR_CODE_MAX 30     // "MT:" and the base-38 payload

typedef struct {
    char manual_code[PLINK_MANUAL_CODE_LEN + 1]; // "34970112332"
    char qr_code[PLINK_QR_CODE_MAX + 1];         // "MT:Y.K9042C00KA0648G00"
} plink_pairing_t;

#define PLINK_CMD_FACTORY_RESET 1 // remove the Matter pairing (fabrics, Thread network) and the bridged devices
#define PLINK_CMD_OPEN_WINDOW 2   // open the commissioning window (pairing mode), as a double click on its BOOT

// Travels with a fixed check word: with the CRC, a damaged or foreign frame is never taken for a command
typedef struct {
    uint8_t command; // PLINK_CMD_*
} plink_command_t;

// What the bridge does with a slot when its SLOT frame arrives
typedef enum {
    PLINK_SLOT_KEEP = 0, // nothing to change
    PLINK_SLOT_ADD,      // new device
    PLINK_SLOT_REPLACE,  // another beacon in the slot: remove the old device, add the new one
    PLINK_SLOT_REMOVE,   // slot no longer active: remove its device
} plink_slot_action_t;

plink_slot_action_t plink_slot_action(bool have, const uint8_t have_mac[PLINK_MAC_LEN], bool want,
                                      const uint8_t want_mac[PLINK_MAC_LEN]);

// "C0:11:22:33:44:01" (18 bytes with the NUL): the name and serial number of a bridged device
#define PLINK_MAC_STR_SIZE 18
void plink_mac_string(const uint8_t mac[PLINK_MAC_LEN], char out[PLINK_MAC_STR_SIZE]);

// Scanner: the known_mask of its STATE frames. A slot's state is known once it was reported (it turned on, or off
// after being on: reported_mask) or once the scanner listened for the slot's whole absence timeout (scan clock, ms)
// without hearing its beacon; inactive slots are known (off). Until then the Matter node keeps the state it has, so
// a restart of the scanner (end of the CONFIG mode, a reset) never turns a sensor off by itself. A scanner that
// cannot scan at all (BLE failure: the scan clock stands still) still declares the slot known, as absent, once its
// uptime passes the timeout by PLINK_UNKNOWN_GRACE_MS: a stale "occupied" does not last forever.
#define PLINK_UNKNOWN_GRACE_MS (5u * 60u * 1000u)
uint32_t plink_known_mask(uint32_t active_mask, uint32_t reported_mask, const uint32_t *timeout_ms, int slots,
                          uint64_t scan_clock_ms, uint64_t uptime_ms);

// Frame into out; returns its length, 0 if it does not fit or the payload is too long
size_t plink_encode(uint8_t type, const uint8_t *payload, size_t len, uint8_t *out, size_t out_size);
size_t plink_encode_state(const plink_state_t *state, uint8_t *out, size_t out_size);
size_t plink_encode_status(const plink_status_t *status, uint8_t *out, size_t out_size);
size_t plink_encode_slot(const plink_slot_t *slot, uint8_t *out, size_t out_size);
size_t plink_encode_pairing(const plink_pairing_t *pairing, uint8_t *out, size_t out_size);
size_t plink_encode_command(const plink_command_t *command, uint8_t *out, size_t out_size);

// Payload of a received frame; false if its length does not match the type or a field is out of range
bool plink_decode_state(const uint8_t *payload, size_t len, plink_state_t *state);
bool plink_decode_status(const uint8_t *payload, size_t len, plink_status_t *status);
bool plink_decode_slot(const uint8_t *payload, size_t len, plink_slot_t *slot);
bool plink_decode_pairing(const uint8_t *payload, size_t len, plink_pairing_t *pairing);
bool plink_decode_command(const uint8_t *payload, size_t len, plink_command_t *command);

typedef struct {
    uint8_t buf[PLINK_MAX_FRAME];
    size_t have; // bytes of the current frame received so far (magic included)
    uint8_t type;
    uint8_t len;
    const uint8_t *payload; // valid after plink_parser_feed() returned true, until the next byte
    uint32_t frames;
    uint32_t crc_errors;
    uint32_t bad_frames; // unknown version, length too long
} plink_parser_t;

void plink_parser_init(plink_parser_t *p);
// One received byte; true when it completes a valid frame (type, len, payload set)
bool plink_parser_feed(plink_parser_t *p, uint8_t byte);
