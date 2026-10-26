/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// Beacon configuration of the presence slots: defaults, validation and the stored format. No ESP-IDF dependency
// besides sdkconfig.h (host tests); config_store.cpp keeps it in flash.
//
// Slots: 5 in the one-chip versions (the five fixed Occupancy Sensor endpoints), 20 in the two-chip version (one
// bridged device each on the ESP32-H2, presence_link_proto.h).
//
// Stored format (one NVS blob, all integers little endian, version 2):
//   0   4  magic "BPRC"
//   4   2  format version
//   6   2  payload length
//   8   .. payload: scan interval ms (2), scan window ms (2), slot count (1), reserved (1),
//              then per slot: flags (1: bit0 enabled, bit1 address set, bit2 RSSI filter), address type (1),
//              address (6, display order), timeout s (2), RSSI threshold (1, signed), hysteresis (1),
//              label length (1), label (PRESENCE_LABEL_MAX_BYTES, zero padded),
//              model length (1), model (PRESENCE_MODEL_MAX_BYTES, zero padded)
//   end 4  CRC-32 of everything before it
// Version 1 is the same without the model, written by the firmware with five slots: it still decodes (its slots
// become the first ones, the others take the defaults) and is written as version 2 at the next save. Version 3
// (firmware 1.0.201, beacons optionally recognised by iBeacon UUID + major + minor) is version 2 with 20 more bytes per
// slot (UUID, major, minor) and two more flags: it decodes as version 2, every slot followed by its MAC (a slot that
// had only an iBeacon identity has no MAC, so it is inactive until a MAC is assigned). A blob with more
// slots than the firmware (the two-chip scanner's 20 read by a five-slot version) decodes when no beacon is assigned
// to the extra ones (they are dropped), else it is refused and kept in flash until the configuration is saved.
// The whole configuration is one blob written with one nvs_set_blob() + nvs_commit(): NVS keeps the old blob
// until the new one is complete, so a power cut leaves either the old or the new configuration, never half of
// the slots. A newer firmware that changes the layout bumps the version and keeps a decoder for each older one
// (presence_config_decode()), converting it in memory; it is written in the new format only when saved again.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sdkconfig.h"

#include "ble_addr.h"

#if defined(CONFIG_PRESENCE_ROLE_SCANNER) || defined(CONFIG_PRESENCE_ROLE_MATTER_UART)
#define PRESENCE_SLOT_COUNT 20 // two-chip version: one bridged device per slot on the ESP32-H2
#else
#define PRESENCE_SLOT_COUNT 5 // one-chip versions: the five fixed Occupancy Sensor endpoints
#endif
#define PRESENCE_LABEL_MAX_BYTES 48 // UTF-8 bytes; Vietnamese letters take 2-3 bytes
// Model of the beacon (two-chip version: ProductName of its bridged device, "Model" in Apple Home), normally its
// advertised name. Matter limits ProductName to 32 bytes.
#define PRESENCE_MODEL_MAX_BYTES 32

#define PRESENCE_TIMEOUT_MIN_S 10
#define PRESENCE_TIMEOUT_MAX_S 3600
#define PRESENCE_RSSI_THRESHOLD_MIN (-100)
#define PRESENCE_RSSI_THRESHOLD_MAX (-20)
#define PRESENCE_RSSI_HYSTERESIS_MAX 20
#define PRESENCE_SCAN_INTERVAL_MIN_MS 20
#define PRESENCE_SCAN_INTERVAL_MAX_MS 10240
#define PRESENCE_SCAN_WINDOW_MIN_MS 10

typedef struct {
    bool enabled;
    bool has_addr;
    uint8_t addr[BLE_ADDR_LEN]; // display order
    uint8_t addr_type;          // beacon_addr_type_t
    uint16_t timeout_s;
    bool rssi_filter;
    int8_t rssi_threshold;   // dBm
    uint8_t rssi_hysteresis; // dB
    char label[PRESENCE_LABEL_MAX_BYTES + 1]; // internal name shown on the web page and in the logs
    char model[PRESENCE_MODEL_MAX_BYTES + 1]; // "" if unknown
} presence_slot_config_t;

typedef struct {
    uint16_t scan_interval_ms; // NORMAL mode scanning
    uint16_t scan_window_ms;
    presence_slot_config_t slots[PRESENCE_SLOT_COUNT];
} presence_config_t;

