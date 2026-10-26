/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "presence_logic.h"

static_assert(PRESENCE_LOGIC_SLOTS <= 32, "presence_logic_tick() returns a 32-bit mask");

#include <string.h>

// HCI reports 127 when the controller has no RSSI for the packet
static constexpr int8_t kRssiUnavailable = 127;

void presence_logic_init(presence_logic_t *pl, const presence_rule_t rules[PRESENCE_LOGIC_SLOTS])
{
    memset(pl, 0, sizeof(*pl));
    memcpy(pl->rules, rules, sizeof(pl->rules));
}

static bool rssi_passes(const presence_rule_t &rule, bool present, int8_t rssi)
{
    if (!rule.rssi_filter) {
        return true;
    }
    if (rssi == kRssiUnavailable) {
        return false;
    }
    const int limit = present ? rule.rssi_threshold - rule.rssi_hysteresis : rule.rssi_threshold;
    return rssi >= limit;
}

bool presence_logic_on_packet(presence_logic_t *pl, int slot, int8_t rssi, uint64_t now_ms)
{
    if (slot < 0 || slot >= PRESENCE_LOGIC_SLOTS) {
        return false;
    }
    const presence_rule_t &rule = pl->rules[slot];
    presence_slot_state_t &st = pl->state[slot];
    if (!rule.active) {
        return false;
    }
    if (!rssi_passes(rule, st.present, rssi)) {
        st.rssi_rejected++;
        return false;
    }
    st.accepted++;
    st.seen = true;
    st.last_seen_ms = now_ms;
    st.last_rssi = rssi;
    if (st.present) {
        return false;
    }
    st.present = true;
    return true;
}

uint32_t presence_logic_tick(presence_logic_t *pl, uint64_t now_ms)
{
    uint32_t turned_off = 0;
    for (int i = 0; i < PRESENCE_LOGIC_SLOTS; i++) {
        presence_slot_state_t &st = pl->state[i];
        if (!st.present) {
            continue;
        }
        // An inactive slot never turns on; the check keeps the rule simple if rules ever change at run time.
        const bool expired = now_ms >= st.last_seen_ms && now_ms - st.last_seen_ms >= pl->rules[i].timeout_ms;
        if (!pl->rules[i].active || expired) {
            st.present = false;
            turned_off |= 1u << i;
        }
    }
    return turned_off;
}

uint64_t presence_logic_age_ms(const presence_logic_t *pl, int slot, uint64_t now_ms)
{
    if (slot < 0 || slot >= PRESENCE_LOGIC_SLOTS || !pl->state[slot].seen || now_ms < pl->state[slot].last_seen_ms) {
        return 0;
    }
    return now_ms - pl->state[slot].last_seen_ms;
}
