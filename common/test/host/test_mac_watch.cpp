// MAC watch of a slot's beacon (main/mac_watch.cpp): fixed after 30 minutes of hearing it, changed when its identity
// comes from another random address while its own is silent, public address fixed at once, two beacons with one
// identity not taken for a change, restarts, stored form.
#include <stdlib.h>

#include "mac_watch.h"
#include "test.h"

static const uint8_t kMac[6] = { 0x48, 0x87, 0x2D, 0x9D, 0xC1, 0xDF };
static const uint8_t kOther[6] = { 0x5A, 0x01, 0x02, 0x03, 0x04, 0x05 };

static ibeacon_t id(uint16_t minor = 6)
{
    ibeacon_t b = {};
    const uint8_t uuid[16] = { 0xE2, 0xC5, 0x6D, 0xB5, 0xDF, 0xFB, 0x48, 0xD2, 0xB0, 0x60, 0xD0, 0xF5, 0xA7, 0x10, 0x96, 0xE0 };
    memcpy(b.uuid, uuid, sizeof(uuid));
    b.major = 5;
    b.minor = minor;
    b.tx_power = -57;
    return b;
}

static mac_watch_t assigned()
{
    mac_watch_t w = {};
    CHECK(mac_watch_assign(&w, kMac));
    CHECK(w.state == MAC_WATCH_RUNNING && w.watched_ms == 0 && !w.has_id);
    return w;
}

static void fixed_after_30_minutes_heard()
{
    mac_watch_t w = assigned();
    const ibeacon_t b = id();
    int64_t t = 1000;
    CHECK(mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, t) == MAC_WATCH_EV_PROGRESS); // identity read
    CHECK(w.has_id && w.major == 5 && w.minor == 6);
    CHECK(mac_watch_needs_all(&w));
    // A frame every second for 29 minutes 59 seconds: still running
    for (int i = 0; i < 30 * 60 - 1; i++) {
        t += 1000;
        CHECK(mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, t) == MAC_WATCH_EV_PROGRESS);
    }
    CHECK(w.state == MAC_WATCH_RUNNING && w.watched_ms == 30u * 60u * 1000u - 1000u);
    t += 1000;
    CHECK(mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, t) == MAC_WATCH_EV_STATE);
    CHECK(w.state == MAC_WATCH_FIXED && !mac_watch_needs_all(&w));
    // Fixed for good: no event any more, another address with its identity changes nothing
    t += 120000;
    CHECK(mac_watch_other(&w, kOther, BLE_OTA_ADDR_RANDOM, &b, t, 0) == MAC_WATCH_EV_NONE && w.state == MAC_WATCH_FIXED);
    CHECK(mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, t) == MAC_WATCH_EV_NONE);
    // Same MAC assigned again (saved again): kept; another MAC: a new watch
    CHECK(!mac_watch_assign(&w, kMac) && w.state == MAC_WATCH_FIXED);
    CHECK(mac_watch_assign(&w, kOther) && w.state == MAC_WATCH_RUNNING && memcmp(w.mac, kOther, 6) == 0);
}

static void gaps_not_counted()
{
    mac_watch_t w = assigned();
    const ibeacon_t b = id();
    mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, 1000);
    mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, 61000); // 60 s: counted
    CHECK(w.watched_ms == 60000);
    mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, 61000 + 60001); // away 60.001 s: not counted
    CHECK(w.watched_ms == 60000);
    mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, 61000 + 60001 + 5000);
    CHECK(w.watched_ms == 65000);
    // Time going backwards (another boot's clock): nothing counted
    mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, 10);
    CHECK(w.watched_ms == 65000 && w.state == MAC_WATCH_RUNNING);
}

static void public_address_fixed_at_once()
{
    mac_watch_t w = assigned();
    const ibeacon_t b = id();
    CHECK(mac_watch_own(&w, BLE_OTA_ADDR_PUBLIC, &b, 1000) == MAC_WATCH_EV_STATE);
    CHECK(w.state == MAC_WATCH_FIXED);
}

