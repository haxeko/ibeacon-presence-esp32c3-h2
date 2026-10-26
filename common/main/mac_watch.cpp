/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "mac_watch.h"

#include <string.h>

#include "crc32.h"

static const uint8_t kMagic[4] = { 'M', 'W', 'A', 'T' };
static constexpr uint8_t kVersion = 1;
static constexpr size_t kHeader = 8; // magic, version, count, 2 reserved
static constexpr size_t kRecord = 38;
static constexpr uint8_t kFlagHasId = 0x01;

static bool same_id(const mac_watch_t *w, const ibeacon_t *id)
{
    return memcmp(w->uuid, id->uuid, IBEACON_UUID_LEN) == 0 && w->major == id->major && w->minor == id->minor;
}

static bool is_random(uint8_t addr_type)
{
    return addr_type == BLE_OTA_ADDR_RANDOM || addr_type == BLE_OTA_ADDR_RANDOM_ID;
}

bool mac_watch_assign(mac_watch_t *w, const uint8_t mac[BLE_ADDR_LEN])
{
    if (w->state != MAC_WATCH_NONE && memcmp(w->mac, mac, BLE_ADDR_LEN) == 0) {
        return false;
    }
    memset(w, 0, sizeof(*w));
    memcpy(w->mac, mac, BLE_ADDR_LEN);
    w->state = MAC_WATCH_RUNNING;
    return true;
}

static void set_id(mac_watch_t *w, const ibeacon_t *id)
{
    memcpy(w->uuid, id->uuid, IBEACON_UUID_LEN);
    w->major = id->major;
    w->minor = id->minor;
    w->has_id = true;
    // The candidates stay: a beacon sending two identities in turn keeps its look-alikes marked
}

bool mac_watch_seed(mac_watch_t *w, const ibeacon_t *id)
{
    if (w->state != MAC_WATCH_RUNNING || w->has_id) {
        return false;
    }
    set_id(w, id);
    return true;
}

mac_watch_event_t mac_watch_own(mac_watch_t *w, uint8_t addr_type, const ibeacon_t *id, int64_t now_ms)
{
    const int64_t last = w->last_own_ms;
    w->last_own_ms = now_ms;
    // Every other address heard so far with its identity was heard while this one is still in use: other beacons
    for (mac_watch_candidate_t &c : w->cand) {
        c.lookalike = c.lookalike || c.used;
    }
    if (w->state == MAC_WATCH_NONE || w->state == MAC_WATCH_FIXED) {
        return MAC_WATCH_EV_NONE;
    }
    mac_watch_event_t ev = MAC_WATCH_EV_NONE;
    if (w->state == MAC_WATCH_CHANGED) {
        // Its own address again: the other one was another beacon with the same identity
        w->state = MAC_WATCH_RUNNING;
        memset(w->new_mac, 0, sizeof(w->new_mac));
        ev = MAC_WATCH_EV_STATE;
    }
    if (!w->has_id || !same_id(w, id)) {
        set_id(w, id); // first frame, or the beacon was given another identity
        ev = ev == MAC_WATCH_EV_STATE ? ev : MAC_WATCH_EV_PROGRESS;
    }
    if (!is_random(addr_type)) {
        w->state = MAC_WATCH_FIXED; // a public address never changes
        return MAC_WATCH_EV_STATE;
    }
    if (last != 0 && now_ms > last && now_ms - last <= MAC_WATCH_GONE_MS) {
        const int64_t total = static_cast<int64_t>(w->watched_ms) + (now_ms - last);
        w->watched_ms = total > MAC_WATCH_TIME_MS ? MAC_WATCH_TIME_MS : static_cast<uint32_t>(total);
        ev = ev == MAC_WATCH_EV_STATE ? ev : MAC_WATCH_EV_PROGRESS;
    }
    if (w->watched_ms >= MAC_WATCH_TIME_MS) {
        w->state = MAC_WATCH_FIXED;
        return MAC_WATCH_EV_STATE;
    }
    return ev;
}

