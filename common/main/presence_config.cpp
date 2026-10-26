/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "presence_config.h"

#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "crc32.h"
#include "json_util.h"

static const uint8_t kMagic[4] = { 'B', 'P', 'R', 'C' };
static constexpr size_t kHeaderSize = 8;
static constexpr size_t kCrcSize = 4;
static constexpr size_t kScanSize = 2 + 2 + 1 + 1;
static constexpr size_t kSlotSizeV1 = 1 + 1 + BLE_ADDR_LEN + 2 + 1 + 1 + 1 + PRESENCE_LABEL_MAX_BYTES;
static constexpr size_t kSlotSizeV2 = kSlotSizeV1 + 1 + PRESENCE_MODEL_MAX_BYTES;
static constexpr size_t kSlotSizeV3 = kSlotSizeV2 + 16 + 2 + 2; // read only: iBeacon UUID, major, minor ignored
static constexpr size_t kPayloadSize = kScanSize + PRESENCE_SLOT_COUNT * kSlotSizeV2;
static_assert(kHeaderSize + kPayloadSize + kCrcSize <= PRESENCE_CONFIG_BLOB_MAX_SIZE, "blob buffer too small");

static constexpr uint8_t kFlagEnabled = 1u << 0;
static constexpr uint8_t kFlagHasAddr = 1u << 1;
static constexpr uint8_t kFlagRssiFilter = 1u << 2;

void presence_config_default_label(int slot, char *out, size_t size)
{
    snprintf(out, size, "Presence %d", slot + 1);
}

void presence_config_set_defaults(presence_config_t *cfg)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->scan_interval_ms = CONFIG_PRESENCE_SCAN_INTERVAL_MS;
    cfg->scan_window_ms = CONFIG_PRESENCE_SCAN_WINDOW_MS;
    for (int i = 0; i < PRESENCE_SLOT_COUNT; i++) {
        presence_slot_config_t &s = cfg->slots[i];
        s.enabled = false;
        s.has_addr = false;
        s.addr_type = BEACON_ADDR_ANY;
        s.timeout_s = CONFIG_PRESENCE_DEFAULT_TIMEOUT_SEC;
        s.rssi_filter = false;
        s.rssi_threshold = CONFIG_PRESENCE_DEFAULT_RSSI_THRESHOLD;
        s.rssi_hysteresis = CONFIG_PRESENCE_DEFAULT_RSSI_HYSTERESIS;
        presence_config_default_label(i, s.label, sizeof(s.label));
    }
}

static bool slot_equal(const presence_slot_config_t &a, const presence_slot_config_t &b)
{
    return a.enabled == b.enabled && a.has_addr == b.has_addr && memcmp(a.addr, b.addr, BLE_ADDR_LEN) == 0 &&
           a.addr_type == b.addr_type && a.timeout_s == b.timeout_s && a.rssi_filter == b.rssi_filter &&
           a.rssi_threshold == b.rssi_threshold && a.rssi_hysteresis == b.rssi_hysteresis &&
           strcmp(a.label, b.label) == 0 && strcmp(a.model, b.model) == 0;
}

bool presence_config_equal(const presence_config_t *a, const presence_config_t *b)
{
    if (a->scan_interval_ms != b->scan_interval_ms || a->scan_window_ms != b->scan_window_ms) {
        return false;
    }
    for (int i = 0; i < PRESENCE_SLOT_COUNT; i++) {
        if (!slot_equal(a->slots[i], b->slots[i])) {
            return false;
        }
    }
    return true;
}

bool presence_slot_is_active(const presence_slot_config_t *slot)
{
    return slot->enabled && slot->has_addr;
}

bool presence_slots_conflict(const presence_slot_config_t *a, const presence_slot_config_t *b)
{
    if (!presence_slot_is_active(a) || !presence_slot_is_active(b)) {
        return false;
    }
    if (memcmp(a->addr, b->addr, BLE_ADDR_LEN) != 0) {
        return false;
    }
    return a->addr_type == b->addr_type || a->addr_type == BEACON_ADDR_ANY || b->addr_type == BEACON_ADDR_ANY;
}

