// UART link frames of the two-chip version (main/presence_link_proto.cpp): round trip, every corrupted byte and
// every truncation rejected, resynchronisation after garbage such as ROM boot messages, frames of the earlier
// protocol versions refused.
#include "test.h"

#include <stdlib.h>

#include "crc32.h"
#include "presence_link_proto.h"

static int feed_all(plink_parser_t *p, const uint8_t *data, size_t len, uint8_t *last_type)
{
    int frames = 0;
    for (size_t i = 0; i < len; i++) {
        if (plink_parser_feed(p, data[i])) {
            frames++;
            *last_type = p->type;
        }
    }
    return frames;
}

static void state_round_trip()
{
    const plink_state_t in = { PLINK_MODE_NORMAL, PLINK_STATE_SCANNING, 0x8001F, 0x80005, 0x80007, 123456 };
    uint8_t frame[PLINK_MAX_FRAME];
    const size_t n = plink_encode_state(&in, frame, sizeof(frame));
    CHECK(n == 2 + 3 + 18 + 4);
    plink_parser_t p;
    plink_parser_init(&p);
    uint8_t type = 0;
    CHECK(feed_all(&p, frame, n, &type) == 1);
    CHECK(type == PLINK_TYPE_STATE);
    plink_state_t out;
    CHECK(plink_decode_state(p.payload, p.len, &out));
    CHECK(out.mode == in.mode && out.flags == in.flags && out.active_mask == 0x8001F && out.present_mask == 0x80005);
    // Slots 0-2 and 19 known, 3-4 not yet; the inactive slots are known (off); no bit beyond slot 19
    CHECK(out.known_mask == 0xFFFE7);
    CHECK(out.uptime_s == 123456);
    plink_status_t wrong;
    CHECK(!plink_decode_status(p.payload, p.len, &wrong)); // a state frame is not a status
}

static void status_round_trip()
{
    const plink_status_t in = { PLINK_STATUS_MATTER_STARTED | PLINK_STATUS_COMMISSIONED, 2, 42 };
    uint8_t frame[PLINK_MAX_FRAME];
    const size_t n = plink_encode_status(&in, frame, sizeof(frame));
    plink_parser_t p;
    plink_parser_init(&p);
    uint8_t type = 0;
    CHECK(feed_all(&p, frame, n, &type) == 1 && type == PLINK_TYPE_STATUS);
    plink_status_t out;
    CHECK(plink_decode_status(p.payload, p.len, &out));
    CHECK(out.flags == in.flags && out.fabrics == 2 && out.uptime_s == 42);
}

static void present_only_for_active_slots()
{
    const plink_state_t in = { PLINK_MODE_NORMAL, 0, 0x03, 0xFFFFFFFF, 0x00, 0 };
    uint8_t frame[PLINK_MAX_FRAME];
    const size_t n = plink_encode_state(&in, frame, sizeof(frame));
    plink_parser_t p;
    plink_parser_init(&p);
    uint8_t type = 0;
    CHECK(feed_all(&p, frame, n, &type) == 1);
    plink_state_t out;
    CHECK(plink_decode_state(p.payload, p.len, &out) && out.present_mask == 0x03);
    CHECK(out.known_mask == 0xFFFFC); // the active slots are not known yet, the inactive ones are off

    // Bits beyond the 20 slots are dropped on both sides
    const plink_state_t wide = { PLINK_MODE_NORMAL, 0, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0 };
    plink_parser_init(&p);
    CHECK(feed_all(&p, frame, plink_encode_state(&wide, frame, sizeof(frame)), &type) == 1);
    CHECK(plink_decode_state(p.payload, p.len, &out));
    CHECK(out.active_mask == PLINK_SLOT_MASK && out.present_mask == PLINK_SLOT_MASK && out.known_mask == PLINK_SLOT_MASK);
    uint8_t raw[18] = {};
    raw[2] = raw[3] = raw[4] = raw[5] = 0xFF; // active: every bit
    raw[6] = raw[7] = raw[8] = raw[9] = 0xFF;
    CHECK(plink_decode_state(raw, sizeof(raw), &out) && out.active_mask == PLINK_SLOT_MASK && out.present_mask == PLINK_SLOT_MASK);
}

