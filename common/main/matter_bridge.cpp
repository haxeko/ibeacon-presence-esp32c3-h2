/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "matter_bridge.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <nvs.h>

#include <app/clusters/power-source-server/power-source-server.h>

#include "sdkconfig.h"

#include "crc32.h"
#include "presence_config.h"

static_assert(PLINK_SLOT_COUNT == PRESENCE_SLOT_COUNT, "one bridged device per presence slot");
static_assert(CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT >= PLINK_SLOT_COUNT + 2,
              "endpoints: root node, Aggregator and one per slot (CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT)");

static const char *TAG = "bridge";

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

static constexpr const char *kNamespace = "pbridge";
static constexpr const char *kKey = "table";
static constexpr const char *kKeyUnreadable = "table_bad"; // a table this firmware cannot read, kept as it was
static constexpr uint32_t kMagicV1 = 0x50425231; // "PBR1": five slots, endpoint and MAC only
static constexpr uint32_t kMagic = 0x50425232;   // "PBR2"
static constexpr int kSlotsV1 = 5;
// Heap a device takes, rounded up (the real cost of each one is logged); a device is only built with this much
// free above the reserve
static constexpr size_t kDeviceCost = 4096;
static constexpr size_t kLargestBlockMin = 2048;
// After a device that could not be built: 30 s, doubled at each failure up to an hour (each attempt to add a device
// uses up an endpoint ID). A saved device that cannot be restored is retried this way kRestoreAttempts times.
static constexpr int64_t kErrorRetryMs = 30000;
static constexpr int64_t kErrorRetryMaxMs = 3600000;
static constexpr uint8_t kRestoreAttempts = 3;
static constexpr char kDefaultModel[] = "iBeacon";
static constexpr char kPowerSourceDescription[] = "Battery";

namespace {

// As stored: no padding, fixed layout
struct Entry {
    uint16_t endpoint; // 0: no device
    uint8_t mac[PLINK_MAC_LEN];
    uint8_t battery_pct; // PLINK_BATTERY_UNKNOWN until known
    uint8_t battery_level;
    char model[PLINK_MODEL_MAX + 1]; // "" until known
    uint8_t flags;                   // kEntryModelConfigured
};
constexpr uint8_t kEntryModelConfigured = 0x01; // model typed on the scanner's page (else its advertised name)
static_assert(sizeof(Entry) == 44, "stored entry layout");

struct Table {
    uint32_t magic;
    Entry entries[PLINK_SLOT_COUNT];
    uint32_t crc; // of magic and entries
};

struct EntryV1 {
    uint16_t endpoint;
    uint8_t mac[PLINK_MAC_LEN];
};

struct TableV1 {
    uint32_t magic;
    EntryV1 entries[kSlotsV1];
    uint32_t crc;
};

} // namespace

static node_t *s_node;
static uint16_t s_aggregator = chip::kInvalidEndpointId;
static uint32_t s_occupancy_feature;
static Entry s_entries[PLINK_SLOT_COUNT]; // the table: devices built or to restore
static bool s_built[PLINK_SLOT_COUNT];    // the entry's endpoint exists in the data model
static bool s_waiting[PLINK_SLOT_COUNT];  // should have a device that is not there yet
static bool s_error[PLINK_SLOT_COUNT];    // waiting: it could not be built (retried at s_retry_ms)
static int64_t s_retry_ms[PLINK_SLOT_COUNT];
static int64_t s_retry_delay_ms[PLINK_SLOT_COUNT];
static uint8_t s_failures[PLINK_SLOT_COUNT]; // of the restore of a saved device
static bool s_ready;  // table loaded; false again once erased (factory reset running)
static bool s_reachable = true;
static std::atomic<bool> s_commissioned{ false };
static size_t s_last_cost;
static size_t s_max_cost;

static int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

static uint32_t table_crc(const Table &t)
{
    return crc32_ieee(reinterpret_cast<const uint8_t *>(&t), offsetof(Table, crc));
}

