/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "scan_cache.h"

#include <stdlib.h>
#include <string.h>

#include <atomic>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "sdkconfig.h"

static const char *TAG = "scan_cache";

static constexpr int kQueueLength = 32;

static QueueHandle_t s_queue;
static SemaphoreHandle_t s_lock;
// Guarded by s_lock
static scan_entry_t *s_entries;
static size_t s_count;
static uint32_t s_evictions;

static std::atomic<uint32_t> s_queue_drops{ 0 };

// NimBLE host task only: addresses of the iBeacons heard lately; the one heard least recently is replaced first, so
// a beacon that keeps advertising stays known (its TLM and name frames are kept) even with many beacons around
namespace {
struct KnownAddr {
    uint8_t addr[BLE_ADDR_LEN];
    uint8_t addr_type;
    bool used;
    uint32_t heard; // s_known_clock at its last iBeacon frame
};
} // namespace
static KnownAddr s_known[CONFIG_PRESENCE_SCAN_CACHE_SIZE];
static uint32_t s_known_clock;

static int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

static KnownAddr *find_known(const uint8_t addr[BLE_ADDR_LEN], uint8_t addr_type)
{
    for (KnownAddr &k : s_known) {
        if (k.used && k.addr_type == addr_type && memcmp(k.addr, addr, BLE_ADDR_LEN) == 0) {
            return &k;
        }
    }
    return nullptr;
}

// An iBeacon frame: the address is known from now on
static void remember(const uint8_t addr[BLE_ADDR_LEN], uint8_t addr_type)
{
    const uint32_t clock = ++s_known_clock;
    KnownAddr *k = find_known(addr, addr_type);
    if (k == nullptr) {
        k = &s_known[0];
        for (KnownAddr &c : s_known) {
            if (!c.used) {
                k = &c;
                break;
            }
            if (clock - c.heard > clock - k->heard) { // wrap-safe: the longest since heard
                k = &c;
            }
        }
        memcpy(k->addr, addr, BLE_ADDR_LEN);
        k->addr_type = addr_type;
        k->used = true;
    }
    k->heard = clock;
}

void scan_cache_sink(const ble_adv_report_t *report)
{
    if (report->has_ibeacon) {
        remember(report->addr, report->addr_type);
    } else if (find_known(report->addr, report->addr_type) == nullptr) {
        return; // a name or TLM frame of a device that sends no iBeacon: not listed
    }
    if (xQueueSend(s_queue, report, 0) != pdTRUE) {
        s_queue_drops.fetch_add(1, std::memory_order_relaxed);
    }
}

static scan_entry_t *find(const uint8_t addr[BLE_ADDR_LEN], uint8_t addr_type)
{
    for (size_t i = 0; i < s_count; i++) {
        if (s_entries[i].addr_type == addr_type && memcmp(s_entries[i].addr, addr, BLE_ADDR_LEN) == 0) {
            return &s_entries[i];
        }
    }
    return nullptr;
}

static void remove_at(size_t i)
{
    s_entries[i] = s_entries[s_count - 1];
    s_count--;
}

static void expire(int64_t now)
{
    const int64_t max_age = static_cast<int64_t>(CONFIG_PRESENCE_SCAN_CACHE_EXPIRE_SEC) * 1000;
    for (size_t i = 0; i < s_count;) {
        if (now - s_entries[i].last_seen_ms > max_age) {
            remove_at(i);
        } else {
            i++;
        }
    }
}

static void merge(const ble_adv_report_t &r)
{
    scan_entry_t *e = find(r.addr, r.addr_type);
    if (!r.has_ibeacon) {
        // TLM frame or name (scan response or another advertisement of the beacon): only for a beacon in the list
        if (e == nullptr) {
            return;
        }
        const uint16_t mv = r.has_tlm ? eddystone_tlm_battery_mv(&r.tlm, &e->tlm_order) : 0;
        if (mv != 0) {
            e->battery_mv = mv;
            e->battery_ms = r.time_ms;
        }
        if (r.name[0] != '\0') {
            memcpy(e->name, r.name, sizeof(e->name));
        }
        return;
    }
    if (e == nullptr) {
        if (s_count < CONFIG_PRESENCE_SCAN_CACHE_SIZE) {
            e = &s_entries[s_count++];
        } else {
            size_t oldest = 0;
            for (size_t i = 1; i < s_count; i++) {
                if (s_entries[i].last_seen_ms < s_entries[oldest].last_seen_ms) {
                    oldest = i;
                }
            }
            e = &s_entries[oldest];
            s_evictions++;
        }
        memset(e, 0, sizeof(*e));
        memcpy(e->addr, r.addr, BLE_ADDR_LEN);
        e->addr_type = r.addr_type;
        e->first_seen_ms = r.time_ms;
    }
    e->rssi = r.rssi;
    e->ibeacon = r.ibeacon;
    if (r.name[0] != '\0') {
        memcpy(e->name, r.name, sizeof(e->name));
    }
    e->last_seen_ms = r.time_ms;
    e->count++;
}

static void cache_task(void *)
{
    ble_adv_report_t report;
    for (;;) {
        const bool got = xQueueReceive(s_queue, &report, pdMS_TO_TICKS(1000)) == pdTRUE;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        if (got) {
            do {
                merge(report);
            } while (xQueueReceive(s_queue, &report, 0) == pdTRUE);
        }
        expire(now_ms());
        xSemaphoreGive(s_lock);
    }
}

esp_err_t scan_cache_start(void)
{
    s_entries = static_cast<scan_entry_t *>(calloc(CONFIG_PRESENCE_SCAN_CACHE_SIZE, sizeof(scan_entry_t)));
    s_queue = xQueueCreate(kQueueLength, sizeof(ble_adv_report_t));
    s_lock = xSemaphoreCreateMutex();
    if (s_entries == nullptr || s_queue == nullptr || s_lock == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Beacon list: up to %d entries, %u bytes", CONFIG_PRESENCE_SCAN_CACHE_SIZE,
             static_cast<unsigned>(CONFIG_PRESENCE_SCAN_CACHE_SIZE * sizeof(scan_entry_t)));
    return xTaskCreate(cache_task, "scan_cache", 3072, nullptr, 4, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

size_t scan_cache_snapshot(scan_entry_t *out, size_t max, int64_t *now)
{
    *now = now_ms();
    xSemaphoreTake(s_lock, portMAX_DELAY);
    expire(*now);
    const size_t n = s_count < max ? s_count : max;
    memcpy(out, s_entries, n * sizeof(scan_entry_t));
    xSemaphoreGive(s_lock);
    // Strongest first (insertion sort: at most CONFIG_PRESENCE_SCAN_CACHE_SIZE entries)
    for (size_t i = 1; i < n; i++) {
        const scan_entry_t tmp = out[i];
        size_t j = i;
        while (j > 0 && out[j - 1].rssi < tmp.rssi) {
            out[j] = out[j - 1];
            j--;
        }
        out[j] = tmp;
    }
    return n;
}

void scan_cache_clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count = 0;
    xSemaphoreGive(s_lock);
}

void scan_cache_get_stats(scan_cache_stats_t *out)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->count = static_cast<uint32_t>(s_count);
    out->evictions = s_evictions;
    xSemaphoreGive(s_lock);
    out->capacity = CONFIG_PRESENCE_SCAN_CACHE_SIZE;
    out->queue_drops = s_queue_drops.load();
}