// Firmware of an earlier protocol (version 1: no known_mask, 2: no bridge, 3: five slots, 4: one wait reason for
// every device) on the other chip: its frames are counted as bad and never applied, whatever their CRC
static void other_version_rejected()
{
    for (uint8_t version = 1; version <= 6; version++) {
        if (version == PLINK_VERSION) {
            continue;
        }
        const plink_state_t in = { PLINK_MODE_NORMAL, 0, 0x01, 0x01, 0x01, 3 };
        uint8_t frame[PLINK_MAX_FRAME];
        const size_t n = plink_encode_state(&in, frame, sizeof(frame));
        frame[2] = version;
        const uint32_t crc = crc32_ieee(&frame[2], n - 6);
        for (int i = 0; i < 4; i++) {
            frame[n - 4 + i] = static_cast<uint8_t>(crc >> (8 * i));
        }
        plink_parser_t p;
        plink_parser_init(&p);
        uint8_t type = 0;
        CHECK(feed_all(&p, frame, n, &type) == 0);
        CHECK(p.bad_frames == 1 && p.crc_errors == 0);
    }
    // Payloads of the earlier versions would not decode either: version 3 state (9 bytes), status (17 bytes),
    // version 1 state (8 bytes), version 2 status (6 bytes)
    plink_state_t out;
    const uint8_t v3_state[9] = { PLINK_MODE_NORMAL, 0, 0x01, 0x01, 0x01, 3, 0, 0, 0 };
    CHECK(!plink_decode_state(v3_state, sizeof(v3_state), &out));
    CHECK(!plink_decode_state(v3_state, 8, &out));
    plink_status_t status;
    const uint8_t v3_status[17] = { PLINK_STATUS_COMMISSIONED, 1 };
    CHECK(!plink_decode_status(v3_status, sizeof(v3_status), &status));
    CHECK(!plink_decode_status(v3_status, 6, &status));
}

// One frame through the parser; returns its payload length, -1 if no frame came out
static int through_parser(const uint8_t *frame, size_t n, plink_parser_t *p, uint8_t expected_type)
{
    plink_parser_init(p);
    uint8_t type = 0;
    if (feed_all(p, frame, n, &type) != 1 || type != expected_type) {
        return -1;
    }
    return p->len;
}

static void status_with_bridge()
{
    plink_status_t in = {};
    in.flags = PLINK_STATUS_MATTER_STARTED | PLINK_STATUS_COMMISSIONED | PLINK_STATUS_NETWORK_UP;
    in.fabrics = 1;
    in.uptime_s = 77;
    in.bridged_mask = 0x80005; // slots 0, 2 and 19 on the bridge
    in.waiting_mask = 0x0000A | 0x80000; // slots 1 and 3 waiting (19 is bridged: never waiting)
    in.error_mask = 0x00008 | 0x00100;   // slot 3 could not be created (8 is not waiting: dropped)
    in.heap_free_kb = 41;
    in.heap_min_kb = 23;
    in.endpoint[0] = 2;
    in.endpoint[1] = 9; // not bridged: decodes as 0
    in.endpoint[2] = 0x1234;
    in.endpoint[19] = 40;
    uint8_t frame[PLINK_MAX_FRAME];
    const size_t n = plink_encode_status(&in, frame, sizeof(frame));
    plink_parser_t p;
    CHECK(through_parser(frame, n, &p, PLINK_TYPE_STATUS) == 62);
    plink_status_t out;
    CHECK(plink_decode_status(p.payload, p.len, &out));
    CHECK(out.bridged_mask == 0x80005 && out.endpoint[0] == 2 && out.endpoint[1] == 0 && out.endpoint[2] == 0x1234);
    CHECK(out.endpoint[19] == 40 && out.endpoint[3] == 0 && out.fabrics == 1 && out.uptime_s == 77);
    CHECK(out.waiting_mask == 0x0000A && out.error_mask == 0x00008);
    CHECK(out.heap_free_kb == 41 && out.heap_min_kb == 23);
}