static uint32_t table_v1_crc(const TableV1 &t)
{
    return crc32_ieee(reinterpret_cast<const uint8_t *>(&t), offsetof(TableV1, crc));
}

// Buffer of save_table() and load_table(): both run with the CHIP lock held, one at a time
static Table s_table;

static void save_table()
{
    Table &t = s_table;
    memset(&t, 0, sizeof(t));
    t.magic = kMagic;
    memcpy(t.entries, s_entries, sizeof(t.entries));
    t.crc = table_crc(t);
    nvs_handle_t h;
    esp_err_t err = nvs_open(kNamespace, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_blob(h, kKey, &t, sizeof(t));
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Saving the bridged device table failed: %s (the devices come back from the scanner's list)",
                 esp_err_to_name(err));
    }
}

// A saved table this firmware cannot read (another firmware's format, damaged): copied as it is to "table_bad"
// before the bridge rebuilds its own from the scanner's list and saves it over "table". Nothing is erased.
static void keep_unreadable_table(size_t len)
{
    ESP_LOGE(TAG, "Bridged device table unreadable (%u bytes): kept as %s/%s, devices rebuilt from the scanner's list "
                  "on new endpoints",
             static_cast<unsigned>(len), kNamespace, kKeyUnreadable);
    nvs_handle_t h;
    if (len == 0 || len > 4096 || nvs_open(kNamespace, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    size_t kept = 0;
    uint8_t *copy = static_cast<uint8_t *>(malloc(len));
    if (copy != nullptr && nvs_get_blob(h, kKeyUnreadable, nullptr, &kept) == ESP_ERR_NVS_NOT_FOUND &&
        nvs_get_blob(h, kKey, copy, &len) == ESP_OK && nvs_set_blob(h, kKeyUnreadable, copy, len) == ESP_OK) {
        nvs_commit(h);
    }
    free(copy);
    nvs_close(h);
}

// The table saved by this firmware, or by the five-slot one (converted)
static bool load_table()
{
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READONLY, &h) != ESP_OK) {
        return false; // nothing saved yet
    }
    Table &t = s_table;
    size_t len = 0;
    esp_err_t err = nvs_get_blob(h, kKey, nullptr, &len);
    bool ok = false;
    if (err == ESP_OK && len == sizeof(Table)) {
        err = nvs_get_blob(h, kKey, &t, &len);
        ok = err == ESP_OK && t.magic == kMagic && t.crc == table_crc(t);
        if (ok) {
            memcpy(s_entries, t.entries, sizeof(s_entries));
        }
    } else if (err == ESP_OK && len == sizeof(TableV1)) {
        TableV1 v1;
        err = nvs_get_blob(h, kKey, &v1, &len);
        ok = err == ESP_OK && v1.magic == kMagicV1 && v1.crc == table_v1_crc(v1);
        if (ok) {
            memset(s_entries, 0, sizeof(s_entries));
            for (int i = 0; i < kSlotsV1; i++) {
                s_entries[i].endpoint = v1.entries[i].endpoint;
                memcpy(s_entries[i].mac, v1.entries[i].mac, PLINK_MAC_LEN);
                s_entries[i].battery_pct = PLINK_BATTERY_UNKNOWN;
            }
            ESP_LOGI(TAG, "Bridged device table of the five-slot firmware converted");
        }
    }
    nvs_close(h);
    if (!ok && err != ESP_ERR_NVS_NOT_FOUND) {
        keep_unreadable_table(len);
    }
    return ok;
}

static const char *shown_model(const Entry &e)
{
    return e.model[0] != '\0' ? e.model : kDefaultModel;
}

// BatPercentRemaining is in half percent, null when unknown
static nullable<uint8_t> percent_value(uint8_t pct)
{
    return pct == PLINK_BATTERY_UNKNOWN ? nullable<uint8_t>() : nullable<uint8_t>(static_cast<uint8_t>(pct * 2));
}

