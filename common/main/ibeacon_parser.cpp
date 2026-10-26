/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "ibeacon_parser.h"

#include <stdio.h>
#include <string.h>

#include "json_util.h"

static constexpr uint8_t kAdTypeShortName = 0x08;
static constexpr uint8_t kAdTypeCompleteName = 0x09;
static constexpr uint8_t kAdTypeServiceData16 = 0x16;
static constexpr uint8_t kAdTypeManufacturerData = 0xFF;
static constexpr size_t kIBeaconDataLen = 25; // company id 2 + type 1 + length 1 + 21
static constexpr size_t kTlmDataLen = 16;     // UUID 2 + frame type 1 + version 1 + 12

// Calls fn(type, payload, payload_len) for every AD structure until fn returns true. Returns false when the data
// ends (or is malformed) before that.
template <typename Fn> static bool for_each_ad(const uint8_t *data, size_t len, Fn fn)
{
    if (data == nullptr) {
        return false;
    }
    size_t i = 0;
    while (i < len) {
        const size_t field_len = data[i];
        if (field_len == 0) {
            return false; // zero padding ends the significant part
        }
        if (field_len > len - i - 1) {
            return false; // the structure would run past the received bytes
        }
        const uint8_t type = data[i + 1];
        if (fn(type, &data[i + 2], field_len - 1)) {
            return true;
        }
        i += 1 + field_len;
    }
    return false;
}

bool ibeacon_parse(const uint8_t *data, size_t len, ibeacon_t *out)
{
    return for_each_ad(data, len, [out](uint8_t type, const uint8_t *p, size_t plen) {
        if (type != kAdTypeManufacturerData || plen != kIBeaconDataLen) {
            return false;
        }
        if (p[0] != 0x4C || p[1] != 0x00 || p[2] != 0x02 || p[3] != 0x15) {
            return false;
        }
        memcpy(out->uuid, &p[4], IBEACON_UUID_LEN);
        out->major = static_cast<uint16_t>((p[20] << 8) | p[21]);
        out->minor = static_cast<uint16_t>((p[22] << 8) | p[23]);
        out->tx_power = static_cast<int8_t>(p[24]);
        return true;
    });
}

static uint16_t get_be16(const uint8_t *p)
{
    return static_cast<uint16_t>((p[0] << 8) | p[1]);
}

static uint32_t get_be32(const uint8_t *p)
{
    return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8) | p[3];
}

bool eddystone_tlm_parse(const uint8_t *data, size_t len, eddystone_tlm_t *out)
{
    return for_each_ad(data, len, [out](uint8_t type, const uint8_t *p, size_t plen) {
        // Longer: DX-SMART beacons append 6 bytes (ignored). Shorter: truncated, refused.
        if (type != kAdTypeServiceData16 || plen < kTlmDataLen) {
            return false;
        }
        if (p[0] != 0xAA || p[1] != 0xFE || p[2] != 0x20 || p[3] != 0x00) {
            return false;
        }
        out->battery_raw = get_be16(&p[4]);
        out->temperature_q8 = static_cast<int16_t>(get_be16(&p[6]));
        out->adv_count = get_be32(&p[8]);
        out->uptime_ds = get_be32(&p[12]);
        return true;
    });
}

static bool plausible_mv(uint16_t mv)
{
    return mv >= EDDYSTONE_TLM_MV_MIN && mv <= EDDYSTONE_TLM_MV_MAX;
}

uint16_t eddystone_tlm_battery_mv(const eddystone_tlm_t *tlm, uint8_t *order)
{
    const uint16_t big = tlm->battery_raw;
    const uint16_t little = static_cast<uint16_t>((big >> 8) | (big << 8));
    switch (*order) {
    case TLM_ORDER_BIG:
        return plausible_mv(big) ? big : 0;
    case TLM_ORDER_LITTLE:
        return plausible_mv(little) ? little : 0;
    default:
        break;
    }
    const bool big_ok = plausible_mv(big);
    const bool little_ok = plausible_mv(little);
    if (big_ok && little_ok) {
        return big == little ? big : 0; // e.g. 0x0B0C or 0x0C0B: wait for a reading that tells
    }
    if (big_ok) {
        *order = TLM_ORDER_BIG;
        return big;
    }
    if (little_ok) {
        *order = TLM_ORDER_LITTLE;
        return little;
    }
    return 0;
}

size_t adv_parse_name(const uint8_t *data, size_t len, char *out, size_t out_size)
{
    if (out_size == 0) {
        return 0;
    }
    out[0] = '\0';
    const uint8_t *short_name = nullptr;
    size_t short_len = 0;
    const uint8_t *complete_name = nullptr;
    size_t complete_len = 0;
    for_each_ad(data, len, [&](uint8_t type, const uint8_t *p, size_t plen) {
        if (type == kAdTypeCompleteName) {
            complete_name = p;
            complete_len = plen;
            return true;
        }
        if (type == kAdTypeShortName && short_name == nullptr) {
            short_name = p;
            short_len = plen;
        }
        return false;
    });
    if (complete_name != nullptr) {
        return utf8_sanitize(complete_name, complete_len, out, out_size);
    }
    if (short_name != nullptr) {
        return utf8_sanitize(short_name, short_len, out, out_size);
    }
    return 0;
}

void ibeacon_uuid_format(const uint8_t uuid[IBEACON_UUID_LEN], char out[IBEACON_UUID_STR_SIZE])
{
    snprintf(out, IBEACON_UUID_STR_SIZE,
             "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", uuid[0], uuid[1], uuid[2],
             uuid[3], uuid[4], uuid[5], uuid[6], uuid[7], uuid[8], uuid[9], uuid[10], uuid[11], uuid[12], uuid[13],
             uuid[14], uuid[15]);
}
