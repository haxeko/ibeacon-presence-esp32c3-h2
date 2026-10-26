// Beacon configuration (main/presence_config.cpp): defaults, validation of every field, duplicate beacons, and
// the stored blob (round trip, corruption, unknown version, the five-slot version 1 read by every build).
// Built twice by run.sh: five slots (one-chip versions) and 20 (-DCONFIG_PRESENCE_ROLE_SCANNER=1, two-chip).
#include "test.h"

#include <stdlib.h>

#include "crc32.h"
#include "presence_config.h"

static presence_config_t valid_config()
{
    presence_config_t cfg;
    presence_config_set_defaults(&cfg);
    for (int i = 0; i < PRESENCE_SLOT_COUNT; i++) {
        presence_slot_config_t &s = cfg.slots[i];
        s.enabled = true;
        s.has_addr = true;
        const uint8_t addr[6] = { 0xC0, 0xFF, 0xEE, 0x00, 0x00, static_cast<uint8_t>(i + 1) };
        memcpy(s.addr, addr, 6);
        s.addr_type = i % 2 ? BEACON_ADDR_RANDOM : BEACON_ADDR_PUBLIC;
    }
    strcpy(cfg.slots[0].label, "Phòng khách"); // UTF-8
    strcpy(cfg.slots[1].model, "CP101");
    return cfg;
}

static bool has_error(const presence_config_error_t *errs, size_t n, int slot, uint8_t field, uint8_t code)
{
    for (size_t i = 0; i < n; i++) {
        if (errs[i].slot == slot && errs[i].field == field && errs[i].code == code) {
            return true;
        }
    }
    return false;
}

static void defaults()
{
    presence_config_t cfg;
    presence_config_set_defaults(&cfg);
    CHECK(cfg.scan_interval_ms == 160 && cfg.scan_window_ms == 40);
    CHECK(cfg.scan_window_ms < cfg.scan_interval_ms); // never the whole radio time by default
    for (int i = 0; i < PRESENCE_SLOT_COUNT; i++) {
        CHECK(!cfg.slots[i].enabled && !cfg.slots[i].has_addr);
        CHECK(cfg.slots[i].timeout_s == 45);
        CHECK(!cfg.slots[i].rssi_filter);
        CHECK(!presence_slot_is_active(&cfg.slots[i]));
    }
    CHECK_STR(cfg.slots[2].label, "Presence 3");
    CHECK_STR(cfg.slots[PRESENCE_SLOT_COUNT - 1].model, "");
    CHECK(presence_config_validate(&cfg, false, nullptr, 0) == 0);
}

static void valid_and_enabled_without_address()
{
    presence_config_t cfg = valid_config();
    CHECK(presence_config_validate(&cfg, false, nullptr, 0) == 0);
    // Enabled without an address is allowed: that slot simply reports "no presence"
    cfg.slots[3].has_addr = false;
    CHECK(presence_config_validate(&cfg, false, nullptr, 0) == 0);
    CHECK(!presence_slot_is_active(&cfg.slots[3]));
}

static void field_errors()
{
    presence_config_t cfg = valid_config();
    cfg.slots[0].timeout_s = 9;
    cfg.slots[1].timeout_s = 3601;
    cfg.slots[2].rssi_threshold = -101;
    cfg.slots[3].rssi_hysteresis = 21;
    cfg.slots[4].addr_type = 3;
    memset(cfg.slots[4].addr, 0, 6); // all zero
    cfg.scan_interval_ms = 19;
    presence_config_error_t errs[16];
    const size_t n = presence_config_validate(&cfg, false, errs, 16);
    CHECK(n == 7);
    CHECK(has_error(errs, n, 0, PRESENCE_FIELD_TIMEOUT, PRESENCE_ERR_TIMEOUT_RANGE));
    CHECK(has_error(errs, n, 1, PRESENCE_FIELD_TIMEOUT, PRESENCE_ERR_TIMEOUT_RANGE));
    CHECK(has_error(errs, n, 2, PRESENCE_FIELD_RSSI_THRESHOLD, PRESENCE_ERR_RSSI_RANGE));
    CHECK(has_error(errs, n, 3, PRESENCE_FIELD_RSSI_HYSTERESIS, PRESENCE_ERR_HYSTERESIS_RANGE));
    CHECK(has_error(errs, n, 4, PRESENCE_FIELD_ADDR_TYPE, PRESENCE_ERR_ADDR_TYPE));
    CHECK(has_error(errs, n, 4, PRESENCE_FIELD_ADDR, PRESENCE_ERR_ADDR_INVALID));
    CHECK(has_error(errs, n, -1, PRESENCE_FIELD_SCAN_INTERVAL, PRESENCE_ERR_SCAN_INTERVAL_RANGE));
    // Only max_errors are written, all are counted
    CHECK(presence_config_validate(&cfg, false, errs, 2) == 7);
}

