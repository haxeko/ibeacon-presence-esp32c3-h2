/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// Bluetooth device addresses ("MAC"). No ESP-IDF dependency (host tests).
//
// Byte order: everywhere in this firmware an address is kept in display order, addr[0] being the first octet of
// "AA:BB:CC:DD:EE:FF" (the most significant one). NimBLE (ble_addr_t.val) and the HCI reports carry the address
// least significant octet first, so it is reversed exactly once, where a report enters the firmware
// (ble_addr_from_le()). Printing ble_addr_t.val in array order shows the address backwards.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define BLE_ADDR_LEN 6
#define BLE_ADDR_STR_SIZE 18 // "AA:BB:CC:DD:EE:FF" and the terminating NUL

// Over-the-air address types, as NimBLE reports them (BLE_ADDR_PUBLIC ... BLE_ADDR_RANDOM_ID)
#define BLE_OTA_ADDR_PUBLIC 0
#define BLE_OTA_ADDR_RANDOM 1
#define BLE_OTA_ADDR_PUBLIC_ID 2 // only with controller address resolution, not used here
#define BLE_OTA_ADDR_RANDOM_ID 3

// Address type stored with a slot: the exact type when the beacon was picked from the scan list, or "any" when
// the address was typed in and its type is unknown.
typedef enum {
    BEACON_ADDR_PUBLIC = 0,
    BEACON_ADDR_RANDOM = 1,
    BEACON_ADDR_ANY = 2,
} beacon_addr_type_t;

// What a received address is. Public and static random addresses stay the same; resolvable (RPA) and
// non-resolvable private addresses change every few minutes, so a slot cannot follow such a beacon by its
// address for long.
typedef enum {
    BLE_ADDR_KIND_PUBLIC = 0,
    BLE_ADDR_KIND_RANDOM_STATIC,
    BLE_ADDR_KIND_RANDOM_RESOLVABLE,
    BLE_ADDR_KIND_RANDOM_NON_RESOLVABLE,
    BLE_ADDR_KIND_RANDOM_RESERVED,
} ble_addr_kind_t;

// "AA:BB:CC:DD:EE:FF", "aa-bb-cc-dd-ee-ff" or "AABBCCDDEEFF", surrounding blanks allowed, one separator style per
// address. Returns false (out untouched) for anything else.
bool ble_addr_parse(const char *text, uint8_t out[BLE_ADDR_LEN]);

// Canonical form: upper case, colons.
void ble_addr_format(const uint8_t addr[BLE_ADDR_LEN], char out[BLE_ADDR_STR_SIZE]);

// NimBLE / HCI order (least significant octet first) <-> display order
void ble_addr_from_le(const uint8_t le[BLE_ADDR_LEN], uint8_t out[BLE_ADDR_LEN]);
void ble_addr_to_le(const uint8_t addr[BLE_ADDR_LEN], uint8_t le[BLE_ADDR_LEN]);

// Not 00:00:00:00:00:00 and not FF:FF:FF:FF:FF:FF
bool ble_addr_is_usable(const uint8_t addr[BLE_ADDR_LEN]);

// Kind of a received address, from its over-the-air type and, for random addresses, its two most significant bits.
ble_addr_kind_t ble_addr_kind(uint8_t ota_type, const uint8_t addr[BLE_ADDR_LEN]);
bool ble_addr_kind_is_stable(ble_addr_kind_t kind);
const char *ble_addr_kind_name(ble_addr_kind_t kind);

// Over-the-air type -> stored type (public or random)
beacon_addr_type_t beacon_addr_type_from_ota(uint8_t ota_type);
// Does a received type satisfy the type stored with a slot?
bool beacon_addr_type_matches(uint8_t stored_type, uint8_t ota_type);
const char *beacon_addr_type_name(uint8_t stored_type);
// "public", "random" or "any" -> stored type; false for anything else
bool beacon_addr_type_parse(const char *name, uint8_t *out);