// ---------------------------------------------------------------------------------------------------------------------
// Validation

namespace {

struct ErrorSink {
    presence_config_error_t *errors;
    size_t max;
    size_t count;

    void add(int slot, presence_field_t field, presence_error_code_t code, int other_slot = -1)
    {
        if (errors != nullptr && count < max) {
            errors[count] = { static_cast<int8_t>(slot), static_cast<uint8_t>(field), static_cast<uint8_t>(code),
                              static_cast<int8_t>(other_slot) };
        }
        count++;
    }
};

} // namespace

static void check_scan(uint16_t interval_ms, uint16_t window_ms, ErrorSink &sink)
{
    const bool interval_ok = interval_ms >= PRESENCE_SCAN_INTERVAL_MIN_MS && interval_ms <= PRESENCE_SCAN_INTERVAL_MAX_MS;
    if (!interval_ok) {
        sink.add(-1, PRESENCE_FIELD_SCAN_INTERVAL, PRESENCE_ERR_SCAN_INTERVAL_RANGE);
    }
    if (window_ms < PRESENCE_SCAN_WINDOW_MIN_MS || window_ms > PRESENCE_SCAN_INTERVAL_MAX_MS) {
        sink.add(-1, PRESENCE_FIELD_SCAN_WINDOW, PRESENCE_ERR_SCAN_WINDOW_RANGE);
    } else if (interval_ok && window_ms > interval_ms) {
        // Not reported against an interval that is itself out of range: one error is enough to fix
        sink.add(-1, PRESENCE_FIELD_SCAN_WINDOW, PRESENCE_ERR_SCAN_WINDOW_ABOVE_INTERVAL);
    }
}

// The field ranges of one slot
static void check_slot(int i, const presence_slot_config_t &s, ErrorSink &sink)
{
    const size_t label_len = strnlen(s.label, sizeof(s.label));
    if (label_len > PRESENCE_LABEL_MAX_BYTES) {
        sink.add(i, PRESENCE_FIELD_LABEL, PRESENCE_ERR_LABEL_TOO_LONG);
    } else if (!utf8_is_valid_text(s.label, label_len)) {
        sink.add(i, PRESENCE_FIELD_LABEL, PRESENCE_ERR_LABEL_INVALID);
    }
    const size_t model_len = strnlen(s.model, sizeof(s.model));
    if (model_len > PRESENCE_MODEL_MAX_BYTES) {
        sink.add(i, PRESENCE_FIELD_MODEL, PRESENCE_ERR_MODEL_TOO_LONG);
    } else if (!utf8_is_valid_text(s.model, model_len)) {
        sink.add(i, PRESENCE_FIELD_MODEL, PRESENCE_ERR_MODEL_INVALID);
    }
    if (s.has_addr && !ble_addr_is_usable(s.addr)) {
        sink.add(i, PRESENCE_FIELD_ADDR, PRESENCE_ERR_ADDR_INVALID);
    }
    if (s.addr_type > BEACON_ADDR_ANY) {
        sink.add(i, PRESENCE_FIELD_ADDR_TYPE, PRESENCE_ERR_ADDR_TYPE);
    }
    if (s.timeout_s < PRESENCE_TIMEOUT_MIN_S || s.timeout_s > PRESENCE_TIMEOUT_MAX_S) {
        sink.add(i, PRESENCE_FIELD_TIMEOUT, PRESENCE_ERR_TIMEOUT_RANGE);
    }
    if (s.rssi_threshold < PRESENCE_RSSI_THRESHOLD_MIN || s.rssi_threshold > PRESENCE_RSSI_THRESHOLD_MAX) {
        sink.add(i, PRESENCE_FIELD_RSSI_THRESHOLD, PRESENCE_ERR_RSSI_RANGE);
    }
    if (s.rssi_hysteresis > PRESENCE_RSSI_HYSTERESIS_MAX) {
        sink.add(i, PRESENCE_FIELD_RSSI_HYSTERESIS, PRESENCE_ERR_HYSTERESIS_RANGE);
    }
}