static void changed_address()
{
    mac_watch_t w = assigned();
    const ibeacon_t b = id();
    mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, 1000);
    mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, 300000);
    // The identity from a new random address 1 s after the last own frame: a candidate, not confirmed yet
    CHECK(mac_watch_other(&w, kOther, BLE_OTA_ADDR_RANDOM, &b, 301000, 0) == MAC_WATCH_EV_NONE);
    // Other identity, public address, own address: never a change
    const ibeacon_t c = id(7);
    CHECK(mac_watch_other(&w, kOther, BLE_OTA_ADDR_RANDOM, &c, 500000, 0) == MAC_WATCH_EV_NONE);
    CHECK(mac_watch_other(&w, kOther, BLE_OTA_ADDR_PUBLIC, &b, 500000, 0) == MAC_WATCH_EV_NONE);
    CHECK(mac_watch_other(&w, kMac, BLE_OTA_ADDR_RANDOM, &b, 500000, 0) == MAC_WATCH_EV_NONE);
    CHECK(w.state == MAC_WATCH_RUNNING);
    // Heard 59.999 s: not yet; 60 s and the own address silent more than 60 s: changed
    CHECK(mac_watch_other(&w, kOther, BLE_OTA_ADDR_RANDOM, &b, 360999, 0) == MAC_WATCH_EV_NONE);
    CHECK(mac_watch_other(&w, kOther, BLE_OTA_ADDR_RANDOM, &b, 361001, 0) == MAC_WATCH_EV_STATE);
    CHECK(w.state == MAC_WATCH_CHANGED && memcmp(w.new_mac, kOther, 6) == 0 && !mac_watch_needs_all(&w));
    CHECK(mac_watch_other(&w, kOther, BLE_OTA_ADDR_RANDOM, &b, 400000, 0) == MAC_WATCH_EV_NONE);
    // The own address heard again: it was another beacon with the same identity, watched on
    CHECK(mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, 900000) == MAC_WATCH_EV_STATE);
    CHECK(w.state == MAC_WATCH_RUNNING && w.new_mac[0] == 0);
    // That address can never make it change again (no flip-flop, no flash writes in a loop)
    CHECK(mac_watch_other(&w, kOther, BLE_OTA_ADDR_RANDOM, &b, 2000000, 0) == MAC_WATCH_EV_NONE);
    CHECK(w.state == MAC_WATCH_RUNNING);
}

// Another beacon with the same identity, heard together with the slot's beacon, then the slot's beacon leaves (the
// person goes out): no change
static void lookalike_heard_together()
{
    mac_watch_t w = assigned();
    const ibeacon_t b = id();
    mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, 1000);
    CHECK(mac_watch_other(&w, kOther, BLE_OTA_ADDR_RANDOM, &b, 2000, 0) == MAC_WATCH_EV_NONE);
    mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, 3000); // own frame after it: another beacon
    for (int64_t t = 4000; t < 3600000; t += 1000) {
        CHECK(mac_watch_other(&w, kOther, BLE_OTA_ADDR_RANDOM, &b, t, 0) == MAC_WATCH_EV_NONE);
    }
    CHECK(w.state == MAC_WATCH_RUNNING);
}

// Another beacon with the same identity coming into range long after the slot's beacon left: no change
static void lookalike_arriving_late()
{
    mac_watch_t w = assigned();
    const ibeacon_t b = id();
    mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, 1000);
    CHECK(mac_watch_other(&w, kOther, BLE_OTA_ADDR_RANDOM, &b, 1000 + 60001, 0) == MAC_WATCH_EV_NONE);
    CHECK(mac_watch_other(&w, kOther, BLE_OTA_ADDR_RANDOM, &b, 500000, 0) == MAC_WATCH_EV_NONE);
    CHECK(w.state == MAC_WATCH_RUNNING);
}

static void candidate_table()
{
    mac_watch_t w = assigned();
    const ibeacon_t b = id();
    mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, 1000);
    uint8_t a[6] = { 0x5A, 0, 0, 0, 0, 0 };
    for (uint8_t i = 1; i <= MAC_WATCH_CANDIDATES; i++) {
        a[5] = i;
        mac_watch_other(&w, a, BLE_OTA_ADDR_RANDOM, &b, 2000 + i, 0);
    }
    // Table full of live addresses: a further one is ignored
    a[5] = 9;
    mac_watch_other(&w, a, BLE_OTA_ADDR_RANDOM, &b, 2500, 0);
    CHECK(mac_watch_other(&w, a, BLE_OTA_ADDR_RANDOM, &b, 200000, 0) == MAC_WATCH_EV_NONE);
    // All of them look-alikes after an own frame: their marks are kept, a new address finds no place (more look-alikes
    // around than entries: nothing can be told apart, no warning)
    mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, 3000);
    a[5] = 10;
    CHECK(mac_watch_other(&w, a, BLE_OTA_ADDR_RANDOM, &b, 3500, 0) == MAC_WATCH_EV_NONE);
    CHECK(mac_watch_other(&w, a, BLE_OTA_ADDR_RANDOM, &b, 64000, 0) == MAC_WATCH_EV_NONE);
    for (uint8_t i = 1; i <= MAC_WATCH_CANDIDATES; i++) {
        a[5] = i; // the look-alikes, still heard long after the slot's beacon left
        CHECK(mac_watch_other(&w, a, BLE_OTA_ADDR_RANDOM, &b, 500000 + i, 0) == MAC_WATCH_EV_NONE);
    }
    CHECK(w.state == MAC_WATCH_RUNNING);
}

