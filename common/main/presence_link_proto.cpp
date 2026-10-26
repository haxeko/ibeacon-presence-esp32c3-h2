/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "presence_link_proto.h"

#include <string.h>

#include "crc32.h"
#include "json_util.h"

static constexpr uint8_t kMagic0 = 0xA5;
static constexpr uint8_t kMagic1 = 0x5A;
static constexpr size_t kHeader = 5;                                      // magic, magic, version, type, length
static constexpr size_t kStateLen = 1 + 1 + 4 + 4 + 4 + 4;                // mode, flags, 3 masks, uptime
static constexpr size_t kStatusLen = 1 + 1 + 4 + 4 + 4 + 4 + 2 + 2 + 2 * PLINK_SLOT_COUNT;
static constexpr size_t kSlotMinLen = 1 + 1 + PLINK_MAC_LEN + 1 + 1 + 2 + 1; // then the model
static constexpr size_t kPairingMinLen = PLINK_MANUAL_CODE_LEN + 1;        // manual code, QR length, QR code
static constexpr size_t kCommandLen = 5;
static constexpr uint8_t kCommandCheck[4] = { 'P', 'L', 'C', 'M' };
static constexpr uint8_t kSlotFlagActive = 0x01;
static constexpr uint8_t kSlotFlagModelConfigured = 0x02;

static_assert(PLINK_SLOT_COUNT <= 32, "slot masks are 32 bits");
static_assert(kStateLen <= PLINK_MAX_PAYLOAD && kStatusLen <= PLINK_MAX_PAYLOAD &&
                  kSlotMinLen + PLINK_MODEL_MAX <= PLINK_MAX_PAYLOAD &&
                  kPairingMinLen + PLINK_QR_CODE_MAX <= PLINK_MAX_PAYLOAD,
              "payload too long for PLINK_MAX_PAYLOAD");

static void put_u16(uint8_t *p, uint16_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}

static uint16_t get_u16(const uint8_t *p)
{
    return static_cast<uint16_t>(p[0] | p[1] << 8);
}