// The Power Source servers of esp-matter keep the EndpointList of each endpoint at an index that counts the
// endpoints before it having the cluster: after a device is added or removed the indexes move, so every list is set
// again
static void refresh_endpoint_lists()
{
    for (int i = 0; i < PLINK_SLOT_COUNT; i++) {
        if (!s_built[i]) {
            continue;
        }
        chip::EndpointId id = s_entries[i].endpoint;
        const CHIP_ERROR err = PowerSourceServer::Instance().SetEndpointList(id, chip::Span<chip::EndpointId>(&id, 1));
        if (err != CHIP_NO_ERROR) {
            ESP_LOGW(TAG, "Power Source EndpointList of endpoint %u not set: %" CHIP_ERROR_FORMAT, id, err.Format());
        }
    }
}

// The endpoint of a bridged device, child of the Aggregator. resume_id: the endpoint ID it had before the restart,
// or kInvalidEndpointId for a new one.
static endpoint_t *build_device(int slot, const Entry &e, uint16_t resume_id)
{
    bridged_node::config_t config;
    config.bridged_device_basic_information.reachable = s_reachable;
    // Stable and unique per device, as the specification asks: the slot and the beacon
    const uint8_t number = static_cast<uint8_t>(slot + 1); // 1-20: three digits at most for the compiler too
    snprintf(config.bridged_device_basic_information.unique_id, sizeof(config.bridged_device_basic_information.unique_id),
             "presence-%u-%02X%02X%02X%02X%02X%02X", number, e.mac[0], e.mac[1], e.mac[2], e.mac[3], e.mac[4],
             e.mac[5]);
    // What bridged_node::create() / resume() do, step by step, so that an endpoint added to the node is always known
    // here and removed again if a later step fails. resume() adds nothing when it fails (the ID is in use, or was
    // never given out by this node).
    const uint8_t flags = ENDPOINT_FLAG_DESTROYABLE | ENDPOINT_FLAG_BRIDGE;
    endpoint_t *ep = resume_id == chip::kInvalidEndpointId ? endpoint::create(s_node, flags, nullptr)
                                                          : endpoint::resume(s_node, flags, resume_id, nullptr);
    if (ep == nullptr) {
        return nullptr;
    }
    if (cluster::descriptor::create(ep, &config.descriptor, CLUSTER_FLAG_SERVER) == nullptr ||
        bridged_node::add(ep, &config) != ESP_OK) {
        endpoint::destroy(s_node, ep);
        return nullptr;
    }
    char mac[PLINK_MAC_STR_SIZE];
    plink_mac_string(e.mac, mac);
    char vendor[] = CONFIG_PRESENCE_VENDOR_NAME;
    char model[PLINK_MODEL_MAX + 1];
    snprintf(model, sizeof(model), "%s", shown_model(e));

    occupancy_sensor::config_t sensor;
    sensor.occupancy_sensing.feature_flags = s_occupancy_feature;

    endpoint::power_source::config_t power;
    power.power_source.status = chip::to_underlying(PowerSource::PowerSourceStatusEnum::kActive);
    power.power_source.order = 0;
    snprintf(power.power_source.description, sizeof(power.power_source.description), "%s", kPowerSourceDescription);
    power.power_source.feature_flags = cluster::power_source::feature::battery::get_id();
    power.power_source.features.battery.bat_charge_level = e.battery_level;
    power.power_source.features.battery.bat_replacement_needed =
        e.battery_level == chip::to_underlying(PowerSource::BatChargeLevelEnum::kCritical);
    power.power_source.features.battery.bat_replaceability =
        chip::to_underlying(PowerSource::BatReplaceabilityEnum::kUserReplaceable);

    namespace info_attr = cluster::bridged_device_basic_information::attribute;
    cluster_t *info = cluster::get(ep, BridgedDeviceBasicInformation::Id);
    bool ok = info != nullptr && info_attr::create_node_label(info, mac, strlen(mac)) != nullptr &&
              info_attr::create_serial_number(info, mac, strlen(mac)) != nullptr &&
              info_attr::create_vendor_name(info, vendor, strlen(vendor)) != nullptr &&
              info_attr::create_product_name(info, model, strlen(model)) != nullptr &&
              occupancy_sensor::add(ep, &sensor) == ESP_OK && cluster::get(ep, Identify::Id) != nullptr &&
              cluster::get(ep, OccupancySensing::Id) != nullptr && endpoint::power_source::add(ep, &power) == ESP_OK;
    cluster_t *battery = ok ? cluster::get(ep, PowerSource::Id) : nullptr;
    ok = ok && battery != nullptr &&
         cluster::power_source::attribute::create_bat_percent_remaining(battery, percent_value(e.battery_pct),
                                                                        nullable<uint8_t>(0),
                                                                        nullable<uint8_t>(200)) != nullptr &&
         set_parent_endpoint(ep, endpoint::get(s_node, s_aggregator)) == ESP_OK;
    if (!ok) {
        endpoint::destroy(s_node, ep);
        return nullptr;
    }
    endpoint::enable(ep); // after esp_matter::start(): runs the cluster init callbacks, reports the new PartsList
    return ep;
}