size_t presence_config_validate(const presence_config_t *cfg, bool allow_duplicate_beacons,
                                presence_config_error_t *errors, size_t max_errors)
{
    ErrorSink sink{ errors, max_errors, 0 };
    check_scan(cfg->scan_interval_ms, cfg->scan_window_ms, sink);
    for (int i = 0; i < PRESENCE_SLOT_COUNT; i++) {
        const presence_slot_config_t &s = cfg->slots[i];
        check_slot(i, s, sink);
        if (!allow_duplicate_beacons) {
            for (int j = 0; j < i; j++) {
                if (presence_slots_conflict(&cfg->slots[j], &s)) {
                    sink.add(i, PRESENCE_FIELD_ADDR, PRESENCE_ERR_ADDR_DUPLICATE, j);
                    break;
                }
            }
        }
    }
    return sink.count;
}

const char *presence_error_text(uint8_t code)
{
    switch (code) {
    case PRESENCE_ERR_LABEL_TOO_LONG:
        return "Tên quá dài (tối đa 48 byte UTF-8)";
    case PRESENCE_ERR_LABEL_INVALID:
        return "Tên chứa ký tự không hợp lệ";
    case PRESENCE_ERR_ADDR_INVALID:
        return "Địa chỉ MAC không hợp lệ (dạng AA:BB:CC:DD:EE:FF, không được toàn 00 hoặc FF)";
    case PRESENCE_ERR_ADDR_DUPLICATE:
        return "Beacon này đã được gán cho một slot khác đang bật";
    case PRESENCE_ERR_ADDR_TYPE:
        return "Loại địa chỉ phải là public, random hoặc any";
    case PRESENCE_ERR_TIMEOUT_RANGE:
        return "Thời gian vắng mặt phải từ 10 đến 3600 giây";
    case PRESENCE_ERR_RSSI_RANGE:
        return "Ngưỡng RSSI phải từ -100 đến -20 dBm";
    case PRESENCE_ERR_HYSTERESIS_RANGE:
        return "Độ trễ RSSI phải từ 0 đến 20 dB";
    case PRESENCE_ERR_SCAN_INTERVAL_RANGE:
        return "Chu kỳ quét phải từ 20 đến 10240 ms";
    case PRESENCE_ERR_SCAN_WINDOW_RANGE:
        return "Cửa sổ quét phải từ 10 đến 10240 ms";
    case PRESENCE_ERR_SCAN_WINDOW_ABOVE_INTERVAL:
        return "Cửa sổ quét không được lớn hơn chu kỳ quét";
    case PRESENCE_ERR_TYPE:
        return "Thiếu trường hoặc sai kiểu dữ liệu";
    case PRESENCE_ERR_MODEL_TOO_LONG:
        return "Model quá dài (tối đa 32 byte UTF-8)";
    case PRESENCE_ERR_MODEL_INVALID:
        return "Model chứa ký tự không hợp lệ";
    default:
        return "Lỗi không xác định";
    }
}

const char *presence_field_name(uint8_t field)
{
    switch (field) {
    case PRESENCE_FIELD_ENABLED:
        return "enabled";
    case PRESENCE_FIELD_LABEL:
        return "label";
    case PRESENCE_FIELD_ADDR:
        return "mac";
    case PRESENCE_FIELD_ADDR_TYPE:
        return "addr_type";
    case PRESENCE_FIELD_TIMEOUT:
        return "timeout_s";
    case PRESENCE_FIELD_RSSI_FILTER:
        return "rssi_filter";
    case PRESENCE_FIELD_RSSI_THRESHOLD:
        return "rssi_threshold";
    case PRESENCE_FIELD_RSSI_HYSTERESIS:
        return "rssi_hysteresis";
    case PRESENCE_FIELD_SCAN_INTERVAL:
        return "interval_ms";
    case PRESENCE_FIELD_SCAN_WINDOW:
        return "window_ms";
    case PRESENCE_FIELD_MODEL:
        return "model";
    default:
        return "";
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Stored format

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++) {
        p[i] = static_cast<uint8_t>(v >> (8 * i));
    }
}