static void scan_window_limits()
{
    presence_config_t cfg = valid_config();
    cfg.scan_window_ms = cfg.scan_interval_ms + 1;
    presence_config_error_t errs[4];
    size_t n = presence_config_validate(&cfg, false, errs, 4);
    CHECK(n == 1 && has_error(errs, n, -1, PRESENCE_FIELD_SCAN_WINDOW, PRESENCE_ERR_SCAN_WINDOW_ABOVE_INTERVAL));
    cfg.scan_window_ms = cfg.scan_interval_ms; // allowed when chosen explicitly
    CHECK(presence_config_validate(&cfg, false, nullptr, 0) == 0);
    cfg.scan_window_ms = 9;
    n = presence_config_validate(&cfg, false, errs, 4);
    CHECK(n == 1 && has_error(errs, n, -1, PRESENCE_FIELD_SCAN_WINDOW, PRESENCE_ERR_SCAN_WINDOW_RANGE));
}

static void labels()
{
    presence_config_t cfg = valid_config();
    strcpy(cfg.slots[1].label, "bad\x01name");
    strcpy(cfg.slots[2].label, "\xC3\x28"); // invalid UTF-8
    memset(cfg.slots[3].label, 'x', PRESENCE_LABEL_MAX_BYTES);
    cfg.slots[3].label[PRESENCE_LABEL_MAX_BYTES] = '\0'; // exactly the maximum: fine
    presence_config_error_t errs[8];
    const size_t n = presence_config_validate(&cfg, false, errs, 8);
    CHECK(n == 2);
    CHECK(has_error(errs, n, 1, PRESENCE_FIELD_LABEL, PRESENCE_ERR_LABEL_INVALID));
    CHECK(has_error(errs, n, 2, PRESENCE_FIELD_LABEL, PRESENCE_ERR_LABEL_INVALID));
    // No terminating NUL within the buffer: too long
    memset(cfg.slots[1].label, 'y', sizeof(cfg.slots[1].label));
    strcpy(cfg.slots[2].label, "ok");
    CHECK(presence_config_validate(&cfg, false, errs, 8) == 1);
    CHECK(errs[0].code == PRESENCE_ERR_LABEL_TOO_LONG);
}

static void duplicates()
{
    presence_config_t cfg = valid_config();
    memcpy(cfg.slots[3].addr, cfg.slots[1].addr, 6);
    cfg.slots[3].addr_type = cfg.slots[1].addr_type;
    presence_config_error_t errs[4];
    size_t n = presence_config_validate(&cfg, false, errs, 4);
    CHECK(n == 1);
    CHECK(errs[0].slot == 3 && errs[0].other_slot == 1 && errs[0].code == PRESENCE_ERR_ADDR_DUPLICATE);
    CHECK(presence_config_validate(&cfg, true, errs, 4) == 0); // allowed by Kconfig option

    // "any" conflicts with both types; public vs random with the same address bytes are different devices
    cfg.slots[3].addr_type = BEACON_ADDR_ANY;
    CHECK(presence_config_validate(&cfg, false, errs, 4) == 1);
    cfg.slots[1].addr_type = BEACON_ADDR_PUBLIC;
    cfg.slots[3].addr_type = BEACON_ADDR_RANDOM;
    CHECK(presence_config_validate(&cfg, false, errs, 4) == 0);

    // A disabled slot may keep the same address
    cfg.slots[3].addr_type = BEACON_ADDR_PUBLIC;
    cfg.slots[3].enabled = false;
    CHECK(presence_config_validate(&cfg, false, errs, 4) == 0);
}