// The slot needs no device built any more (it has one, or none is wanted): nothing to wait for or retry
static void clear_wait(int slot)
{
    s_waiting[slot] = false;
    s_error[slot] = false;
    s_retry_ms[slot] = 0;
    s_retry_delay_ms[slot] = 0;
    s_failures[slot] = 0;
}

// Builds the device of the slot's entry; true if it now exists. Logs what it cost.
static bool build_entry(int slot, uint16_t resume_id)
{
    const size_t before = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    Entry &e = s_entries[slot];
    endpoint_t *ep = build_device(slot, e, resume_id);
    if (ep == nullptr) {
        return false;
    }
    e.endpoint = endpoint::get_id(ep);
    s_built[slot] = true;
    clear_wait(slot);
    refresh_endpoint_lists();
    const size_t after = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    s_last_cost = before > after ? before - after : 0;
    s_max_cost = s_last_cost > s_max_cost ? s_last_cost : s_max_cost;
    char mac[PLINK_MAC_STR_SIZE];
    plink_mac_string(e.mac, mac);
    ESP_LOGI(TAG, "Presence %d: device %s \"%s\" %s on endpoint %u (%u bytes of heap, %u left)", slot + 1, mac,
             shown_model(e), resume_id == chip::kInvalidEndpointId ? "added" : "restored", e.endpoint,
             static_cast<unsigned>(s_last_cost), static_cast<unsigned>(after));
    return true;
}

static void remove_device(int slot)
{
    Entry &e = s_entries[slot];
    char mac[PLINK_MAC_STR_SIZE];
    plink_mac_string(e.mac, mac);
    endpoint_t *ep = s_built[slot] ? endpoint::get(s_node, e.endpoint) : nullptr;
    if (ep != nullptr && endpoint::destroy(s_node, ep) != ESP_OK) {
        ESP_LOGE(TAG, "Presence %d: endpoint %u could not be removed", slot + 1, e.endpoint);
    } else {
        ESP_LOGI(TAG, "Presence %d: device %s removed (endpoint %u)", slot + 1, mac, e.endpoint);
    }
    memset(&e, 0, sizeof(e));
    s_built[slot] = false;
    refresh_endpoint_lists();
}

// Memory for one more device: free heap above the reserve of the phase, and a large enough block
static bool memory_allows(int slot)
{
    const bool commissioned = s_commissioned.load();
    const size_t reserve =
        static_cast<size_t>(commissioned ? CONFIG_PRESENCE_BRIDGE_HEAP_RESERVE_KB
                                         : CONFIG_PRESENCE_BRIDGE_COMMISSIONING_RESERVE_KB) * 1024;
    const size_t free_now = heap_caps_get_free_size(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL);
    if (free_now >= reserve + kDeviceCost && largest >= kLargestBlockMin) {
        return true;
    }
    if (!s_waiting[slot] || s_error[slot]) {
        ESP_LOGW(TAG, "Presence %d: device waits (%s: %u bytes free, %u kept in reserve)", slot + 1,
                 commissioned ? "not enough memory" : "memory kept for commissioning, added once commissioned",
                 static_cast<unsigned>(free_now), static_cast<unsigned>(reserve));
    }
    s_waiting[slot] = true;
    s_error[slot] = false;
    return false;
}