static uint16_t get_u16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

static uint32_t get_u32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

size_t presence_config_encode(const presence_config_t *cfg, uint8_t *buf, size_t size)
{
    const size_t total = kHeaderSize + kPayloadSize + kCrcSize;
    if (buf == nullptr || size < total) {
        return 0;
    }
    memset(buf, 0, total);
    memcpy(buf, kMagic, sizeof(kMagic));
    put_u16(&buf[4], PRESENCE_CONFIG_FORMAT_VERSION);
    put_u16(&buf[6], kPayloadSize);

    uint8_t *p = &buf[kHeaderSize];
    put_u16(p, cfg->scan_interval_ms);
    put_u16(p + 2, cfg->scan_window_ms);
    p[4] = PRESENCE_SLOT_COUNT;
    p[5] = 0;
    p += kScanSize;
    for (int i = 0; i < PRESENCE_SLOT_COUNT; i++) {
        const presence_slot_config_t &s = cfg->slots[i];
        const size_t label_len = strnlen(s.label, PRESENCE_LABEL_MAX_BYTES);
        const size_t model_len = strnlen(s.model, PRESENCE_MODEL_MAX_BYTES);
        p[0] = (s.enabled ? kFlagEnabled : 0) | (s.has_addr ? kFlagHasAddr : 0) | (s.rssi_filter ? kFlagRssiFilter : 0);
        p[1] = s.addr_type;
        memcpy(&p[2], s.addr, BLE_ADDR_LEN);
        put_u16(&p[8], s.timeout_s);
        p[10] = static_cast<uint8_t>(s.rssi_threshold);
        p[11] = s.rssi_hysteresis;
        p[12] = static_cast<uint8_t>(label_len);
        memcpy(&p[13], s.label, label_len);
        p[kSlotSizeV1] = static_cast<uint8_t>(model_len);
        memcpy(&p[kSlotSizeV1 + 1], s.model, model_len);
        p += kSlotSizeV2;
    }
    put_u32(&buf[total - kCrcSize], crc32_ieee(buf, total - kCrcSize));
    return total;
}

// A text field: length byte, then max bytes zero padded. false if too long or with a NUL inside.
static bool get_text(const uint8_t *p, size_t max, char *out)
{
    const uint8_t len = p[0];
    if (len > max) {
        return false;
    }
    memcpy(out, &p[1], len);
    out[len] = '\0';
    return memchr(out, '\0', len) == nullptr;
}

// One slot of the blob. false if a text field is too long or holds a NUL.
static bool parse_slot(const uint8_t *p, size_t slot_size, presence_slot_config_t *s)
{
    s->enabled = (p[0] & kFlagEnabled) != 0;
    s->has_addr = (p[0] & kFlagHasAddr) != 0;
    s->rssi_filter = (p[0] & kFlagRssiFilter) != 0;
    s->addr_type = p[1];
    memcpy(s->addr, &p[2], BLE_ADDR_LEN);
    s->timeout_s = get_u16(&p[8]);
    s->rssi_threshold = static_cast<int8_t>(p[10]);
    s->rssi_hysteresis = p[11];
    if (!get_text(&p[12], PRESENCE_LABEL_MAX_BYTES, s->label)) {
        return false;
    }
    if (slot_size >= kSlotSizeV2) {
        return get_text(&p[kSlotSizeV1], PRESENCE_MODEL_MAX_BYTES, s->model);
    }
    s->model[0] = '\0'; // version 1 has none
    return true;
}

