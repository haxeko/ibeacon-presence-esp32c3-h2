/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "presence_manager.h"

#include <string.h>

#include <atomic>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <nvs.h>

#include "sdkconfig.h"

#include "config_store.h"
#include "mac_watch.h"
#include "presence_logic.h"

static const char *TAG = "presence";

static constexpr int kQueueLength = 32;
static constexpr int8_t kRssiUnavailable = 127;
static constexpr int64_t kTlmMinPeriodMs = 1000; // per slot
static constexpr int64_t kWatchSaveMs = 5 * 60 * 1000;  // MAC watch progress written to flash this often
static constexpr int64_t kWatchStateSaveMs = 60 * 1000; // and a new state at most this often
static_assert(PRESENCE_LOGIC_SLOTS == PRESENCE_SLOT_COUNT, "one presence rule per slot");

static const presence_battery_config_t kBatteryConfig = {
    .empty_mv = CONFIG_PRESENCE_BATTERY_EMPTY_MV,
    .full_mv = CONFIG_PRESENCE_BATTERY_FULL_MV,
    .warning_pct = CONFIG_PRESENCE_BATTERY_WARNING_PCT,
    .critical_pct = CONFIG_PRESENCE_BATTERY_CRITICAL_PCT,
    .step_pct = 2,
    .replaced_pct = 15,
};

namespace {

struct SlotMatch {
    bool active;
    uint8_t addr[BLE_ADDR_LEN];
    uint8_t addr_type;
};

enum PacketKind : uint8_t {
    kPacketIBeacon,
    kPacketTlm,
};

struct Packet {
    uint8_t slot;
    PacketKind kind;
    int8_t rssi;
    uint16_t battery_mv; // kPacketTlm
};

} // namespace

// Built before the scanner starts, read-only afterwards (also from the host task)
static SlotMatch s_match[PRESENCE_SLOT_COUNT];
static char s_labels[PRESENCE_SLOT_COUNT][PRESENCE_LABEL_MAX_BYTES + 1];
static presence_rule_t s_rules[PRESENCE_SLOT_COUNT];
static presence_report_fn_t s_report;
static QueueHandle_t s_queue;

// NimBLE host task only
static int64_t s_last_post_ms[PRESENCE_SLOT_COUNT];
static bool s_posted[PRESENCE_SLOT_COUNT];
static bool s_pending[PRESENCE_SLOT_COUNT];
static int8_t s_pending_rssi[PRESENCE_SLOT_COUNT];
static int64_t s_last_tlm_ms[PRESENCE_SLOT_COUNT];
static bool s_tlm_posted[PRESENCE_SLOT_COUNT];
static uint8_t s_tlm_order[PRESENCE_SLOT_COUNT]; // tlm_byte_order_t of the slot's beacon
static char s_seen_name[PRESENCE_SLOT_COUNT][BLE_SCANNER_NAME_SIZE];

// Advertised names, written by the host task, read by any task
static portMUX_TYPE s_name_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_names[PRESENCE_SLOT_COUNT][BLE_SCANNER_NAME_SIZE];

static std::atomic<uint32_t> s_matched[PRESENCE_SLOT_COUNT];
static std::atomic<uint32_t> s_queue_drops{ 0 };
static std::atomic<uint32_t> s_report_retries{ 0 };

// MAC watch of the slots' beacons (mac_watch.h): updated by the host task from the frames, saved by the presence task
static portMUX_TYPE s_watch_mux = portMUX_INITIALIZER_UNLOCKED;
static mac_watch_t s_watch[PRESENCE_SLOT_COUNT]; // s_watch_mux
static int64_t s_watch_since_ms;                 // this boot's watching started (scan clock)
static std::atomic<bool> s_watch_all{ false };     // a watch needs the iBeacon frames of every address
static std::atomic<uint32_t> s_watch_changed{ 0 }; // slots whose watch changed state: logged and saved
static std::atomic<bool> s_watch_progress{ false };

// Snapshot for the diagnostics, written by the presence task
static portMUX_TYPE s_stats_mux = portMUX_INITIALIZER_UNLOCKED;
static presence_manager_stats_t s_stats;

static bool slot_matches(int slot, const uint8_t addr[BLE_ADDR_LEN], uint8_t addr_type)
{
    const SlotMatch &m = s_match[slot];
    return m.active && memcmp(m.addr, addr, BLE_ADDR_LEN) == 0 && beacon_addr_type_matches(m.addr_type, addr_type);
}