static void slot_round_trip()
{
    plink_slot_t in = {};
    in.slot = 19;
    in.active = true;
    const uint8_t a[6] = { 0xC0, 0x11, 0x22, 0x33, 0x44, 0x01 };
    memcpy(in.mac, a, 6);
    in.battery_pct = 87;
    in.battery_level = 0;
    in.battery_mv = 2950;
    in.model_configured = true;
    strcpy(in.model, "CP101 Bếp"); // UTF-8
    uint8_t frame[PLINK_MAX_FRAME];
    const size_t n = plink_encode_slot(&in, frame, sizeof(frame));
    const size_t model_len = strlen(in.model);
    CHECK(n == 2 + 3 + 13 + model_len + 4);
    plink_parser_t p;
    CHECK(through_parser(frame, n, &p, PLINK_TYPE_SLOT) == static_cast<int>(13 + model_len));
    plink_slot_t out;
    CHECK(plink_decode_slot(p.payload, p.len, &out));
    CHECK(out.slot == 19 && out.active && memcmp(out.mac, a, 6) == 0);
    CHECK(out.battery_pct == 87 && out.battery_level == 0 && out.battery_mv == 2950);
    CHECK_STR(out.model, "CP101 Bếp");
    CHECK(out.model_configured);

    // A name the beacon advertises: not the configured model
    plink_slot_t heard = in;
    heard.model_configured = false;
    CHECK(through_parser(frame, plink_encode_slot(&heard, frame, sizeof(frame)), &p, PLINK_TYPE_SLOT) > 0);
    CHECK(plink_decode_slot(p.payload, p.len, &out) && !out.model_configured && strcmp(out.model, "CP101 Bếp") == 0);

    // Longest model: fits the frame
    memset(in.model, 'M', PLINK_MODEL_MAX);
    in.model[PLINK_MODEL_MAX] = '\0';
    CHECK(through_parser(frame, plink_encode_slot(&in, frame, sizeof(frame)), &p, PLINK_TYPE_SLOT) ==
          13 + PLINK_MODEL_MAX);
    CHECK(plink_decode_slot(p.payload, p.len, &out) && strlen(out.model) == PLINK_MODEL_MAX);

    // Inactive slot: no MAC, model or battery travel
    plink_slot_t off = in;
    off.slot = 4;
    off.active = false;
    CHECK(through_parser(frame, plink_encode_slot(&off, frame, sizeof(frame)), &p, PLINK_TYPE_SLOT) == 13);
    CHECK(plink_decode_slot(p.payload, p.len, &out));
    const uint8_t zero[6] = {};
    CHECK(out.slot == 4 && !out.active && memcmp(out.mac, zero, 6) == 0 && out.model[0] == '\0');
    CHECK(out.battery_pct == PLINK_BATTERY_UNKNOWN && out.battery_mv == 0);

    // Battery unknown; an empty model is never "configured"
    in.battery_pct = PLINK_BATTERY_UNKNOWN;
    in.battery_mv = 0;
    strcpy(in.model, "");
    CHECK(through_parser(frame, plink_encode_slot(&in, frame, sizeof(frame)), &p, PLINK_TYPE_SLOT) == 13);
    CHECK(plink_decode_slot(p.payload, p.len, &out) && out.battery_pct == PLINK_BATTERY_UNKNOWN);
    CHECK(!out.model_configured);

    // The encoder refuses a slot beyond the 20 and a model without its NUL
    plink_slot_t bad = in;
    bad.slot = PLINK_SLOT_COUNT;
    CHECK(plink_encode_slot(&bad, frame, sizeof(frame)) == 0);
    bad = in;
    memset(bad.model, 'x', sizeof(bad.model));
    CHECK(plink_encode_slot(&bad, frame, sizeof(frame)) == 0);
}

