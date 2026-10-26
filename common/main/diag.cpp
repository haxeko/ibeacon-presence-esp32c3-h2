/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "diag.h"

#include <stdio.h>

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "sdkconfig.h"

#include "ble_scanner.h"
#include "matter_device.h"
#if CONFIG_PRESENCE_ROLE_SCANNER
#include "presence_link.h"
#endif
#include "mac_watch.h"
#include "presence_manager.h"
#include "scan_cache.h"
#include "wifi_config.h"

static const char *TAG = "diag";

static app_mode_t s_mode;

static void log_scanner(uint32_t *last_reports, int64_t *last_ms)
{
    ble_scanner_stats_t st;
    ble_scanner_get_stats(&st);
    const int64_t now = esp_timer_get_time() / 1000;
    const int64_t dt = now - *last_ms;
    const unsigned rate = dt > 0 ? static_cast<unsigned>((st.adv_reports - *last_reports) * 1000ULL / dt) : 0;
    *last_reports = st.adv_reports;
    *last_ms = now;
    char why[96];
    ESP_LOGI(TAG,
             "Scanner %s%s%s, %u adv/s, %u reports, %u iBeacon, %u TLM, scanned %u s, starts %u, start failures %u "
             "(last %d), unexpected stops %u, watchdog restarts %u",
             ble_scanner_state_name(st.state), st.pause_mask ? " by " : "",
             ble_scanner_pause_text(st.pause_mask, why, sizeof(why)), rate, static_cast<unsigned>(st.adv_reports),
             static_cast<unsigned>(st.ibeacon_reports), static_cast<unsigned>(st.tlm_reports),
             static_cast<unsigned>(st.scan_clock_ms / 1000),
             static_cast<unsigned>(st.starts), static_cast<unsigned>(st.start_failures), st.last_error,
             static_cast<unsigned>(st.unexpected_stops), static_cast<unsigned>(st.watchdog_restarts));
}

static void log_presence()
{
    static presence_manager_stats_t st; // diag task only: 20 slots kept off its stack
    presence_manager_get_stats(&st);
    for (int i = 0; i < PRESENCE_SLOT_COUNT; i++) {
        const presence_slot_stats_t &s = st.slots[i];
        if (!s.active) {
            continue;
        }
        char battery[72] = "no TLM frame";
        char watched[24];
        snprintf(watched, sizeof(watched), " %u/%u min", static_cast<unsigned>(s.watch_s / 60),
                 static_cast<unsigned>(MAC_WATCH_TIME_MS / 60000));
        if (s.tlm > 0) {
            snprintf(battery, sizeof(battery), "battery %u%% (%u mV, %u s ago, %u TLM)", s.battery_pct, s.battery_mv,
                     static_cast<unsigned>(s.battery_age_s), static_cast<unsigned>(s.tlm));
        }
        ESP_LOGI(TAG,
                 "Presence %d: %s%s, RSSI %d dBm, %u s ago, matched %u, accepted %u, RSSI-rejected %u, %s, MAC %s%s",
                 i + 1, s.present ? "ON" : "OFF", s.reported ? "" : " (not reported yet)", s.last_rssi,
                 static_cast<unsigned>(s.age_s), static_cast<unsigned>(s.matched), static_cast<unsigned>(s.accepted),
                 static_cast<unsigned>(s.rssi_rejected), battery, mac_watch_state_name(s.watch_state),
                 s.watch_state == MAC_WATCH_RUNNING ? watched : "");
    }
    ESP_LOGI(TAG, "Presence queue drops %u, Matter report retries %u", static_cast<unsigned>(st.queue_drops),
             static_cast<unsigned>(st.report_retries));
}

static void diag_task(void *)
{
    uint32_t last_reports = 0;
    int64_t last_ms = esp_timer_get_time() / 1000;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(CONFIG_PRESENCE_DIAG_PERIOD_SEC * 1000));
        ESP_LOGI(TAG, "Uptime %lld s, heap free %u, min %u, largest %u", esp_timer_get_time() / 1000000,
                 static_cast<unsigned>(esp_get_free_heap_size()), static_cast<unsigned>(esp_get_minimum_free_heap_size()),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_8BIT)));
        log_scanner(&last_reports, &last_ms);
        if (s_mode == APP_MODE_NORMAL) {
            log_presence();
#if CONFIG_PRESENCE_ROLE_SCANNER
            presence_link_log_diagnostics();
#else
            matter_device_log_diagnostics();
#endif
        } else {
            scan_cache_stats_t cache;
            scan_cache_get_stats(&cache);
            ESP_LOGI(TAG, "Beacon list %u/%u (queue drops %u, evictions %u), SoftAP stations %d, back to NORMAL in %u s",
                     static_cast<unsigned>(cache.count), static_cast<unsigned>(cache.capacity),
                     static_cast<unsigned>(cache.queue_drops), static_cast<unsigned>(cache.evictions),
                     wifi_config_station_count(), static_cast<unsigned>(app_mode_idle_remaining_s()));
            wifi_config_log_diagnostics();
#if CONFIG_PRESENCE_ROLE_SCANNER
            presence_link_log_diagnostics();
#endif
        }
    }
}

esp_err_t diag_start(app_mode_t mode)
{
    if (CONFIG_PRESENCE_DIAG_PERIOD_SEC == 0) {
        return ESP_OK;
    }
    s_mode = mode;
    return xTaskCreate(diag_task, "diag", 4096, nullptr, 2, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}