// What the scanner sends replaces what the table has. An unknown battery keeps the known one; an empty model keeps
// the advertised name known (the scanner only hears names in its NORMAL mode) but clears a model that was typed on
// the page and is no longer. Returns true if the entry changed.
static bool merge(Entry &e, const plink_slot_t &want)
{
    bool changed = false;
    const bool typed = (e.flags & kEntryModelConfigured) != 0;
    if (want.model[0] != '\0' || typed) {
        if (strcmp(e.model, want.model) != 0 || typed != want.model_configured) {
            memcpy(e.model, want.model, sizeof(e.model));
            e.flags = want.model_configured ? (e.flags | kEntryModelConfigured) : (e.flags & ~kEntryModelConfigured);
            changed = true;
        }
    }
    if (want.battery_pct != PLINK_BATTERY_UNKNOWN &&
        (e.battery_pct != want.battery_pct || e.battery_level != want.battery_level)) {
        e.battery_pct = want.battery_pct;
        e.battery_level = want.battery_level;
        changed = true;
    }
    return changed;
}

// The model and battery of a device that stays: attributes updated in place
static void update_device(int slot, const plink_slot_t &want)
{
    Entry &e = s_entries[slot];
    const Entry before = e;
    if (!merge(e, want)) {
        return;
    }
    if (strcmp(before.model, e.model) != 0) {
        char model[PLINK_MODEL_MAX + 1];
        snprintf(model, sizeof(model), "%s", shown_model(e));
        esp_matter_attr_val_t val = esp_matter_char_str(model, static_cast<uint16_t>(strlen(model)));
        attribute::update(e.endpoint, BridgedDeviceBasicInformation::Id,
                          BridgedDeviceBasicInformation::Attributes::ProductName::Id, &val);
        ESP_LOGI(TAG, "Presence %d: model \"%s\"", slot + 1, model);
    }
    if (before.battery_pct != e.battery_pct || before.battery_level != e.battery_level) {
        esp_matter_attr_val_t val = esp_matter_nullable_uint8(percent_value(e.battery_pct));
        attribute::update(e.endpoint, PowerSource::Id, PowerSource::Attributes::BatPercentRemaining::Id, &val);
        val = esp_matter_enum8(e.battery_level);
        attribute::update(e.endpoint, PowerSource::Id, PowerSource::Attributes::BatChargeLevel::Id, &val);
        val = esp_matter_bool(e.battery_level == chip::to_underlying(PowerSource::BatChargeLevelEnum::kCritical));
        attribute::update(e.endpoint, PowerSource::Id, PowerSource::Attributes::BatReplacementNeeded::Id, &val);
        ESP_LOGI(TAG, "Presence %d: battery %u%%%s", slot + 1, e.battery_pct,
                 e.battery_level == chip::to_underlying(PowerSource::BatChargeLevelEnum::kCritical) ? ", replace it"
                 : e.battery_level == chip::to_underlying(PowerSource::BatChargeLevelEnum::kWarning) ? ", low"
                                                                                                     : "");
    }
    save_table();
}

// The slot's device could not be built: waits, tried again after a delay doubled at each failure
static void retry_later(int slot)
{
    const int64_t delay = s_retry_delay_ms[slot] == 0 ? kErrorRetryMs : s_retry_delay_ms[slot] * 2;
    s_retry_delay_ms[slot] = delay < kErrorRetryMaxMs ? delay : kErrorRetryMaxMs;
    s_retry_ms[slot] = now_ms() + s_retry_delay_ms[slot];
    s_waiting[slot] = true;
    s_error[slot] = true;
}