// Every field of a SLOT frame is checked: the model becomes a Matter string, the slot an index
static void slot_decode_checks()
{
    plink_slot_t in = {};
    in.slot = 2;
    in.active = true;
    const uint8_t a[6] = { 0xC0, 0x11, 0x22, 0x33, 0x44, 0x01 };
    memcpy(in.mac, a, 6);
    in.battery_pct = 50;
    strcpy(in.model, "CP101");
    uint8_t frame[PLINK_MAX_FRAME];
    plink_parser_t p;
    const int len = through_parser(frame, plink_encode_slot(&in, frame, sizeof(frame)), &p, PLINK_TYPE_SLOT);
    CHECK(len == 18);
    uint8_t good[PLINK_MAX_PAYLOAD];
    memcpy(good, p.payload, len);
    plink_slot_t out;
    uint8_t b[PLINK_MAX_PAYLOAD];

    memcpy(b, good, len);
    b[0] = PLINK_SLOT_COUNT; // slot index out of range
    CHECK(!plink_decode_slot(b, len, &out));
    memcpy(b, good, len);
    b[1] = 0x05; // unknown flag
    CHECK(!plink_decode_slot(b, len, &out));
    memcpy(b, good, len);
    b[1] = 0x03; // active, model configured
    CHECK(plink_decode_slot(b, len, &out) && out.model_configured);
    memcpy(b, good, len);
    b[8] = 101; // battery above 100 %
    CHECK(!plink_decode_slot(b, len, &out));
    memcpy(b, good, len);
    b[9] = 3; // charge level
    CHECK(!plink_decode_slot(b, len, &out));
    memcpy(b, good, len);
    b[12] = 6; // model length beyond the payload
    CHECK(!plink_decode_slot(b, len, &out));
    memcpy(b, good, len);
    b[13 + 2] = 0x01; // control character in the model
    CHECK(!plink_decode_slot(b, len, &out));
    memcpy(b, good, len);
    b[13 + 2] = 0xC3; // invalid UTF-8
    CHECK(!plink_decode_slot(b, len, &out));
    memcpy(b, good, len);
    b[1] = 0x00; // inactive with a model
    CHECK(!plink_decode_slot(b, len, &out));
    CHECK(!plink_decode_slot(good, 12, &out)); // shorter than the fixed part
    CHECK(!plink_decode_slot(good, len - 1, &out));
    CHECK(plink_decode_slot(good, len, &out) && out.slot == 2);
}

static void pairing_round_trip()
{
    plink_pairing_t in = {};
    strcpy(in.manual_code, "34970112332");
    strcpy(in.qr_code, "MT:Y.K9042C00KA0648G00");
    uint8_t frame[PLINK_MAX_FRAME];
    const size_t n = plink_encode_pairing(&in, frame, sizeof(frame));
    plink_parser_t p;
    CHECK(through_parser(frame, n, &p, PLINK_TYPE_PAIRING) == 12 + 22);
    plink_pairing_t out;
    CHECK(plink_decode_pairing(p.payload, p.len, &out));
    CHECK_STR(out.manual_code, "34970112332");
    CHECK_STR(out.qr_code, "MT:Y.K9042C00KA0648G00");

    // Never anything but digits in the manual code and base-38 in the QR code: the page shows them as they come
    uint8_t payload[PLINK_MAX_PAYLOAD];
    memcpy(payload, p.payload, p.len);
    payload[3] = 'x';
    CHECK(!plink_decode_pairing(payload, p.len, &out));
    memcpy(payload, p.payload, p.len);
    payload[12 + 5] = '<';
    CHECK(!plink_decode_pairing(payload, p.len, &out));
    memcpy(payload, p.payload, p.len);
    payload[11] = 23; // QR length beyond the payload
    CHECK(!plink_decode_pairing(payload, p.len, &out));
    CHECK(!plink_decode_pairing(p.payload, 11, &out));
    // No QR code (not known yet) is fine
    plink_pairing_t only_manual = {};
    strcpy(only_manual.manual_code, "34970112332");
    CHECK(through_parser(frame, plink_encode_pairing(&only_manual, frame, sizeof(frame)), &p, PLINK_TYPE_PAIRING) == 12);
    CHECK(plink_decode_pairing(p.payload, p.len, &out) && out.qr_code[0] == '\0');
    // The encoder refuses a manual code of the wrong length
    plink_pairing_t wrong = {};
    strcpy(wrong.manual_code, "1234");
    CHECK(plink_encode_pairing(&wrong, frame, sizeof(frame)) == 0);
}

static void command_round_trip()
{
    const plink_command_t in = { PLINK_CMD_FACTORY_RESET };
    uint8_t frame[PLINK_MAX_FRAME];
    const size_t n = plink_encode_command(&in, frame, sizeof(frame));
    plink_parser_t p;
    CHECK(through_parser(frame, n, &p, PLINK_TYPE_COMMAND) == 5);
    plink_command_t out = {};
    CHECK(plink_decode_command(p.payload, p.len, &out) && out.command == PLINK_CMD_FACTORY_RESET);
    const plink_command_t open = { PLINK_CMD_OPEN_WINDOW };
    plink_parser_t q0;
    CHECK(through_parser(frame, plink_encode_command(&open, frame, sizeof(frame)), &q0, PLINK_TYPE_COMMAND) == 5);
    CHECK(plink_decode_command(q0.payload, q0.len, &out) && out.command == PLINK_CMD_OPEN_WINDOW);
    plink_encode_command(&in, frame, sizeof(frame));
    // Without the check word it is not a command
    uint8_t payload[5];
    memcpy(payload, p.payload, 5);
    payload[4] ^= 1;
    CHECK(!plink_decode_command(payload, 5, &out));
    CHECK(!plink_decode_command(p.payload, 4, &out));
    // Every corrupted byte of a command frame is rejected by the parser
    for (size_t i = 0; i < n; i++) {
        uint8_t bad[PLINK_MAX_FRAME];
        memcpy(bad, frame, n);
        bad[i] ^= 0x10;
        plink_parser_t q;
        plink_parser_init(&q);
        uint8_t type = 0;
        CHECK(feed_all(&q, bad, n, &type) == 0);
    }
}