// A beacon changing its address every 30 s, faster than the 60 s confirmation: the chain of addresses is followed
static void fast_changing_chain()
{
    mac_watch_t w = assigned();
    const ibeacon_t b = id();
    mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, 1000);
    mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, 10000); // last own frame
    uint8_t a[6] = { 0x5A, 0, 0, 0, 0, 1 };
    mac_watch_event_t ev = MAC_WATCH_EV_NONE;
    int64_t t = 11000;
    for (int k = 0; k < 6 && ev == MAC_WATCH_EV_NONE; k++) {
        a[5] = static_cast<uint8_t>(k + 1);
        for (int64_t end = t + 30000; t < end && ev == MAC_WATCH_EV_NONE; t += 1000) {
            ev = mac_watch_other(&w, a, BLE_OTA_ADDR_RANDOM, &b, t, 0);
        }
    }
    CHECK(ev == MAC_WATCH_EV_STATE && w.state == MAC_WATCH_CHANGED);
    CHECK(t - 1000 >= 11000 + MAC_WATCH_CONFIRM_MS && t - 1000 <= 11000 + MAC_WATCH_CONFIRM_MS + 1000);
    CHECK(w.new_mac[5] == 3); // the third address of the chain
}

// A beacon sending two identities in turn from its address, with a look-alike of one of them: the mark is kept
static void two_identities_keep_marks()
{
    mac_watch_t w = assigned();
    const ibeacon_t b = id();
    const ibeacon_t c = id(7);
    mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, 1000);
    mac_watch_other(&w, kOther, BLE_OTA_ADDR_RANDOM, &b, 1500, 0);
    int64_t t = 2000;
    for (int k = 0; k < 20; k++, t += 1000) {
        mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, k % 2 ? &b : &c, t); // identities in turn
    }
    mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, t); // leaves on identity b
    for (int64_t u = t + 1000; u < t + 600000; u += 1000) {
        CHECK(mac_watch_other(&w, kOther, BLE_OTA_ADDR_RANDOM, &b, u, 0) == MAC_WATCH_EV_NONE);
    }
    CHECK(w.state == MAC_WATCH_RUNNING);
}

static void restart_does_not_count_as_silence()
{
    mac_watch_t w = assigned();
    const ibeacon_t b = id();
    mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, 1000);
    w.last_own_ms = 0; // restart: this boot has not heard it yet
    memset(w.cand, 0, sizeof(w.cand));
    const int64_t since = 5000;
    // A new address right after the start, nothing from the own address: changed once confirmed
    CHECK(mac_watch_other(&w, kOther, BLE_OTA_ADDR_RANDOM, &b, since + 2000, since) == MAC_WATCH_EV_NONE);
    CHECK(mac_watch_other(&w, kOther, BLE_OTA_ADDR_RANDOM, &b, since + 30000, since) == MAC_WATCH_EV_NONE);
    CHECK(mac_watch_other(&w, kOther, BLE_OTA_ADDR_RANDOM, &b, since + 62000, since) == MAC_WATCH_EV_STATE);
    // Appearing more than 60 s after the start instead: another beacon
    mac_watch_t v = assigned();
    mac_watch_own(&v, BLE_OTA_ADDR_RANDOM, &b, 1000);
    v.last_own_ms = 0;
    CHECK(mac_watch_other(&v, kOther, BLE_OTA_ADDR_RANDOM, &b, since + 61000, since) == MAC_WATCH_EV_NONE);
    CHECK(mac_watch_other(&v, kOther, BLE_OTA_ADDR_RANDOM, &b, since + 500000, since) == MAC_WATCH_EV_NONE);
}

static void identity_needed_first()
{
    mac_watch_t w = assigned();
    const ibeacon_t b = id();
    // Never heard from its own address, nothing seeded: its identity is unknown, nothing can be compared
    CHECK(mac_watch_other(&w, kOther, BLE_OTA_ADDR_RANDOM, &b, 10000000, 0) == MAC_WATCH_EV_NONE);
    CHECK(!mac_watch_needs_all(&w) && w.state == MAC_WATCH_RUNNING);
    // Nothing assigned: no event
    mac_watch_t none = {};
    CHECK(mac_watch_own(&none, BLE_OTA_ADDR_RANDOM, &b, 1000) == MAC_WATCH_EV_NONE && none.state == MAC_WATCH_NONE);
    CHECK(!mac_watch_seed(&none, &b));
    // The beacon given another identity: taken over
    mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &b, 1000);
    mac_watch_other(&w, kOther, BLE_OTA_ADDR_RANDOM, &b, 1500, 0);
    const ibeacon_t c = id(9);
    CHECK(mac_watch_own(&w, BLE_OTA_ADDR_RANDOM, &c, 2000) == MAC_WATCH_EV_PROGRESS && w.minor == 9);
}