static void put_u32(uint8_t *p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

static uint32_t get_u32(const uint8_t *p)
{
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

uint32_t plink_known_mask(uint32_t active_mask, uint32_t reported_mask, const uint32_t *timeout_ms, int slots,
                          uint64_t scan_clock_ms, uint64_t uptime_ms)
{
    uint32_t known = ~active_mask | reported_mask;
    for (int i = 0; i < slots && i < 32; i++) {
        const uint64_t timeout = timeout_ms[i];
        if (scan_clock_ms >= timeout || uptime_ms >= timeout + PLINK_UNKNOWN_GRACE_MS) {
            known |= 1u << i;
        }
    }
    return known;
}

size_t plink_encode(uint8_t type, const uint8_t *payload, size_t len, uint8_t *out, size_t out_size)
{
    const size_t total = kHeader + len + 4;
    if (len > PLINK_MAX_PAYLOAD || total > out_size) {
        return 0;
    }
    out[0] = kMagic0;
    out[1] = kMagic1;
    out[2] = PLINK_VERSION;
    out[3] = type;
    out[4] = static_cast<uint8_t>(len);
    if (len > 0) {
        memcpy(&out[kHeader], payload, len);
    }
    put_u32(&out[kHeader + len], crc32_ieee(&out[2], 3 + len));
    return total;
}

size_t plink_encode_state(const plink_state_t *state, uint8_t *out, size_t out_size)
{
    uint8_t p[kStateLen];
    const uint32_t active = state->active_mask & PLINK_SLOT_MASK;
    p[0] = state->mode;
    p[1] = state->flags;
    put_u32(&p[2], active);
    put_u32(&p[6], state->present_mask & active);
    put_u32(&p[10], state->known_mask & PLINK_SLOT_MASK);
    put_u32(&p[14], state->uptime_s);
    return plink_encode(PLINK_TYPE_STATE, p, sizeof(p), out, out_size);
}

size_t plink_encode_status(const plink_status_t *status, uint8_t *out, size_t out_size)
{
    uint8_t p[kStatusLen];
    p[0] = status->flags;
    p[1] = status->fabrics;
    put_u32(&p[2], status->uptime_s);
    put_u32(&p[6], status->bridged_mask & PLINK_SLOT_MASK);
    put_u32(&p[10], status->waiting_mask & PLINK_SLOT_MASK);
    put_u32(&p[14], status->error_mask & PLINK_SLOT_MASK);
    put_u16(&p[18], status->heap_free_kb);
    put_u16(&p[20], status->heap_min_kb);
    for (int i = 0; i < PLINK_SLOT_COUNT; i++) {
        put_u16(&p[22 + 2 * i], status->endpoint[i]);
    }
    return plink_encode(PLINK_TYPE_STATUS, p, sizeof(p), out, out_size);
}

size_t plink_encode_slot(const plink_slot_t *slot, uint8_t *out, size_t out_size)
{
    const size_t model_len = slot->active ? strnlen(slot->model, sizeof(slot->model)) : 0;
    if (slot->slot >= PLINK_SLOT_COUNT || model_len > PLINK_MODEL_MAX) {
        return 0;
    }
    uint8_t p[kSlotMinLen + PLINK_MODEL_MAX] = {};
    p[0] = slot->slot;
    p[1] = slot->active ? kSlotFlagActive : 0;
    if (slot->active && slot->model_configured && model_len > 0) {
        p[1] |= kSlotFlagModelConfigured;
    }
    p[8] = PLINK_BATTERY_UNKNOWN;
    if (slot->active) {
        memcpy(&p[2], slot->mac, PLINK_MAC_LEN);
        p[8] = slot->battery_pct;
        p[9] = slot->battery_level;
        put_u16(&p[10], slot->battery_mv);
    }
    p[12] = static_cast<uint8_t>(model_len);
    memcpy(&p[kSlotMinLen], slot->model, model_len);
    return plink_encode(PLINK_TYPE_SLOT, p, kSlotMinLen + model_len, out, out_size);
}

size_t plink_encode_pairing(const plink_pairing_t *pairing, uint8_t *out, size_t out_size)
{
    const size_t manual_len = strnlen(pairing->manual_code, sizeof(pairing->manual_code));
    const size_t qr_len = strnlen(pairing->qr_code, sizeof(pairing->qr_code));
    if (manual_len != PLINK_MANUAL_CODE_LEN || qr_len > PLINK_QR_CODE_MAX) {
        return 0;
    }
    uint8_t p[kPairingMinLen + PLINK_QR_CODE_MAX];
    memcpy(p, pairing->manual_code, PLINK_MANUAL_CODE_LEN);
    p[PLINK_MANUAL_CODE_LEN] = static_cast<uint8_t>(qr_len);
    memcpy(&p[kPairingMinLen], pairing->qr_code, qr_len);
    return plink_encode(PLINK_TYPE_PAIRING, p, kPairingMinLen + qr_len, out, out_size);
}

size_t plink_encode_command(const plink_command_t *command, uint8_t *out, size_t out_size)
{
    uint8_t p[kCommandLen];
    p[0] = command->command;
    memcpy(&p[1], kCommandCheck, sizeof(kCommandCheck));
    return plink_encode(PLINK_TYPE_COMMAND, p, sizeof(p), out, out_size);
}

bool plink_decode_state(const uint8_t *payload, size_t len, plink_state_t *state)
{
    if (len != kStateLen) {
        return false;
    }
    const uint32_t active = get_u32(&payload[2]) & PLINK_SLOT_MASK;
    state->mode = payload[0];
    state->flags = payload[1];
    state->active_mask = active;
    state->present_mask = get_u32(&payload[6]) & active;
    state->known_mask = (get_u32(&payload[10]) | ~active) & PLINK_SLOT_MASK;
    state->uptime_s = get_u32(&payload[14]);
    return true;
}

bool plink_decode_status(const uint8_t *payload, size_t len, plink_status_t *status)
{
    if (len != kStatusLen) {
        return false;
    }
    status->flags = payload[0];
    status->fabrics = payload[1];
    status->uptime_s = get_u32(&payload[2]);
    status->bridged_mask = get_u32(&payload[6]) & PLINK_SLOT_MASK;
    status->waiting_mask = get_u32(&payload[10]) & PLINK_SLOT_MASK & ~status->bridged_mask;
    status->error_mask = get_u32(&payload[14]) & status->waiting_mask;
    status->heap_free_kb = get_u16(&payload[18]);
    status->heap_min_kb = get_u16(&payload[20]);
    for (int i = 0; i < PLINK_SLOT_COUNT; i++) {
        const bool bridged = status->bridged_mask & (1u << i);
        status->endpoint[i] = bridged ? get_u16(&payload[22 + 2 * i]) : 0;
    }
    return true;
}

bool plink_decode_slot(const uint8_t *payload, size_t len, plink_slot_t *slot)
{
    if (len < kSlotMinLen || payload[0] >= PLINK_SLOT_COUNT ||
        (payload[1] & ~(kSlotFlagActive | kSlotFlagModelConfigured)) != 0) {
        return false;
    }
    const size_t model_len = payload[12];
    const bool active = payload[1] & kSlotFlagActive;
    const uint8_t pct = payload[8];
    if (model_len > PLINK_MODEL_MAX || len != kSlotMinLen + model_len || (pct > 100 && pct != PLINK_BATTERY_UNKNOWN) ||
        payload[9] > 2 || (!active && model_len > 0)) {
        return false;
    }
    // The model becomes a Matter string (ProductName): UTF-8 text only
    if (!utf8_is_valid_text(reinterpret_cast<const char *>(&payload[kSlotMinLen]), model_len)) {
        return false;
    }
    memset(slot, 0, sizeof(*slot));
    slot->slot = payload[0];
    slot->active = active;
    slot->battery_pct = PLINK_BATTERY_UNKNOWN;
    if (active) {
        slot->model_configured = (payload[1] & kSlotFlagModelConfigured) != 0 && model_len > 0;
        memcpy(slot->mac, &payload[2], PLINK_MAC_LEN);
        slot->battery_pct = pct;
        slot->battery_level = payload[9];
        slot->battery_mv = get_u16(&payload[10]);
        memcpy(slot->model, &payload[kSlotMinLen], model_len);
    }
    return true;
}

static bool is_digits(const uint8_t *p, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        if (p[i] < '0' || p[i] > '9') {
            return false;
        }
    }
    return true;
}

// Characters of a Matter QR code payload: "MT:" then base-38 (0-9, A-Z, '-', '.')
static bool is_qr_text(const uint8_t *p, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        const uint8_t c = p[i];
        const bool ok = (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || c == '-' || c == '.' || c == ':';
        if (!ok) {
            return false;
        }
    }
    return true;
}

bool plink_decode_pairing(const uint8_t *payload, size_t len, plink_pairing_t *pairing)
{
    if (len < kPairingMinLen || !is_digits(payload, PLINK_MANUAL_CODE_LEN)) {
        return false;
    }
    const size_t qr_len = payload[PLINK_MANUAL_CODE_LEN];
    if (qr_len > PLINK_QR_CODE_MAX || len != kPairingMinLen + qr_len || !is_qr_text(&payload[kPairingMinLen], qr_len)) {
        return false;
    }
    memset(pairing, 0, sizeof(*pairing));
    memcpy(pairing->manual_code, payload, PLINK_MANUAL_CODE_LEN);
    memcpy(pairing->qr_code, &payload[kPairingMinLen], qr_len);
    return true;
}

bool plink_decode_command(const uint8_t *payload, size_t len, plink_command_t *command)
{
    if (len != kCommandLen || memcmp(&payload[1], kCommandCheck, sizeof(kCommandCheck)) != 0) {
        return false;
    }
    command->command = payload[0];
    return true;
}

plink_slot_action_t plink_slot_action(bool have, const uint8_t have_mac[PLINK_MAC_LEN], bool want,
                                      const uint8_t want_mac[PLINK_MAC_LEN])
{
    if (!want) {
        return have ? PLINK_SLOT_REMOVE : PLINK_SLOT_KEEP;
    }
    if (!have) {
        return PLINK_SLOT_ADD;
    }
    return memcmp(have_mac, want_mac, PLINK_MAC_LEN) == 0 ? PLINK_SLOT_KEEP : PLINK_SLOT_REPLACE;
}

void plink_mac_string(const uint8_t mac[PLINK_MAC_LEN], char out[PLINK_MAC_STR_SIZE])
{
    static const char kHex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (int i = 0; i < PLINK_MAC_LEN; i++) {
        if (i > 0) {
            out[o++] = ':';
        }
        out[o++] = kHex[mac[i] >> 4];
        out[o++] = kHex[mac[i] & 0x0F];
    }
    out[o] = '\0';
}

void plink_parser_init(plink_parser_t *p)
{
    memset(p, 0, sizeof(*p));
}

bool plink_parser_feed(plink_parser_t *p, uint8_t byte)
{
    if (p->have == 0) {
        if (byte == kMagic0) {
            p->buf[p->have++] = byte;
        }
        return false;
    }
    if (p->have == 1) {
        if (byte == kMagic1) {
            p->buf[p->have++] = byte;
        } else {
            p->have = byte == kMagic0 ? 1 : 0; // A5 A5 5A: the second A5 may start the frame
        }
        return false;
    }
    p->buf[p->have++] = byte;
    if (p->have == kHeader) {
        if (p->buf[2] != PLINK_VERSION || p->buf[4] > PLINK_MAX_PAYLOAD) {
            p->bad_frames++;
            p->have = 0;
        }
        return false;
    }
    if (p->have < kHeader) {
        return false;
    }
    const size_t len = p->buf[4];
    if (p->have < kHeader + len + 4) {
        return false;
    }
    p->have = 0;
    if (get_u32(&p->buf[kHeader + len]) != crc32_ieee(&p->buf[2], 3 + len)) {
        p->crc_errors++;
        return false;
    }
    p->type = p->buf[3];
    p->len = static_cast<uint8_t>(len);
    p->payload = &p->buf[kHeader];
    p->frames++;
    return true;
}