void presence_config_set_defaults(presence_config_t *cfg);
void presence_config_default_label(int slot, char *out, size_t size);
bool presence_config_equal(const presence_config_t *a, const presence_config_t *b);
// Enabled with an address: the only slots that can ever report presence
bool presence_slot_is_active(const presence_slot_config_t *slot);
// Two active slots that would both follow the same beacon (same address, types equal or one of them "any")
bool presence_slots_conflict(const presence_slot_config_t *a, const presence_slot_config_t *b);

// ---------------------------------------------------------------------------------------------------------------------
// Validation

typedef enum {
    PRESENCE_FIELD_NONE = 0,
    PRESENCE_FIELD_ENABLED,
    PRESENCE_FIELD_LABEL,
    PRESENCE_FIELD_ADDR,
    PRESENCE_FIELD_ADDR_TYPE,
    PRESENCE_FIELD_TIMEOUT,
    PRESENCE_FIELD_RSSI_FILTER,
    PRESENCE_FIELD_RSSI_THRESHOLD,
    PRESENCE_FIELD_RSSI_HYSTERESIS,
    PRESENCE_FIELD_SCAN_INTERVAL,
    PRESENCE_FIELD_SCAN_WINDOW,
    PRESENCE_FIELD_MODEL,
} presence_field_t;

typedef enum {
    PRESENCE_ERR_LABEL_TOO_LONG = 1,
    PRESENCE_ERR_LABEL_INVALID,
    PRESENCE_ERR_ADDR_INVALID,
    PRESENCE_ERR_ADDR_DUPLICATE,
    PRESENCE_ERR_ADDR_TYPE,
    PRESENCE_ERR_TIMEOUT_RANGE,
    PRESENCE_ERR_RSSI_RANGE,
    PRESENCE_ERR_HYSTERESIS_RANGE,
    PRESENCE_ERR_SCAN_INTERVAL_RANGE,
    PRESENCE_ERR_SCAN_WINDOW_RANGE,
    PRESENCE_ERR_SCAN_WINDOW_ABOVE_INTERVAL,
    PRESENCE_ERR_TYPE, // wrong JSON type or missing field (web_server.cpp)
    PRESENCE_ERR_MODEL_TOO_LONG,
    PRESENCE_ERR_MODEL_INVALID,
} presence_error_code_t;

typedef struct {
    int8_t slot;       // 0 to PRESENCE_SLOT_COUNT - 1, -1 for the scan settings
    uint8_t field;     // presence_field_t
    uint8_t code;      // presence_error_code_t
    int8_t other_slot; // the other slot of a duplicate address, else -1
} presence_config_error_t;

// Checks every field and, unless allow_duplicate_beacons, that no beacon is assigned to two active slots.
// Writes up to max_errors errors and returns how many there are in total.
size_t presence_config_validate(const presence_config_t *cfg, bool allow_duplicate_beacons,
                                presence_config_error_t *errors, size_t max_errors);
const char *presence_error_text(uint8_t code); // Vietnamese, for the web page
const char *presence_field_name(uint8_t field); // JSON field name

// ---------------------------------------------------------------------------------------------------------------------
// Stored format

#define PRESENCE_CONFIG_FORMAT_VERSION 2
#define PRESENCE_CONFIG_BLOB_MAX_SIZE 2560 // also a version 3 blob of 20 slots (2298 bytes)

typedef enum {
    PRESENCE_DECODE_OK = 0,
    PRESENCE_DECODE_TRUNCATED,
    PRESENCE_DECODE_BAD_MAGIC,
    PRESENCE_DECODE_UNSUPPORTED_VERSION,
    PRESENCE_DECODE_BAD_LENGTH,
    PRESENCE_DECODE_BAD_CRC,
    PRESENCE_DECODE_BAD_VALUE,
} presence_decode_result_t;

// Returns the blob size, 0 if buf is too small.
size_t presence_config_encode(const presence_config_t *cfg, uint8_t *buf, size_t size);
// *out is only written when the whole blob is valid.
presence_decode_result_t presence_config_decode(const uint8_t *buf, size_t len, presence_config_t *out);
const char *presence_decode_result_name(presence_decode_result_t result);