// Identity from the scan list when the configuration is saved: a beacon that changes its MAC before the NORMAL mode
// ever hears it is still caught
static void seeded_identity()
{
    mac_watch_t w = assigned();
    const ibeacon_t b = id();
    CHECK(mac_watch_seed(&w, &b) && w.has_id && mac_watch_needs_all(&w));
    CHECK(!mac_watch_seed(&w, &b)); // only once
    CHECK(mac_watch_other(&w, kOther, BLE_OTA_ADDR_RANDOM, &b, 1000, 0) == MAC_WATCH_EV_NONE);
    CHECK(mac_watch_other(&w, kOther, BLE_OTA_ADDR_RANDOM, &b, 61001, 0) == MAC_WATCH_EV_STATE);
    CHECK(w.state == MAC_WATCH_CHANGED);
}

static void stored_form()
{
    mac_watch_t w[20] = {};
    const ibeacon_t b = id();
    mac_watch_assign(&w[0], kMac);
    mac_watch_own(&w[0], BLE_OTA_ADDR_RANDOM, &b, 1000);
    mac_watch_own(&w[0], BLE_OTA_ADDR_RANDOM, &b, 31000);
    mac_watch_assign(&w[3], kOther);
    w[3].state = MAC_WATCH_CHANGED;
    memcpy(w[3].new_mac, kMac, 6);
    uint8_t buf[MAC_WATCH_BLOB_SIZE(20)];
    const size_t len = mac_watch_encode(w, 20, buf, sizeof(buf));
    CHECK(len == sizeof(buf) && len == 8 + 20 * 38 + 4);
    CHECK(mac_watch_encode(w, 20, buf, sizeof(buf) - 1) == 0);
    mac_watch_t out[20];
    CHECK(mac_watch_decode(buf, len, out, 20));
    CHECK(memcmp(out[0].mac, kMac, 6) == 0 && out[0].state == MAC_WATCH_RUNNING && out[0].has_id &&
          out[0].watched_ms == 30000 && out[0].minor == 6 && out[0].last_own_ms == 0);
    CHECK(out[3].state == MAC_WATCH_CHANGED && memcmp(out[3].new_mac, kMac, 6) == 0);
    CHECK(out[1].state == MAC_WATCH_NONE);

    // Five-slot firmware reading 20 records: the first five; 20-slot reading 5: the others none
    mac_watch_t five[5];
    CHECK(mac_watch_decode(buf, len, five, 5) && five[3].state == MAC_WATCH_CHANGED);
    uint8_t small[MAC_WATCH_BLOB_SIZE(5)];
    const size_t slen = mac_watch_encode(five, 5, small, sizeof(small));
    CHECK(mac_watch_decode(small, slen, out, 20) && out[3].state == MAC_WATCH_CHANGED && out[10].state == MAC_WATCH_NONE);

    // Every bit flip is caught (all reset), a truncated blob too
    for (size_t i = 0; i < len; i++) {
        for (int bit = 0; bit < 8; bit++) {
            buf[i] ^= static_cast<uint8_t>(1u << bit);
            CHECK(!mac_watch_decode(buf, len, out, 20) && out[0].state == MAC_WATCH_NONE);
            buf[i] ^= static_cast<uint8_t>(1u << bit);
        }
    }
    for (size_t l = 0; l < len; l++) {
        CHECK(!mac_watch_decode(buf, l, out, 20));
    }
    CHECK(!mac_watch_decode(nullptr, len, out, 20));
    CHECK_STR(mac_watch_state_name(MAC_WATCH_CHANGED), "changed");
    CHECK_STR(mac_watch_state_name(42), "none");
}

int main()
{
    RUN(fixed_after_30_minutes_heard);
    RUN(gaps_not_counted);
    RUN(public_address_fixed_at_once);
    RUN(changed_address);
    RUN(lookalike_heard_together);
    RUN(lookalike_arriving_late);
    RUN(candidate_table);
    RUN(fast_changing_chain);
    RUN(two_identities_keep_marks);
    RUN(restart_does_not_count_as_silence);
    RUN(identity_needed_first);
    RUN(seeded_identity);
    RUN(stored_form);
    return test_summary("mac_watch");
}