static void slot_actions()
{
    const uint8_t a[6] = { 1, 2, 3, 4, 5, 6 };
    const uint8_t b[6] = { 1, 2, 3, 4, 5, 7 };
    CHECK(plink_slot_action(false, a, false, a) == PLINK_SLOT_KEEP);
    CHECK(plink_slot_action(false, a, true, a) == PLINK_SLOT_ADD);
    CHECK(plink_slot_action(true, a, true, a) == PLINK_SLOT_KEEP);
    CHECK(plink_slot_action(true, a, true, b) == PLINK_SLOT_REPLACE);
    CHECK(plink_slot_action(true, a, false, a) == PLINK_SLOT_REMOVE);
    char name[PLINK_MAC_STR_SIZE];
    const uint8_t mac[6] = { 0xC0, 0x11, 0x22, 0x3A, 0xB4, 0x0F };
    plink_mac_string(mac, name);
    CHECK_STR(name, "C0:11:22:3A:B4:0F");
}

// After a restart of the scanner a slot is unknown (the Matter node keeps its state) until its beacon is reported or
// a whole absence timeout of scanning passed without it
static void known_mask_after_restart()
{
    uint32_t timeouts[PLINK_SLOT_COUNT];
    for (int i = 0; i < PLINK_SLOT_COUNT; i++) {
        timeouts[i] = 45000;
    }
    timeouts[1] = 60000;
    timeouts[3] = 10000;
    timeouts[4] = 3600000;
    timeouts[19] = 120000;
    const int n = PLINK_SLOT_COUNT;
    const uint32_t all = 0xFFFFFFFF;
    // Slots 0 and 1 assigned, nothing heard yet, scanning just started: only the unassigned slots are known (off)
    CHECK(plink_known_mask(0x03, 0x00, timeouts, n, 0, 1000) == (all & ~0x03u));
    CHECK(plink_known_mask(0x03, 0x00, timeouts, n, 44999, 46000) == (all & ~0x03u));
    // Slot 0's 45 s of scanning without its beacon: known absent; slot 1 waits for its own 60 s
    CHECK(plink_known_mask(0x03, 0x00, timeouts, n, 45000, 46000) == (all & ~0x02u));
    CHECK(plink_known_mask(0x03, 0x00, timeouts, n, 60000, 61000) == all);
    // Slot 1 reported (its beacon was heard): known at once
    CHECK(plink_known_mask(0x03, 0x02, timeouts, n, 0, 1000) == (all & ~0x01u));
    // Every slot of the 20 assigned, slot 4 with a one hour timeout, slot 19 two minutes, slot 3 heard
    CHECK(plink_known_mask(PLINK_SLOT_MASK, 0x08, timeouts, n, 50000, 51000) == (all & ~0x80012u));
    CHECK(plink_known_mask(PLINK_SLOT_MASK, 0x08, timeouts, n, 120000, 121000) == (all & ~0x00010u));
    CHECK(plink_known_mask(PLINK_SLOT_MASK, 0x08, timeouts, n, 3600000, 3601000) == all);
    // No scanning at all (BLE failure, the scan clock stands still): known, as absent, 5 min after the timeout
    CHECK(plink_known_mask(0x01, 0x00, timeouts, n, 0, 45000 + PLINK_UNKNOWN_GRACE_MS - 1) == (all & ~0x01u));
    CHECK(plink_known_mask(0x01, 0x00, timeouts, n, 0, 45000 + PLINK_UNKNOWN_GRACE_MS) == all);
}