bool presence_manager_filter(const uint8_t addr[BLE_ADDR_LEN], uint8_t addr_type)
{
    // A beacon being watched: its iBeacon identity may come from another address
    if (s_watch_all.load(std::memory_order_relaxed)) {
        return true;
    }
    for (int i = 0; i < PRESENCE_SLOT_COUNT; i++) {
        if (slot_matches(i, addr, addr_type)) {
            return true;
        }
    }
    return false;
}

static bool stronger(int8_t rssi, int8_t than)
{
    if (rssi == kRssiUnavailable) {
        return false;
    }
    return than == kRssiUnavailable || rssi > than;
}

// Host task: the name a slot's beacon advertises, published when it changes
static void note_name(int slot, const char *name)
{
    if (strcmp(s_seen_name[slot], name) == 0) {
        return;
    }
    memcpy(s_seen_name[slot], name, sizeof(s_seen_name[slot]));
    portENTER_CRITICAL(&s_name_mux);
    memcpy(s_names[slot], name, sizeof(s_names[slot]));
    portEXIT_CRITICAL(&s_name_mux);
}

// Host task: a TLM frame of the slot's beacon, at most one per second (it carries the battery only)
static void post_tlm(int slot, const ble_adv_report_t *report)
{
    if (s_tlm_posted[slot] && report->time_ms - s_last_tlm_ms[slot] < kTlmMinPeriodMs) {
        return;
    }
    const uint16_t mv = eddystone_tlm_battery_mv(&report->tlm, &s_tlm_order[slot]);
    if (mv == 0) {
        return;
    }
    const Packet packet = { static_cast<uint8_t>(slot), kPacketTlm, report->rssi, mv };
    if (xQueueSend(s_queue, &packet, 0) == pdTRUE) {
        s_tlm_posted[slot] = true;
        s_last_tlm_ms[slot] = report->time_ms;
    } else {
        s_queue_drops.fetch_add(1, std::memory_order_relaxed);
    }
}

// Host task: an iBeacon frame for the MAC watch of every active slot. Times in scanning time: a paused scanner
// (network down, commissioning) hears nothing, which is no silence of the beacon.
static void watch_frame(const ble_adv_report_t *report)
{
    const int64_t now = static_cast<int64_t>(ble_scanner_scan_clock_ms());
    // The address of a slot's beacon is never another slot's beacon changing its address
    bool slot_address = false;
    for (int i = 0; i < PRESENCE_SLOT_COUNT && !slot_address; i++) {
        slot_address = slot_matches(i, report->addr, report->addr_type);
    }
    uint32_t changed = 0;
    bool progress = false;
    bool all = false;
    portENTER_CRITICAL(&s_watch_mux);
    for (int i = 0; i < PRESENCE_SLOT_COUNT; i++) {
        if (!s_match[i].active) {
            continue;
        }
        mac_watch_t &w = s_watch[i];
        mac_watch_event_t ev = MAC_WATCH_EV_NONE;
        if (slot_matches(i, report->addr, report->addr_type)) {
            ev = mac_watch_own(&w, report->addr_type, &report->ibeacon, now);
        } else if (!slot_address) {
            ev = mac_watch_other(&w, report->addr, report->addr_type, &report->ibeacon, now, s_watch_since_ms);
        }
        changed |= ev == MAC_WATCH_EV_STATE ? 1u << i : 0;
        progress = progress || ev == MAC_WATCH_EV_PROGRESS;
        all = all || mac_watch_needs_all(&w);
    }
    portEXIT_CRITICAL(&s_watch_mux);
    s_watch_all.store(all, std::memory_order_relaxed);
    if (changed != 0) {
        s_watch_changed.fetch_or(changed);
    }
    if (progress) {
        s_watch_progress.store(true, std::memory_order_relaxed);
    }
}