static void blob_round_trip()
{
    presence_config_t cfg = valid_config();
    cfg.slots[2].rssi_filter = true;
    cfg.slots[2].rssi_threshold = -77;
    cfg.slots[2].rssi_hysteresis = 4;
    cfg.slots[4].timeout_s = 3600;
    cfg.scan_interval_ms = 300;
    cfg.scan_window_ms = 30;
    uint8_t blob[PRESENCE_CONFIG_BLOB_MAX_SIZE];
    const size_t len = presence_config_encode(&cfg, blob, sizeof(blob));
    CHECK(len > 0);
    presence_config_t out;
    memset(&out, 0, sizeof(out));
    CHECK(presence_config_decode(blob, len, &out) == PRESENCE_DECODE_OK);
    CHECK(presence_config_equal(&cfg, &out));
    CHECK(presence_config_encode(&cfg, blob, len - 1) == 0); // buffer too small
}

static void blob_corruption()
{
    presence_config_t cfg = valid_config();
    uint8_t blob[PRESENCE_CONFIG_BLOB_MAX_SIZE];
    const size_t len = presence_config_encode(&cfg, blob, sizeof(blob));
    presence_config_t out;
    presence_config_set_defaults(&out);
    const presence_config_t before = out;

    // Any single flipped bit is caught
    for (size_t i = 0; i < len; i++) {
        uint8_t copy[PRESENCE_CONFIG_BLOB_MAX_SIZE];
        memcpy(copy, blob, len);
        copy[i] ^= 0x10;
        CHECK(presence_config_decode(copy, len, &out) != PRESENCE_DECODE_OK);
    }
    CHECK(presence_config_equal(&out, &before)); // never partially written

    CHECK(presence_config_decode(blob, 5, &out) == PRESENCE_DECODE_TRUNCATED);
    CHECK(presence_config_decode(blob, len - 1, &out) == PRESENCE_DECODE_BAD_LENGTH);
    CHECK(presence_config_decode(nullptr, len, &out) == PRESENCE_DECODE_TRUNCATED);
}

// Rewrites a field of an encoded blob and fixes its CRC, so only the value check can catch it
static void patch(uint8_t *blob, size_t len, size_t offset, uint8_t value)
{
    blob[offset] = value;
    const uint32_t crc = crc32_ieee(blob, len - 4);
    for (int i = 0; i < 4; i++) {
        blob[len - 4 + i] = static_cast<uint8_t>(crc >> (8 * i));
    }
}

static void blob_values_checked_and_versions()
{
    presence_config_t cfg = valid_config();
    uint8_t blob[PRESENCE_CONFIG_BLOB_MAX_SIZE];
    const size_t len = presence_config_encode(&cfg, blob, sizeof(blob));
    presence_config_t out;

    uint8_t copy[PRESENCE_CONFIG_BLOB_MAX_SIZE];
    memcpy(copy, blob, len);
    patch(copy, len, 8 + 6 + 1, 9); // slot 0 address type 9
    CHECK(presence_config_decode(copy, len, &out) == PRESENCE_DECODE_BAD_VALUE);

    memcpy(copy, blob, len);
    patch(copy, len, 8 + 6 + 12, PRESENCE_LABEL_MAX_BYTES + 1); // slot 0 label length too big
    CHECK(presence_config_decode(copy, len, &out) == PRESENCE_DECODE_BAD_VALUE);

    memcpy(copy, blob, len);
    patch(copy, len, 8 + 6 + 61, PRESENCE_MODEL_MAX_BYTES + 1); // slot 0 model length too big
    CHECK(presence_config_decode(copy, len, &out) == PRESENCE_DECODE_BAD_VALUE);

    memcpy(copy, blob, len);
    patch(copy, len, 4, 4); // format version 4, written by a newer firmware
    CHECK(presence_config_decode(copy, len, &out) == PRESENCE_DECODE_UNSUPPORTED_VERSION);

    memcpy(copy, blob, len);
    patch(copy, len, 0, 'X');
    CHECK(presence_config_decode(copy, len, &out) == PRESENCE_DECODE_BAD_MAGIC);
}