mac_watch_event_t mac_watch_other(mac_watch_t *w, const uint8_t addr[BLE_ADDR_LEN], uint8_t addr_type,
                                  const ibeacon_t *id, int64_t now_ms, int64_t since_ms)
{
    if (w->state != MAC_WATCH_RUNNING || !w->has_id || !is_random(addr_type) ||
        memcmp(addr, w->mac, BLE_ADDR_LEN) == 0 || !same_id(w, id)) {
        return MAC_WATCH_EV_NONE;
    }
    const int64_t own = w->last_own_ms != 0 ? w->last_own_ms : since_ms;
    mac_watch_candidate_t *c = nullptr;
    for (mac_watch_candidate_t &k : w->cand) {
        if (k.used && memcmp(k.addr, addr, BLE_ADDR_LEN) == 0) {
            c = &k;
            break;
        }
    }
    if (c == nullptr) {
        // A new address: a free entry, else that of an earlier address of a chain gone silent. A look-alike's entry is
        // never reused (its mark would be lost); a table full of them and of live addresses: ignored.
        for (mac_watch_candidate_t &k : w->cand) {
            if (!k.used) {
                c = &k;
                break;
            }
        }
        for (mac_watch_candidate_t &k : w->cand) {
            if (c == nullptr && !k.lookalike && now_ms - k.last_ms > MAC_WATCH_GONE_MS) {
                c = &k;
            }
        }
        if (c == nullptr) {
            return MAC_WATCH_EV_NONE;
        }
        // The address it would follow on from: the own one, or the live candidate heard last (a fast changing beacon)
        int64_t before = own;
        int64_t chain = now_ms;
        for (const mac_watch_candidate_t &k : w->cand) {
            if (&k != c && k.used && !k.lookalike && k.last_ms > before) {
                before = k.last_ms;
                chain = k.chain_ms;
            }
        }
        memcpy(c->addr, addr, BLE_ADDR_LEN);
        c->used = true;
        c->chain_ms = chain;
        // A changed address takes over at once; one appearing long after is another beacon coming into range
        c->lookalike = now_ms - before > MAC_WATCH_SEAMLESS_MS;
    }
    c->last_ms = now_ms;
    if (c->lookalike || now_ms - c->chain_ms < MAC_WATCH_CONFIRM_MS || now_ms - own <= MAC_WATCH_GONE_MS) {
        return MAC_WATCH_EV_NONE;
    }
    w->state = MAC_WATCH_CHANGED;
    memcpy(w->new_mac, addr, BLE_ADDR_LEN);
    return MAC_WATCH_EV_STATE;
}

bool mac_watch_needs_all(const mac_watch_t *w)
{
    return w->state == MAC_WATCH_RUNNING && w->has_id;
}

const char *mac_watch_state_name(uint8_t state)
{
    switch (state) {
    case MAC_WATCH_RUNNING:
        return "running";
    case MAC_WATCH_FIXED:
        return "fixed";
    case MAC_WATCH_CHANGED:
        return "changed";
    default:
        return "none";
    }
}

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

size_t mac_watch_encode(const mac_watch_t *w, size_t n, uint8_t *buf, size_t size)
{
    static_assert(MAC_WATCH_BLOB_SIZE(1) == kHeader + kRecord + 4, "blob size macro");
    const size_t total = MAC_WATCH_BLOB_SIZE(n);
    if (buf == nullptr || n > 255 || size < total) {
        return 0;
    }
    memset(buf, 0, total);
    memcpy(buf, kMagic, sizeof(kMagic));
    buf[4] = kVersion;
    buf[5] = static_cast<uint8_t>(n);
    for (size_t i = 0; i < n; i++) {
        uint8_t *p = &buf[kHeader + i * kRecord];
        memcpy(p, w[i].mac, BLE_ADDR_LEN);
        p[6] = w[i].state;
        p[7] = w[i].has_id ? kFlagHasId : 0;
        memcpy(&p[8], w[i].uuid, IBEACON_UUID_LEN);
        put_u16(&p[24], w[i].major);
        put_u16(&p[26], w[i].minor);
        put_u32(&p[28], w[i].watched_ms);
        memcpy(&p[32], w[i].new_mac, BLE_ADDR_LEN);
    }
    put_u32(&buf[total - 4], crc32_ieee(buf, total - 4));
    return total;
}

bool mac_watch_decode(const uint8_t *buf, size_t len, mac_watch_t *w, size_t n)
{
    memset(w, 0, n * sizeof(*w));
    if (buf == nullptr || len < kHeader + 4 || memcmp(buf, kMagic, sizeof(kMagic)) != 0 || buf[4] != kVersion ||
        len != MAC_WATCH_BLOB_SIZE(buf[5]) || get_u32(&buf[len - 4]) != crc32_ieee(buf, len - 4)) {
        return false;
    }
    const size_t count = buf[5] < n ? buf[5] : n; // another slot count: the first slots
    for (size_t i = 0; i < count; i++) {
        const uint8_t *p = &buf[kHeader + i * kRecord];
        if (p[6] > MAC_WATCH_CHANGED || (p[7] & ~kFlagHasId) != 0) {
            continue; // unknown: watched again from the start
        }
        memcpy(w[i].mac, p, BLE_ADDR_LEN);
        w[i].state = p[6];
        w[i].has_id = (p[7] & kFlagHasId) != 0;
        memcpy(w[i].uuid, &p[8], IBEACON_UUID_LEN);
        w[i].major = get_u16(&p[24]);
        w[i].minor = get_u16(&p[26]);
        const uint32_t watched = get_u32(&p[28]);
        w[i].watched_ms = watched > MAC_WATCH_TIME_MS ? MAC_WATCH_TIME_MS : watched;
        memcpy(w[i].new_mac, &p[32], BLE_ADDR_LEN);
    }
    return true;
}