void presence_manager_sink(const ble_adv_report_t *report)
{
    if (report->has_ibeacon) {
        watch_frame(report);
    }
    // Only a well-formed iBeacon frame refreshes a slot; the advertised name is never used to identify a beacon.
    for (int i = 0; i < PRESENCE_SLOT_COUNT; i++) {
        if (!slot_matches(i, report->addr, report->addr_type)) {
            continue;
        }
        if (report->name[0] != '\0') {
            note_name(i, report->name);
        }
        if (report->has_tlm) {
            post_tlm(i, report);
        }
        if (!report->has_ibeacon) {
            continue;
        }
        s_matched[i].fetch_add(1, std::memory_order_relaxed);
        if (!s_pending[i] || stronger(report->rssi, s_pending_rssi[i])) {
            s_pending_rssi[i] = report->rssi;
        }
        s_pending[i] = true;
        if (s_posted[i] && report->time_ms - s_last_post_ms[i] < CONFIG_PRESENCE_REPORT_THROTTLE_MS) {
            continue;
        }
        const Packet packet = { static_cast<uint8_t>(i), kPacketIBeacon, s_pending_rssi[i], 0 };
        if (xQueueSend(s_queue, &packet, 0) == pdTRUE) {
            s_posted[i] = true;
            s_pending[i] = false;
            s_last_post_ms[i] = report->time_ms;
        } else {
            s_queue_drops.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

// Presence task only
static presence_battery_t s_battery[PRESENCE_SLOT_COUNT];
static uint32_t s_tlm_count[PRESENCE_SLOT_COUNT];
static int64_t s_tlm_time_ms[PRESENCE_SLOT_COUNT];

static void publish_stats(const presence_logic_t &logic, const bool reported[PRESENCE_SLOT_COUNT], uint64_t now)
{
    const int64_t now_ms = esp_timer_get_time() / 1000;
    static presence_manager_stats_t st; // presence task only: off its stack (20 slots)
    memset(&st, 0, sizeof(st));
    for (int i = 0; i < PRESENCE_SLOT_COUNT; i++) {
        presence_slot_stats_t &s = st.slots[i];
        s.active = s_rules[i].active;
        s.present = logic.state[i].present;
        s.reported = reported[i] == logic.state[i].present;
        s.last_rssi = logic.state[i].last_rssi;
        s.age_s = static_cast<uint32_t>(presence_logic_age_ms(&logic, i, now) / 1000);
        s.matched = s_matched[i].load(std::memory_order_relaxed);
        s.accepted = logic.state[i].accepted;
        s.rssi_rejected = logic.state[i].rssi_rejected;
        portENTER_CRITICAL(&s_watch_mux);
        s.watch_state = s_watch[i].state;
        s.watch_s = s_watch[i].watched_ms / 1000;
        portEXIT_CRITICAL(&s_watch_mux);
        s.tlm = s_tlm_count[i];
        s.battery_mv = s_battery[i].median_mv;
        s.battery_pct = s_battery[i].published;
        s.battery_level = presence_battery_level(&kBatteryConfig, s_battery[i].published);
        s.battery_age_s = s_tlm_count[i] > 0 ? static_cast<uint32_t>((now_ms - s_tlm_time_ms[i]) / 1000) : 0;
    }
    st.queue_drops = s_queue_drops.load();
    st.report_retries = s_report_retries.load();
    portENTER_CRITICAL(&s_stats_mux);
    s_stats = st;
    portEXIT_CRITICAL(&s_stats_mux);
}

// Presence task: a battery reading of the slot's beacon
static void on_battery(int slot, uint16_t mv)
{
    if (slot < 0 || slot >= PRESENCE_SLOT_COUNT) {
        return;
    }
    s_tlm_count[slot]++;
    s_tlm_time_ms[slot] = esp_timer_get_time() / 1000;
    const uint8_t before = s_battery[slot].published;
    if (presence_battery_add(&s_battery[slot], &kBatteryConfig, mv)) {
        const uint8_t pct = s_battery[slot].published;
        ESP_LOGI(TAG, "Presence %d \"%s\": battery %u%% (%u mV)%s", slot + 1, s_labels[slot], pct,
                 s_battery[slot].median_mv,
                 before == PRESENCE_BATTERY_UNKNOWN ? ""
                 : presence_battery_level(&kBatteryConfig, pct) == PRESENCE_BATTERY_CRITICAL ? ": replace it"
                 : presence_battery_level(&kBatteryConfig, pct) == PRESENCE_BATTERY_WARNING ? ": low"
                                                                                            : "");
    }
}

// Presence task (and start-up): the records to flash
static void watch_save(const mac_watch_t *records)
{
    const esp_err_t err = config_store_save_watch(records, PRESENCE_SLOT_COUNT);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "MAC watch not saved (%s): it goes on in memory", esp_err_to_name(err));
    }
}

static void watch_log(int slot, const mac_watch_t &w)
{
    char mac[BLE_ADDR_STR_SIZE];
    ble_addr_format(w.mac, mac);
    switch (w.state) {
    case MAC_WATCH_FIXED:
        ESP_LOGI(TAG, "Presence %d \"%s\": %s keeps its MAC (heard %u min, or a public address): no warning", slot + 1,
                 s_labels[slot], mac, static_cast<unsigned>(w.watched_ms / 60000));
        break;
    case MAC_WATCH_CHANGED: {
        char other[BLE_ADDR_STR_SIZE];
        ble_addr_format(w.new_mac, other);
        ESP_LOGW(TAG, "Presence %d \"%s\": %s silent while its iBeacon identity comes from %s: the beacon changes its "
                      "MAC, this slot will lose it",
                 slot + 1, s_labels[slot], mac, other);
        break;
    }
    case MAC_WATCH_RUNNING:
        ESP_LOGI(TAG, "Presence %d \"%s\": MAC watch of %s: %u of %u min heard", slot + 1, s_labels[slot], mac,
                 static_cast<unsigned>(w.watched_ms / 60000), static_cast<unsigned>(MAC_WATCH_TIME_MS / 60000));
        break;
    default:
        break;
    }
}

// Presence task: new states logged at once and saved within kWatchStateSaveMs, progress every kWatchSaveMs (flash
// writes stay rare whatever the frames do)
static void watch_service()
{
    static mac_watch_t copy[PRESENCE_SLOT_COUNT];
    static int64_t last_save_ms;
    static bool state_pending;
    const int64_t now = esp_timer_get_time() / 1000;
    const uint32_t changed = s_watch_changed.exchange(0);
    if (changed != 0) {
        portENTER_CRITICAL(&s_watch_mux);
        memcpy(copy, s_watch, sizeof(copy));
        portEXIT_CRITICAL(&s_watch_mux);
        for (int i = 0; i < PRESENCE_SLOT_COUNT; i++) {
            if (changed & (1u << i)) {
                watch_log(i, copy[i]);
            }
        }
        state_pending = true;
    }
    const bool progress = s_watch_progress.load(std::memory_order_relaxed);
    const bool due = (state_pending && now - last_save_ms >= kWatchStateSaveMs) ||
                     (progress && now - last_save_ms >= kWatchSaveMs);
    if (!due) {
        return;
    }
    s_watch_progress.store(false, std::memory_order_relaxed);
    state_pending = false;
    portENTER_CRITICAL(&s_watch_mux);
    memcpy(copy, s_watch, sizeof(copy));
    portEXIT_CRITICAL(&s_watch_mux);
    watch_save(copy);
    last_save_ms = now;
}

// Start-up: the records of the last boot; a slot with another MAC than its record starts a new watch
static void watch_start(const presence_config_t *cfg)
{
    const esp_err_t err = config_store_load_watch(s_watch, PRESENCE_SLOT_COUNT);
    if (err != ESP_OK && err != ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGW(TAG, "MAC watch records not read (%s): every beacon watched again", esp_err_to_name(err));
    }
    bool reset = false;
    bool all = false;
    for (int i = 0; i < PRESENCE_SLOT_COUNT; i++) {
        if (!presence_slot_is_active(&cfg->slots[i])) {
            continue; // its record is kept: the same MAC enabled again keeps its verdict
        }
        reset = mac_watch_assign(&s_watch[i], cfg->slots[i].addr) || reset;
        all = all || mac_watch_needs_all(&s_watch[i]);
        watch_log(i, s_watch[i]);
    }
    s_watch_since_ms = static_cast<int64_t>(ble_scanner_scan_clock_ms());
    s_watch_all.store(all);
    if (reset) {
        watch_save(s_watch);
    }
}

static void presence_task(void *)
{
    presence_logic_t logic;
    presence_logic_init(&logic, s_rules);
    bool reported[PRESENCE_SLOT_COUNT] = {}; // the Occupancy attribute starts unoccupied at boot
    bool retry_logged[PRESENCE_SLOT_COUNT] = {};

    for (;;) {
        // Apply every queued packet first, then the timeouts: a packet waiting in the queue is never late enough
        // to turn its slot off.
        Packet packet;
        if (xQueueReceive(s_queue, &packet, pdMS_TO_TICKS(250)) == pdTRUE) {
            do {
                if (packet.kind == kPacketTlm) {
                    on_battery(packet.slot, packet.battery_mv);
                    continue;
                }
                const uint64_t now = ble_scanner_scan_clock_ms();
                if (presence_logic_on_packet(&logic, packet.slot, packet.rssi, now)) {
                    ESP_LOGI(TAG, "Presence %d \"%s\": ON (RSSI %d dBm)", packet.slot + 1, s_labels[packet.slot],
                             packet.rssi);
                }
            } while (xQueueReceive(s_queue, &packet, 0) == pdTRUE);
        }

        const uint64_t now = ble_scanner_scan_clock_ms();
        const uint32_t turned_off = presence_logic_tick(&logic, now);
        for (int i = 0; i < PRESENCE_SLOT_COUNT; i++) {
            if (turned_off & (1u << i)) {
                ESP_LOGI(TAG, "Presence %d \"%s\": OFF (no valid packet for %u s of scanning)", i + 1, s_labels[i],
                         static_cast<unsigned>(s_rules[i].timeout_ms / 1000));
            }
        }

        // Deliver changes; the report function takes the Matter stack lock, this task holds no other lock.
        for (int i = 0; i < PRESENCE_SLOT_COUNT; i++) {
            const bool present = logic.state[i].present;
            if (present == reported[i]) {
                continue;
            }
            if (s_report(i, present)) {
                reported[i] = present;
                retry_logged[i] = false;
            } else {
                s_report_retries.fetch_add(1);
                if (!retry_logged[i]) {
                    ESP_LOGW(TAG, "Presence %d: Matter not ready, report retried", i + 1);
                    retry_logged[i] = true;
                }
            }
        }
        publish_stats(logic, reported, now);
        watch_service();
    }
}

esp_err_t presence_manager_start(const presence_config_t *cfg, presence_report_fn_t report)
{
    s_report = report;
    for (int i = 0; i < PRESENCE_SLOT_COUNT; i++) {
        presence_battery_init(&s_battery[i]);
    }
    for (int i = 0; i < PRESENCE_SLOT_COUNT; i++) {
        const presence_slot_config_t &s = cfg->slots[i];
        s_match[i].active = presence_slot_is_active(&s);
        memcpy(s_match[i].addr, s.addr, BLE_ADDR_LEN);
        s_match[i].addr_type = s.addr_type;
        s_rules[i].active = s_match[i].active;
        s_rules[i].timeout_ms = static_cast<uint32_t>(s.timeout_s) * 1000;
        s_rules[i].rssi_filter = s.rssi_filter;
        s_rules[i].rssi_threshold = s.rssi_threshold;
        s_rules[i].rssi_hysteresis = s.rssi_hysteresis;
        if (s.label[0] != '\0') {
            memcpy(s_labels[i], s.label, sizeof(s_labels[i]));
        } else {
            presence_config_default_label(i, s_labels[i], sizeof(s_labels[i]));
        }

        char addr[BLE_ADDR_STR_SIZE];
        ble_addr_format(s.addr, addr);
        if (s_match[i].active) {
            ESP_LOGI(TAG, "Presence %d \"%s\": %s (%s), absent after %u s%s", i + 1, s_labels[i], addr,
                     beacon_addr_type_name(s.addr_type), s.timeout_s, s.rssi_filter ? ", RSSI filter" : "");
        } else {
            ESP_LOGI(TAG, "Presence %d \"%s\": %s, always off", i + 1, s_labels[i],
                     s.enabled ? "no beacon assigned" : "disabled");
        }
    }
    watch_start(cfg);
    s_queue = xQueueCreate(kQueueLength, sizeof(Packet));
    if (s_queue == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    return xTaskCreate(presence_task, "presence", 4096, nullptr, 5, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void presence_manager_get_stats(presence_manager_stats_t *out)
{
    portENTER_CRITICAL(&s_stats_mux);
    *out = s_stats;
    portEXIT_CRITICAL(&s_stats_mux);
}

void presence_manager_get_slot_info(int slot, presence_slot_info_t *out)
{
    memset(out, 0, sizeof(*out));
    out->battery_pct = PRESENCE_BATTERY_UNKNOWN;
    if (slot < 0 || slot >= PRESENCE_SLOT_COUNT || s_queue == nullptr) {
        return; // not started (CONFIG mode)
    }
    portENTER_CRITICAL(&s_stats_mux);
    const presence_slot_stats_t &s = s_stats.slots[slot];
    if (s.tlm > 0 && s.battery_pct != PRESENCE_BATTERY_UNKNOWN) { // tlm 0: nothing read (or not published yet)
        out->battery_pct = s.battery_pct;
        out->battery_level = s.battery_level;
        out->battery_mv = s.battery_mv;
    }
    portEXIT_CRITICAL(&s_stats_mux);
    portENTER_CRITICAL(&s_name_mux);
    memcpy(out->name, s_names[slot], sizeof(out->name));
    portEXIT_CRITICAL(&s_name_mux);
}

const presence_battery_config_t *presence_battery_kconfig(void)
{
    return &kBatteryConfig;
}