static void models()
{
    presence_config_t cfg = valid_config();
    memset(cfg.slots[2].model, 'm', PRESENCE_MODEL_MAX_BYTES); // exactly the maximum: fine
    cfg.slots[2].model[PRESENCE_MODEL_MAX_BYTES] = '\0';
    strcpy(cfg.slots[3].model, "Bếp"); // UTF-8
    CHECK(presence_config_validate(&cfg, false, nullptr, 0) == 0);
    strcpy(cfg.slots[3].model, "bad\x02");
    memset(cfg.slots[4].model, 'z', sizeof(cfg.slots[4].model)); // no NUL within the buffer
    presence_config_error_t errs[4];
    const size_t n = presence_config_validate(&cfg, false, errs, 4);
    CHECK(n == 2);
    CHECK(has_error(errs, n, 3, PRESENCE_FIELD_MODEL, PRESENCE_ERR_MODEL_INVALID));
    CHECK(has_error(errs, n, 4, PRESENCE_FIELD_MODEL, PRESENCE_ERR_MODEL_TOO_LONG));
    CHECK_STR(presence_field_name(PRESENCE_FIELD_MODEL), "model");
}

static void put16(uint8_t *p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

static void seal(uint8_t *blob, size_t len)
{
    const uint32_t crc = crc32_ieee(blob, len - 4);
    for (int i = 0; i < 4; i++) {
        blob[len - 4 + i] = static_cast<uint8_t>(crc >> (8 * i));
    }
}

// Version 1 blob as the five-slot firmware wrote it (no model)
static size_t make_v1(uint8_t *blob)
{
    const size_t slot_size = 61;
    const size_t payload = 6 + 5 * slot_size;
    const size_t len = 8 + payload + 4;
    memset(blob, 0, len);
    memcpy(blob, "BPRC", 4);
    put16(&blob[4], 1);
    put16(&blob[6], static_cast<uint16_t>(payload));
    uint8_t *p = &blob[8];
    put16(p, 320);
    put16(p + 2, 64);
    p[4] = 5;
    p += 6;
    for (int i = 0; i < 5; i++, p += slot_size) {
        p[0] = i == 1 ? 0x03 : 0x00; // slot 1: enabled with an address
        p[1] = BEACON_ADDR_RANDOM;
        const uint8_t addr[6] = { 0xC0, 0x11, 0x22, 0x33, 0x44, static_cast<uint8_t>(0x50 + i) };
        memcpy(&p[2], addr, 6);
        put16(&p[8], static_cast<uint16_t>(60 + i));
        p[10] = static_cast<uint8_t>(-70);
        p[11] = 3;
        const char *label = i == 1 ? "Ví" : "";
        p[12] = static_cast<uint8_t>(strlen(label));
        memcpy(&p[13], label, strlen(label));
    }
    seal(blob, len);
    return len;
}

static void version_1_migrates()
{
    uint8_t blob[PRESENCE_CONFIG_BLOB_MAX_SIZE];
    const size_t len = make_v1(blob);
    presence_config_t out;
    CHECK(presence_config_decode(blob, len, &out) == PRESENCE_DECODE_OK);
    CHECK(out.scan_interval_ms == 320 && out.scan_window_ms == 64);
    CHECK(out.slots[1].enabled && out.slots[1].has_addr && presence_slot_is_active(&out.slots[1]));
    CHECK(out.slots[1].addr[5] == 0x51 && out.slots[1].addr_type == BEACON_ADDR_RANDOM);
    CHECK(out.slots[1].timeout_s == 61 && out.slots[1].rssi_threshold == -70 && out.slots[1].rssi_hysteresis == 3);
    CHECK_STR(out.slots[1].label, "Ví");
    CHECK_STR(out.slots[1].model, ""); // version 1 has none
    CHECK(!out.slots[0].enabled && out.slots[0].timeout_s == 60);
    for (int i = 5; i < PRESENCE_SLOT_COUNT; i++) {
        // Slots the old firmware did not have: defaults
        CHECK(!out.slots[i].enabled && !out.slots[i].has_addr && out.slots[i].timeout_s == 45);
    }
    // Saved again: version 2, the same configuration
    uint8_t blob2[PRESENCE_CONFIG_BLOB_MAX_SIZE];
    const size_t len2 = presence_config_encode(&out, blob2, sizeof(blob2));
    CHECK(len2 > 0 && blob2[4] == 2);
    presence_config_t again;
    CHECK(presence_config_decode(blob2, len2, &again) == PRESENCE_DECODE_OK);
    CHECK(presence_config_equal(&out, &again));

    // Payload length that does not match the slot count
    blob[8 + 4] = 4;
    seal(blob, len);
    CHECK(presence_config_decode(blob, len, &out) == PRESENCE_DECODE_BAD_LENGTH);
}

// A version 2 blob with more slots than this build (the two-chip scanner's 20 read by a five-slot firmware)
static void more_slots_than_this_firmware()
{
    const size_t slot_size = 61 + 1 + PRESENCE_MODEL_MAX_BYTES;
    const size_t count = PRESENCE_SLOT_COUNT + 2;
    const size_t payload = 6 + count * slot_size;
    const size_t len = 8 + payload + 4;
    uint8_t *blob = static_cast<uint8_t *>(calloc(1, len));
    memcpy(blob, "BPRC", 4);
    put16(&blob[4], 2);
    put16(&blob[6], static_cast<uint16_t>(payload));
    put16(&blob[8], 160);
    put16(&blob[10], 40);
    blob[12] = static_cast<uint8_t>(count);
    uint8_t *slots = &blob[8 + 6];
    for (size_t i = 0; i < count; i++) {
        put16(&slots[i * slot_size + 8], 45); // valid timeout
        slots[i * slot_size + 1] = BEACON_ADDR_ANY;
        slots[i * slot_size + 10] = static_cast<uint8_t>(-80);
    }
    // Slot 1 of this build: enabled with a beacon and a model
    const uint8_t addr[6] = { 0xC0, 0x11, 0x22, 0x33, 0x44, 0x55 };
    slots[slot_size] = 0x03;
    memcpy(&slots[slot_size + 2], addr, 6);
    slots[slot_size + 61] = 5;
    memcpy(&slots[slot_size + 62], "CP101", 5);
    // An extra slot that is enabled without a beacon, with a label: nothing is lost by dropping it
    const size_t extra = PRESENCE_SLOT_COUNT * slot_size;
    slots[extra] = 0x01;
    slots[extra + 12] = static_cast<uint8_t>(strlen("Bếp"));
    memcpy(&slots[extra + 13], "Bếp", strlen("Bếp"));
    seal(blob, len);
    presence_config_t out;
    presence_config_set_defaults(&out);
    CHECK(presence_config_decode(blob, len, &out) == PRESENCE_DECODE_OK);
    CHECK(out.scan_interval_ms == 160 && out.scan_window_ms == 40);
    CHECK(presence_slot_is_active(&out.slots[1]) && out.slots[1].addr[5] == 0x55);
    CHECK_STR(out.slots[1].model, "CP101");
    CHECK(!out.slots[0].enabled && !out.slots[PRESENCE_SLOT_COUNT - 1].has_addr);

    // A beacon assigned to an extra slot (even disabled): refused, *out untouched
    const presence_config_t before = out;
    slots[extra + slot_size] = 0x02;
    memcpy(&slots[extra + slot_size + 2], addr, 6);
    seal(blob, len);
    CHECK(presence_config_decode(blob, len, &out) == PRESENCE_DECODE_BAD_VALUE);
    CHECK(presence_config_equal(&out, &before));

    // An extra slot with a bad value: refused too (the blob is not one this format wrote)
    slots[extra + slot_size] = 0x00;
    put16(&slots[extra + slot_size + 8], 5); // timeout below the minimum
    seal(blob, len);
    CHECK(presence_config_decode(blob, len, &out) == PRESENCE_DECODE_BAD_VALUE);
    free(blob);
}

// Version 3 (firmware 1.0.201, beacons optionally recognised by iBeacon identity): read as version 2, every slot
// followed by its MAC; a slot with only an identity has no MAC (inactive)
static void version_3_read()
{
    const size_t slot_size = 61 + 1 + PRESENCE_MODEL_MAX_BYTES + 16 + 2 + 2;
    const size_t payload = 6 + PRESENCE_SLOT_COUNT * slot_size;
    const size_t len = 8 + payload + 4;
    CHECK(len <= PRESENCE_CONFIG_BLOB_MAX_SIZE);
    uint8_t *blob = static_cast<uint8_t *>(calloc(1, len));
    memcpy(blob, "BPRC", 4);
    put16(&blob[4], 3);
    put16(&blob[6], static_cast<uint16_t>(payload));
    put16(&blob[8], 160);
    put16(&blob[10], 40);
    blob[12] = PRESENCE_SLOT_COUNT;
    uint8_t *slots = &blob[8 + 6];
    for (size_t i = 0; i < PRESENCE_SLOT_COUNT; i++) {
        put16(&slots[i * slot_size + 8], 45);
        slots[i * slot_size + 1] = BEACON_ADDR_ANY;
        slots[i * slot_size + 10] = static_cast<uint8_t>(-80);
    }
    const uint8_t addr[6] = { 0x48, 0x87, 0x2D, 0x9D, 0xC1, 0xDF };
    slots[0] = 0x1B; // enabled, MAC, identity, recognised by identity
    memcpy(&slots[2], addr, 6);
    slots[61] = 5;
    memcpy(&slots[62], "CP101", 5);
    slots[61 + 1 + PRESENCE_MODEL_MAX_BYTES] = 0xE2; // identity: ignored
    slots[slot_size] = 0x19; // enabled, identity only
    seal(blob, len);
    presence_config_t out;
    CHECK(presence_config_decode(blob, len, &out) == PRESENCE_DECODE_OK);
    CHECK(presence_slot_is_active(&out.slots[0]) && memcmp(out.slots[0].addr, addr, 6) == 0);
    CHECK_STR(out.slots[0].model, "CP101");
    CHECK(out.slots[1].enabled && !out.slots[1].has_addr && !presence_slot_is_active(&out.slots[1]));
    // Saved again: version 2
    uint8_t again[PRESENCE_CONFIG_BLOB_MAX_SIZE];
    const size_t len2 = presence_config_encode(&out, again, sizeof(again));
    CHECK(len2 > 0 && again[4] == 2);
    free(blob);
}

static void error_texts()
{
    for (int code = PRESENCE_ERR_LABEL_TOO_LONG; code <= PRESENCE_ERR_MODEL_INVALID; code++) {
        CHECK(strcmp(presence_error_text(code), "Lỗi không xác định") != 0);
    }
    CHECK_STR(presence_field_name(PRESENCE_FIELD_ADDR), "mac");
}

int main()
{
    RUN(defaults);
    RUN(valid_and_enabled_without_address);
    RUN(field_errors);
    RUN(scan_window_limits);
    RUN(labels);
    RUN(duplicates);
    RUN(blob_round_trip);
    RUN(blob_corruption);
    RUN(blob_values_checked_and_versions);
    RUN(models);
    RUN(version_1_migrates);
    RUN(more_slots_than_this_firmware);
    RUN(version_3_read);
    RUN(error_texts);
    return test_summary(PRESENCE_SLOT_COUNT == 5 ? "presence_config" : "presence_config 20");
}
