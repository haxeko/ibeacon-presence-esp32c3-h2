// Presence decision (main/presence_logic.cpp): ON at the first packet, OFF after the timeout, repeated
// identical advertisements keep it ON, scanner pauses never turn a slot OFF, RSSI filter with hysteresis.
#include "test.h"

#include "presence_logic.h"

static presence_rule_t rule(uint32_t timeout_s, bool active = true)
{
    presence_rule_t r = {};
    r.active = active;
    r.timeout_ms = timeout_s * 1000;
    return r;
}

static void init_all(presence_logic_t *pl, presence_rule_t r)
{
    presence_rule_t rules[PRESENCE_LOGIC_SLOTS];
    for (auto &x : rules) {
        x = r;
    }
    presence_logic_init(pl, rules);
}

static void boot_state_is_off()
{
    presence_logic_t pl;
    init_all(&pl, rule(45));
    for (int i = 0; i < PRESENCE_LOGIC_SLOTS; i++) {
        CHECK(!pl.state[i].present);
        CHECK(!pl.state[i].seen);
    }
    CHECK(presence_logic_tick(&pl, 1000000) == 0);
}

static void on_then_off_after_timeout()
{
    presence_logic_t pl;
    init_all(&pl, rule(45));
    CHECK(presence_logic_on_packet(&pl, 2, -60, 10000)); // ON at once
    CHECK(pl.state[2].present);
    CHECK(!presence_logic_on_packet(&pl, 2, -61, 11000)); // already ON: no new transition
    CHECK(presence_logic_tick(&pl, 11000 + 44999) == 0);    // 1 ms before the timeout
    CHECK(pl.state[2].present);
    CHECK(presence_logic_tick(&pl, 11000 + 45000) == (1u << 2)); // exactly at the timeout
    CHECK(!pl.state[2].present);
    CHECK(presence_logic_tick(&pl, 200000) == 0); // reported once
    // Back again
    CHECK(presence_logic_on_packet(&pl, 2, -60, 300000));
}

static void repeated_identical_advertisements_keep_on()
{
    // A beacon repeating the same payload every second for an hour: every report refreshes last_seen (no
    // duplicate filtering in the decision), the slot never turns off.
    presence_logic_t pl;
    init_all(&pl, rule(45));
    uint64_t t = 0;
    int transitions = 0;
    for (int i = 0; i < 3600; i++) {
        t += 1000;
        transitions += presence_logic_on_packet(&pl, 0, -70, t) ? 1 : 0;
        CHECK(presence_logic_tick(&pl, t) == 0);
    }
    CHECK(transitions == 1);
    CHECK(pl.state[0].accepted == 3600);
    CHECK(pl.state[0].present);
}

static void scanner_pause_does_not_turn_off()
{
    // presence_logic only sees the scan clock: while the scanner is paused (commissioning, Thread re-attach) the
    // clock does not move, so even a long pause does not count toward the timeout.
    presence_logic_t pl;
    init_all(&pl, rule(45));
    uint64_t scan_clock = 0;
    presence_logic_on_packet(&pl, 1, -60, scan_clock);
    scan_clock += 30000; // 30 s of scanning, no packet
    CHECK(presence_logic_tick(&pl, scan_clock) == 0);
    // 10 minutes of pause: wall time moves, the scan clock does not -> nothing happens
    for (int i = 0; i < 600; i++) {
        CHECK(presence_logic_tick(&pl, scan_clock) == 0);
    }
    CHECK(pl.state[1].present);
    // Scanning resumes: off only after the remaining 15 s
    scan_clock += 14999;
    CHECK(presence_logic_tick(&pl, scan_clock) == 0);
    scan_clock += 1;
    CHECK(presence_logic_tick(&pl, scan_clock) == (1u << 1));
}

static void inactive_slot_never_on()
{
    presence_logic_t pl;
    presence_rule_t rules[PRESENCE_LOGIC_SLOTS] = { rule(45), rule(45, false), rule(45), rule(45), rule(45) };
    presence_logic_init(&pl, rules);
    CHECK(!presence_logic_on_packet(&pl, 1, -40, 1000));
    CHECK(!pl.state[1].present);
    CHECK(!presence_logic_on_packet(&pl, -1, -40, 1000)); // out of range slots are ignored
    CHECK(!presence_logic_on_packet(&pl, PRESENCE_LOGIC_SLOTS, -40, 1000));
}

static void slots_are_independent()
{
    presence_logic_t pl;
    presence_rule_t rules[PRESENCE_LOGIC_SLOTS] = { rule(10), rule(45), rule(60), rule(45), rule(45) };
    presence_logic_init(&pl, rules);
    presence_logic_on_packet(&pl, 0, -60, 0);
    presence_logic_on_packet(&pl, 1, -60, 0);
    presence_logic_on_packet(&pl, 2, -60, 0);
    CHECK(presence_logic_tick(&pl, 10000) == (1u << 0));
    CHECK(presence_logic_tick(&pl, 45000) == (1u << 1));
    CHECK(presence_logic_tick(&pl, 59999) == 0);
    CHECK(presence_logic_tick(&pl, 60000) == (1u << 2));
}

static void rssi_filter_with_hysteresis()
{
    presence_rule_t r = rule(30);
    r.rssi_filter = true;
    r.rssi_threshold = -75;
    r.rssi_hysteresis = 6;
    presence_logic_t pl;
    init_all(&pl, r);

    // Off: needs >= -75
    CHECK(!presence_logic_on_packet(&pl, 0, -76, 1000));
    CHECK(pl.state[0].rssi_rejected == 1);
    CHECK(!presence_logic_on_packet(&pl, 0, 127, 1500)); // RSSI unavailable never passes the filter
    CHECK(presence_logic_on_packet(&pl, 0, -75, 2000));
    // On: packets down to -81 keep it on
    presence_logic_on_packet(&pl, 0, -81, 20000);
    CHECK(pl.state[0].last_seen_ms == 20000);
    CHECK(presence_logic_tick(&pl, 49999) == 0);
    // Weaker packets do not refresh: off 30 s after the last strong enough one, although packets keep coming
    for (uint64_t t = 21000; t < 50000; t += 1000) {
        presence_logic_on_packet(&pl, 0, -82, t);
    }
    CHECK(presence_logic_tick(&pl, 50000) == 1u);
    // Off again: -80 is not enough to come back, -75 is
    CHECK(!presence_logic_on_packet(&pl, 0, -80, 51000));
    CHECK(presence_logic_on_packet(&pl, 0, -70, 52000));
}

static void filter_off_accepts_any_rssi()
{
    presence_logic_t pl;
    init_all(&pl, rule(45));
    CHECK(presence_logic_on_packet(&pl, 4, -105, 0));
    CHECK(pl.state[4].rssi_rejected == 0);
}

static void age()
{
    presence_logic_t pl;
    init_all(&pl, rule(45));
    CHECK(presence_logic_age_ms(&pl, 0, 5000) == 0); // never seen
    presence_logic_on_packet(&pl, 0, -60, 5000);
    CHECK(presence_logic_age_ms(&pl, 0, 7500) == 2500);
}

int main()
{
    RUN(boot_state_is_off);
    RUN(on_then_off_after_timeout);
    RUN(repeated_identical_advertisements_keep_on);
    RUN(scanner_pause_does_not_turn_off);
    RUN(inactive_slot_never_on);
    RUN(slots_are_independent);
    RUN(rssi_filter_with_hysteresis);
    RUN(filter_off_accepts_any_rssi);
    RUN(age);
    return test_summary(PRESENCE_LOGIC_SLOTS == 5 ? "presence_logic" : "presence_logic 20");
}