// A saved device not built yet (memory at boot): built now if memory allows. want: the scanner's wish, if known.
// Returns false while it has to wait.
static bool restore(int slot, const plink_slot_t *want, bool *added)
{
    Entry &e = s_entries[slot];
    char mac[PLINK_MAC_STR_SIZE];
    plink_mac_string(e.mac, mac);
    if (want != nullptr && (!want->active || memcmp(want->mac, e.mac, PLINK_MAC_LEN) != 0)) {
        // The slot changed while this chip was off: the saved device is not brought back
        ESP_LOGI(TAG, "Presence %d: saved device %s (endpoint %u) no longer in the scanner's list", slot + 1, mac,
                 e.endpoint);
        memset(&e, 0, sizeof(e));
        clear_wait(slot);
        save_table();
        return true;
    }
    if (now_ms() < s_retry_ms[slot] || !memory_allows(slot)) {
        return false;
    }
    const bool merged = want != nullptr && merge(e, *want);
    if (build_entry(slot, e.endpoint)) {
        *added = true;
        if (merged) {
            save_table();
        }
        return true;
    }
    if (++s_failures[slot] < kRestoreAttempts) {
        // Memory short of the estimate: the endpoint ID is kept (the accessory stays the same in the controllers)
        ESP_LOGW(TAG, "Presence %d: device %s on endpoint %u not restored, tried again later", slot + 1, mac,
                 e.endpoint);
        retry_later(slot);
        if (merged) {
            save_table(); // the new model or battery is kept for the next attempt
        }
        return false;
    }
    // The endpoint cannot come back (e.g. the Matter data was erased): the scanner's list adds the device again on a
    // new endpoint
    ESP_LOGE(TAG, "Presence %d: device %s on endpoint %u cannot be restored", slot + 1, mac, e.endpoint);
    memset(&e, 0, sizeof(e));
    clear_wait(slot);
    save_table();
    return true;
}

esp_err_t matter_bridge_create_aggregator(node_t *node, uint32_t occupancy_feature)
{
    aggregator::config_t config;
    endpoint_t *ep = aggregator::create(node, &config, ENDPOINT_FLAG_NONE, nullptr);
    if (ep == nullptr) {
        ESP_LOGE(TAG, "Cannot create the Aggregator endpoint");
        return ESP_FAIL;
    }
    s_node = node;
    s_aggregator = endpoint::get_id(ep);
    s_occupancy_feature = occupancy_feature;
    ESP_LOGI(TAG, "Matter bridge \"%s\": Aggregator on endpoint %u, up to %d bridged devices", CONFIG_PRESENCE_PRODUCT_NAME,
             s_aggregator, PLINK_SLOT_COUNT);
    return ESP_OK;
}

void matter_bridge_load(void)
{
    if (load_table()) {
        int restored = 0;
        int waiting = 0;
        for (int i = 0; i < PLINK_SLOT_COUNT; i++) {
            if (s_entries[i].endpoint == 0) {
                continue;
            }
            bool added = false;
            if (restore(i, nullptr, &added) && added) {
                restored++;
            } else if (s_entries[i].endpoint != 0 && !s_built[i]) {
                waiting++;
            }
        }
        ESP_LOGI(TAG, "%d bridged device(s) restored, %d waiting for memory", restored, waiting);
    }
    s_ready = true;
}