static void every_corruption_rejected()
{
    const plink_state_t in = { PLINK_MODE_NORMAL, 1, 0xF001F, 0x0A, 0x1F, 99 };
    uint8_t frame[PLINK_MAX_FRAME];
    const size_t n = plink_encode_state(&in, frame, sizeof(frame));
    for (size_t i = 0; i < n; i++) {
        for (int bit = 0; bit < 8; bit++) {
            uint8_t bad[PLINK_MAX_FRAME];
            memcpy(bad, frame, n);
            bad[i] ^= static_cast<uint8_t>(1u << bit);
            plink_parser_t p;
            plink_parser_init(&p);
            uint8_t type = 0;
            CHECK(feed_all(&p, bad, n, &type) == 0);
            // A damaged length byte (up to PLINK_MAX_PAYLOAD) makes the parser swallow up to 68 more bytes, here the
            // next two frames and the start of the third: the fourth is received
            int got = 0;
            for (int k = 0; k < 5; k++) {
                got += feed_all(&p, frame, n, &type);
            }
            CHECK(got >= 1);
        }
    }
}

static void truncation_then_resync()
{
    const plink_state_t in = { PLINK_MODE_CONFIG, 0, 0x01, 0, 0, 7 };
    uint8_t frame[PLINK_MAX_FRAME];
    const size_t n = plink_encode_state(&in, frame, sizeof(frame));
    for (size_t cut = 1; cut < n; cut++) {
        plink_parser_t p;
        plink_parser_init(&p);
        uint8_t type = 0;
        CHECK(feed_all(&p, frame, cut, &type) == 0);
        // a cut frame followed by full frames: at most the first full one is swallowed by the cut one
        int got = feed_all(&p, frame, n, &type);
        got += feed_all(&p, frame, n, &type);
        got += feed_all(&p, frame, n, &type);
        CHECK(got >= 2);
    }
}

static void garbage_like_rom_messages()
{
    const char *rom = "ESP-ROM:esp32c3-api1-20210207\r\nBuild:Feb  7 2021\r\nrst:0x1 (POWERON),boot:0xc (SPI_FAST_FLASH_BOOT)\r\n"
                      "\xA5\xA5\x5A\x01\x01\xFF garbage \xA5\x5A\x07";
    const plink_status_t in = { PLINK_STATUS_NETWORK_UP, 1, 5 };
    uint8_t frame[PLINK_MAX_FRAME];
    const size_t n = plink_encode_status(&in, frame, sizeof(frame));
    plink_parser_t p;
    plink_parser_init(&p);
    uint8_t type = 0;
    CHECK(feed_all(&p, reinterpret_cast<const uint8_t *>(rom), strlen(rom), &type) == 0);
    int got = 0;
    for (int i = 0; i < 3; i++) {
        got += feed_all(&p, frame, n, &type);
    }
    CHECK(got >= 2);

    // random noise never yields a frame with a bad CRC accepted as good
    srand(12345);
    plink_parser_init(&p);
    int accepted = 0;
    for (int i = 0; i < 200000; i++) {
        if (plink_parser_feed(&p, static_cast<uint8_t>(rand()))) {
            accepted++;
        }
    }
    CHECK(accepted == 0);
}

static void encode_limits()
{
    uint8_t payload[PLINK_MAX_PAYLOAD + 1] = {};
    uint8_t out[PLINK_MAX_FRAME + 8];
    CHECK(plink_encode(PLINK_TYPE_STATE, payload, PLINK_MAX_PAYLOAD + 1, out, sizeof(out)) == 0);
    CHECK(plink_encode(PLINK_TYPE_STATE, payload, PLINK_MAX_PAYLOAD, out, sizeof(out)) == PLINK_MAX_FRAME);
    CHECK(plink_encode(PLINK_TYPE_STATE, payload, 4, out, 8) == 0);
}

int main()
{
    RUN(state_round_trip);
    RUN(status_round_trip);
    RUN(present_only_for_active_slots);
    RUN(other_version_rejected);
    RUN(status_with_bridge);
    RUN(slot_round_trip);
    RUN(slot_decode_checks);
    RUN(pairing_round_trip);
    RUN(command_round_trip);
    RUN(slot_actions);
    RUN(known_mask_after_restart);
    RUN(every_corruption_rejected);
    RUN(truncation_then_resync);
    RUN(garbage_like_rom_messages);
    RUN(encode_limits);
    return test_summary("presence_link");
}