// Versions 1 (no model), 2 and 3 (2 + iBeacon identity, ignored). slot_size: kSlotSizeV1, V2 or V3. The blob may
// hold fewer slots than this firmware (version 1: five): the others keep the defaults. More (the 20 slots of the
// two-chip scanner read by a five-slot firmware): the extra ones are dropped if no beacon is assigned to them, else
// the blob is refused, so no assigned beacon is lost without notice.
// Checked slot by slot first, then decoded into *out: no whole configuration (2 KB with 20 slots) on the stack.
static presence_decode_result_t decode_slots(const uint8_t *payload, size_t payload_len, size_t slot_size,
                                             presence_config_t *out)
{
    if (payload_len < kScanSize) {
        return PRESENCE_DECODE_BAD_LENGTH;
    }
    const size_t count = payload[4];
    if (payload_len != kScanSize + count * slot_size) {
        return PRESENCE_DECODE_BAD_LENGTH;
    }
    const uint16_t interval_ms = get_u16(payload);
    const uint16_t window_ms = get_u16(payload + 2);
    const uint8_t *slots = payload + kScanSize;
    // Field ranges only: duplicates were allowed or refused when the configuration was saved.
    ErrorSink sink{ nullptr, 0, 0 };
    check_scan(interval_ms, window_ms, sink);
    presence_slot_config_t slot;
    for (size_t i = 0; i < count && sink.count == 0; i++) {
        if (!parse_slot(slots + i * slot_size, slot_size, &slot) ||
            (i >= PRESENCE_SLOT_COUNT && slot.has_addr)) {
            return PRESENCE_DECODE_BAD_VALUE;
        }
        check_slot(static_cast<int>(i), slot, sink);
    }
    if (sink.count != 0) {
        return PRESENCE_DECODE_BAD_VALUE;
    }
    presence_config_set_defaults(out);
    out->scan_interval_ms = interval_ms;
    out->scan_window_ms = window_ms;
    for (size_t i = 0; i < count && i < PRESENCE_SLOT_COUNT; i++) {
        parse_slot(slots + i * slot_size, slot_size, &out->slots[i]);
    }
    return PRESENCE_DECODE_OK;
}

presence_decode_result_t presence_config_decode(const uint8_t *buf, size_t len, presence_config_t *out)
{
    if (buf == nullptr || len < kHeaderSize + kCrcSize) {
        return PRESENCE_DECODE_TRUNCATED;
    }
    if (memcmp(buf, kMagic, sizeof(kMagic)) != 0) {
        return PRESENCE_DECODE_BAD_MAGIC;
    }
    const uint16_t version = get_u16(&buf[4]);
    const uint16_t payload_len = get_u16(&buf[6]);
    if (len != kHeaderSize + payload_len + kCrcSize) {
        return PRESENCE_DECODE_BAD_LENGTH;
    }
    if (get_u32(&buf[len - kCrcSize]) != crc32_ieee(buf, len - kCrcSize)) {
        return PRESENCE_DECODE_BAD_CRC;
    }
    switch (version) {
    case 1:
        return decode_slots(&buf[kHeaderSize], payload_len, kSlotSizeV1, out);
    case 2:
        return decode_slots(&buf[kHeaderSize], payload_len, kSlotSizeV2, out);
    case 3:
        return decode_slots(&buf[kHeaderSize], payload_len, kSlotSizeV3, out);
    default:
        // Written by a newer firmware: keep it untouched in flash and run with defaults until saved again.
        return PRESENCE_DECODE_UNSUPPORTED_VERSION;
    }
}

const char *presence_decode_result_name(presence_decode_result_t result)
{
    switch (result) {
    case PRESENCE_DECODE_OK:
        return "ok";
    case PRESENCE_DECODE_TRUNCATED:
        return "truncated";
    case PRESENCE_DECODE_BAD_MAGIC:
        return "bad magic";
    case PRESENCE_DECODE_UNSUPPORTED_VERSION:
        return "unsupported version";
    case PRESENCE_DECODE_BAD_LENGTH:
        return "bad length";
    case PRESENCE_DECODE_BAD_CRC:
        return "bad CRC";
    case PRESENCE_DECODE_BAD_VALUE:
        return "bad value";
    default:
        return "?";
    }
}