bool matter_bridge_step(int slot, const plink_slot_t *want, bool *added)
{
    *added = false;
    if (!s_ready || slot < 0 || slot >= PLINK_SLOT_COUNT) {
        return false;
    }
    Entry &e = s_entries[slot];
    if (e.endpoint != 0 && !s_built[slot] && !restore(slot, want, added)) {
        return false;
    }
    if (want == nullptr) {
        return true; // nothing known from the scanner yet: restoring was all there was to do
    }
    const bool have = e.endpoint != 0 && s_built[slot];
    switch (plink_slot_action(have, e.mac, want->active, want->mac)) {
    case PLINK_SLOT_KEEP:
        if (have) {
            update_device(slot, *want);
        }
        clear_wait(slot);
        return true;
    case PLINK_SLOT_REMOVE:
        remove_device(slot);
        save_table();
        clear_wait(slot);
        return true;
    case PLINK_SLOT_REPLACE:
        remove_device(slot);
        save_table();
        [[fallthrough]];
    case PLINK_SLOT_ADD:
        break;
    }
    if (now_ms() < s_retry_ms[slot] || !memory_allows(slot)) {
        return false; // after a failure, and while memory is short
    }
    Entry fresh = {};
    memcpy(fresh.mac, want->mac, PLINK_MAC_LEN);
    fresh.battery_pct = PLINK_BATTERY_UNKNOWN;
    merge(fresh, *want);
    e = fresh;
    if (!build_entry(slot, chip::kInvalidEndpointId)) {
        memset(&e, 0, sizeof(e));
        retry_later(slot); // each attempt uses up an endpoint ID
        char mac[PLINK_MAC_STR_SIZE];
        plink_mac_string(want->mac, mac);
        ESP_LOGE(TAG, "Presence %d: device %s could not be added (endpoints or memory), tried again in %d s", slot + 1,
                 mac, static_cast<int>(s_retry_delay_ms[slot] / 1000));
        return false;
    }
    save_table();
    *added = true;
    return true;
}

bool matter_bridge_ready(void)
{
    return s_ready;
}

uint16_t matter_bridge_endpoint(int slot)
{
    return slot >= 0 && slot < PLINK_SLOT_COUNT && s_built[slot] ? s_entries[slot].endpoint : 0;
}

void matter_bridge_get_status(plink_status_t *status)
{
    status->bridged_mask = 0;
    status->waiting_mask = 0;
    status->error_mask = 0;
    for (int i = 0; i < PLINK_SLOT_COUNT; i++) {
        status->endpoint[i] = matter_bridge_endpoint(i);
        if (status->endpoint[i] != 0) {
            status->bridged_mask |= 1u << i;
        } else if (s_waiting[i] || (s_entries[i].endpoint != 0 && !s_built[i])) {
            status->waiting_mask |= 1u << i;
            status->error_mask |= s_error[i] ? 1u << i : 0;
        }
    }
}

void matter_bridge_set_reachable(bool reachable)
{
    s_reachable = reachable;
    for (int i = 0; i < PLINK_SLOT_COUNT; i++) {
        if (!s_built[i]) {
            continue;
        }
        esp_matter_attr_val_t val = esp_matter_bool(reachable);
        attribute::update(s_entries[i].endpoint, BridgedDeviceBasicInformation::Id,
                          BridgedDeviceBasicInformation::Attributes::Reachable::Id, &val);
    }
}

void matter_bridge_set_commissioned(bool commissioned)
{
    s_commissioned = commissioned;
}

void matter_bridge_erase(void)
{
    // No step from now on (the restart follows): one would add a device and save a table again
    s_ready = false;
    nvs_handle_t h;
    if (nvs_open(kNamespace, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    memset(s_entries, 0, sizeof(s_entries));
    memset(s_built, 0, sizeof(s_built));
    memset(s_waiting, 0, sizeof(s_waiting));
    memset(s_error, 0, sizeof(s_error));
}

void matter_bridge_log_diagnostics(void)
{
    int built = 0;
    int waiting = 0;
    int errors = 0;
    for (int i = 0; i < PLINK_SLOT_COUNT; i++) {
        built += s_built[i] ? 1 : 0;
        const bool w = !s_built[i] && (s_waiting[i] || s_entries[i].endpoint != 0);
        waiting += w ? 1 : 0;
        errors += w && s_error[i] ? 1 : 0;
    }
    ESP_LOGI(TAG, "Bridge: %d device(s), %d waiting (%d after an error); heap of the last device %u bytes, largest %u",
             built, waiting, errors, static_cast<unsigned>(s_last_cost), static_cast<unsigned>(s_max_cost));
}
