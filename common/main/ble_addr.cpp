/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "ble_addr.h"

#include <stdio.h>
#include <string.h>

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static bool is_blank(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

static bool parse_octet(const char *p, uint8_t *out)
{
    const int hi = hex_value(p[0]);
    const int lo = hex_value(p[1]);
    if (hi < 0 || lo < 0) {
        return false;
    }
    *out = static_cast<uint8_t>((hi << 4) | lo);
    return true;
}

bool ble_addr_parse(const char *text, uint8_t out[BLE_ADDR_LEN])
{
    if (text == nullptr) {
        return false;
    }
    while (is_blank(*text)) {
        text++;
    }
    size_t len = strlen(text);
    while (len > 0 && is_blank(text[len - 1])) {
        len--;
    }

    uint8_t addr[BLE_ADDR_LEN];
    if (len == 2 * BLE_ADDR_LEN) {
        for (int i = 0; i < BLE_ADDR_LEN; i++) {
            if (!parse_octet(&text[2 * i], &addr[i])) {
                return false;
            }
        }
    } else if (len == 3 * BLE_ADDR_LEN - 1) {
        const char sep = text[2];
        if (sep != ':' && sep != '-') {
            return false;
        }
        for (int i = 0; i < BLE_ADDR_LEN; i++) {
            const char *p = &text[3 * i];
            if (!parse_octet(p, &addr[i])) {
                return false;
            }
            if (i < BLE_ADDR_LEN - 1 && p[2] != sep) {
                return false;
            }
        }
    } else {
        return false;
    }
    memcpy(out, addr, BLE_ADDR_LEN);
    return true;
}

void ble_addr_format(const uint8_t addr[BLE_ADDR_LEN], char out[BLE_ADDR_STR_SIZE])
{
    snprintf(out, BLE_ADDR_STR_SIZE, "%02X:%02X:%02X:%02X:%02X:%02X", addr[0], addr[1], addr[2], addr[3], addr[4],
             addr[5]);
}

void ble_addr_from_le(const uint8_t le[BLE_ADDR_LEN], uint8_t out[BLE_ADDR_LEN])
{
    for (int i = 0; i < BLE_ADDR_LEN; i++) {
        out[i] = le[BLE_ADDR_LEN - 1 - i];
    }
}

void ble_addr_to_le(const uint8_t addr[BLE_ADDR_LEN], uint8_t le[BLE_ADDR_LEN])
{
    ble_addr_from_le(addr, le); // reversing is its own inverse
}

bool ble_addr_is_usable(const uint8_t addr[BLE_ADDR_LEN])
{
    bool all_zero = true;
    bool all_ff = true;
    for (int i = 0; i < BLE_ADDR_LEN; i++) {
        all_zero = all_zero && addr[i] == 0x00;
        all_ff = all_ff && addr[i] == 0xFF;
    }
    return !all_zero && !all_ff;
}

ble_addr_kind_t ble_addr_kind(uint8_t ota_type, const uint8_t addr[BLE_ADDR_LEN])
{
    switch (ota_type) {
    case BLE_OTA_ADDR_PUBLIC:
    case BLE_OTA_ADDR_PUBLIC_ID:
        return BLE_ADDR_KIND_PUBLIC;
    case BLE_OTA_ADDR_RANDOM_ID:
        return BLE_ADDR_KIND_RANDOM_STATIC; // identity address resolved by the controller
    default:
        break;
    }
    // Core Specification Vol 6 Part B 1.3.2: the two most significant bits of a random address give its sub-type.
    switch (addr[0] >> 6) {
    case 0x3:
        return BLE_ADDR_KIND_RANDOM_STATIC;
    case 0x1:
        return BLE_ADDR_KIND_RANDOM_RESOLVABLE;
    case 0x0:
        return BLE_ADDR_KIND_RANDOM_NON_RESOLVABLE;
    default:
        return BLE_ADDR_KIND_RANDOM_RESERVED;
    }
}

bool ble_addr_kind_is_stable(ble_addr_kind_t kind)
{
    return kind == BLE_ADDR_KIND_PUBLIC || kind == BLE_ADDR_KIND_RANDOM_STATIC;
}

const char *ble_addr_kind_name(ble_addr_kind_t kind)
{
    switch (kind) {
    case BLE_ADDR_KIND_PUBLIC:
        return "public";
    case BLE_ADDR_KIND_RANDOM_STATIC:
        return "static";
    case BLE_ADDR_KIND_RANDOM_RESOLVABLE:
        return "rpa";
    case BLE_ADDR_KIND_RANDOM_NON_RESOLVABLE:
        return "nrpa";
    default:
        return "reserved";
    }
}

beacon_addr_type_t beacon_addr_type_from_ota(uint8_t ota_type)
{
    return (ota_type == BLE_OTA_ADDR_PUBLIC || ota_type == BLE_OTA_ADDR_PUBLIC_ID) ? BEACON_ADDR_PUBLIC
                                                                                    : BEACON_ADDR_RANDOM;
}

bool beacon_addr_type_matches(uint8_t stored_type, uint8_t ota_type)
{
    switch (stored_type) {
    case BEACON_ADDR_ANY:
        return true;
    case BEACON_ADDR_PUBLIC:
    case BEACON_ADDR_RANDOM:
        return beacon_addr_type_from_ota(ota_type) == stored_type;
    default:
        return false;
    }
}

const char *beacon_addr_type_name(uint8_t stored_type)
{
    switch (stored_type) {
    case BEACON_ADDR_PUBLIC:
        return "public";
    case BEACON_ADDR_RANDOM:
        return "random";
    case BEACON_ADDR_ANY:
        return "any";
    default:
        return "invalid";
    }
}

bool beacon_addr_type_parse(const char *name, uint8_t *out)
{
    if (name == nullptr) {
        return false;
    }
    if (strcmp(name, "public") == 0) {
        *out = BEACON_ADDR_PUBLIC;
    } else if (strcmp(name, "random") == 0) {
        *out = BEACON_ADDR_RANDOM;
    } else if (strcmp(name, "any") == 0) {
        *out = BEACON_ADDR_ANY;
    } else {
        return false;
    }
    return true;
}
