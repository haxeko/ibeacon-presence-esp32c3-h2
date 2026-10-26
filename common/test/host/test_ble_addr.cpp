// Bluetooth addresses (main/ble_addr.cpp): parsing and normalisation of typed addresses, byte order between
// NimBLE reports and the display form, address kinds and type matching.
#include "test.h"

#include "ble_addr.h"

static void parse_formats()
{
    const uint8_t expected[6] = { 0xAA, 0xBB, 0xCC, 0x01, 0x02, 0x0F };
    const char *ok[] = { "AA:BB:CC:01:02:0F", "aa:bb:cc:01:02:0f", "AA-BB-CC-01-02-0F", "aabbcc01020f",
                         "  AA:BB:CC:01:02:0F\t", "Aa:bB:Cc:01:02:0F\n" };
    for (const char *s : ok) {
        uint8_t addr[6] = {};
        CHECK(ble_addr_parse(s, addr));
        CHECK(memcmp(addr, expected, 6) == 0);
    }
}

static void parse_rejects()
{
    const char *bad[] = { "",
                          "AA:BB:CC:01:02",       // too short
                          "AA:BB:CC:01:02:0F:11", // too long
                          "AA:BB:CC:01:02:0G",    // not hex
                          "AA:BB-CC:01:02:0F",    // mixed separators
                          "AA.BB.CC.01.02.0F",    // other separator
                          "AABB.CC01.020F",       // Cisco style
                          "AA:BB:CC:01:02:0 F", "A:BB:CC:01:02:0FF", "aabbcc01020",
                          "aabbcc01020fx" };
    for (const char *s : bad) {
        uint8_t addr[6] = { 1, 2, 3, 4, 5, 6 };
        CHECK(!ble_addr_parse(s, addr));
        CHECK(addr[0] == 1 && addr[5] == 6); // untouched
    }
    uint8_t addr[6];
    CHECK(!ble_addr_parse(nullptr, addr));
}

static void format_and_byte_order()
{
    // NimBLE's ble_addr_t.val holds the least significant octet first: on air AA:BB:CC:DD:EE:FF is
    // val = {FF, EE, DD, CC, BB, AA}.
    const uint8_t nimble_val[6] = { 0xFF, 0xEE, 0xDD, 0xCC, 0xBB, 0xAA };
    uint8_t addr[6];
    ble_addr_from_le(nimble_val, addr);
    char text[BLE_ADDR_STR_SIZE];
    ble_addr_format(addr, text);
    CHECK_STR(text, "AA:BB:CC:DD:EE:FF");

    uint8_t back[6];
    ble_addr_to_le(addr, back);
    CHECK(memcmp(back, nimble_val, 6) == 0);

    // Round trip typed text -> stored -> text
    uint8_t parsed[6];
    CHECK(ble_addr_parse("c0:ff:ee:12:34:56", parsed));
    ble_addr_format(parsed, text);
    CHECK_STR(text, "C0:FF:EE:12:34:56");
}

static void usable()
{
    const uint8_t zero[6] = {};
    const uint8_t ff[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    const uint8_t ok[6] = { 0, 0, 0, 0, 0, 1 };
    CHECK(!ble_addr_is_usable(zero));
    CHECK(!ble_addr_is_usable(ff));
    CHECK(ble_addr_is_usable(ok));
}

static void kinds()
{
    const uint8_t static_addr[6] = { 0xC3, 1, 2, 3, 4, 5 };
    const uint8_t rpa[6] = { 0x4A, 1, 2, 3, 4, 5 };
    const uint8_t nrpa[6] = { 0x12, 1, 2, 3, 4, 5 };
    const uint8_t reserved[6] = { 0x8A, 1, 2, 3, 4, 5 };
    CHECK(ble_addr_kind(BLE_OTA_ADDR_PUBLIC, rpa) == BLE_ADDR_KIND_PUBLIC); // top bits only matter for random
    CHECK(ble_addr_kind(BLE_OTA_ADDR_RANDOM, static_addr) == BLE_ADDR_KIND_RANDOM_STATIC);
    CHECK(ble_addr_kind(BLE_OTA_ADDR_RANDOM, rpa) == BLE_ADDR_KIND_RANDOM_RESOLVABLE);
    CHECK(ble_addr_kind(BLE_OTA_ADDR_RANDOM, nrpa) == BLE_ADDR_KIND_RANDOM_NON_RESOLVABLE);
    CHECK(ble_addr_kind(BLE_OTA_ADDR_RANDOM, reserved) == BLE_ADDR_KIND_RANDOM_RESERVED);
    CHECK(ble_addr_kind_is_stable(BLE_ADDR_KIND_PUBLIC));
    CHECK(ble_addr_kind_is_stable(BLE_ADDR_KIND_RANDOM_STATIC));
    CHECK(!ble_addr_kind_is_stable(BLE_ADDR_KIND_RANDOM_RESOLVABLE));
    CHECK(!ble_addr_kind_is_stable(BLE_ADDR_KIND_RANDOM_NON_RESOLVABLE));
}

static void type_matching()
{
    CHECK(beacon_addr_type_matches(BEACON_ADDR_ANY, BLE_OTA_ADDR_PUBLIC));
    CHECK(beacon_addr_type_matches(BEACON_ADDR_ANY, BLE_OTA_ADDR_RANDOM));
    CHECK(beacon_addr_type_matches(BEACON_ADDR_PUBLIC, BLE_OTA_ADDR_PUBLIC));
    CHECK(!beacon_addr_type_matches(BEACON_ADDR_PUBLIC, BLE_OTA_ADDR_RANDOM));
    CHECK(beacon_addr_type_matches(BEACON_ADDR_RANDOM, BLE_OTA_ADDR_RANDOM));
    CHECK(beacon_addr_type_matches(BEACON_ADDR_RANDOM, BLE_OTA_ADDR_RANDOM_ID));
    CHECK(!beacon_addr_type_matches(BEACON_ADDR_RANDOM, BLE_OTA_ADDR_PUBLIC));
    CHECK(!beacon_addr_type_matches(7, BLE_OTA_ADDR_PUBLIC));

    uint8_t t = 0xEE;
    CHECK(beacon_addr_type_parse("random", &t) && t == BEACON_ADDR_RANDOM);
    CHECK(beacon_addr_type_parse("any", &t) && t == BEACON_ADDR_ANY);
    CHECK(!beacon_addr_type_parse("Public", &t)); // exact names only
    CHECK_STR(beacon_addr_type_name(BEACON_ADDR_PUBLIC), "public");
}

int main()
{
    RUN(parse_formats);
    RUN(parse_rejects);
    RUN(format_and_byte_order);
    RUN(usable);
    RUN(kinds);
    RUN(type_matching);
    return test_summary("ble_addr");
}
