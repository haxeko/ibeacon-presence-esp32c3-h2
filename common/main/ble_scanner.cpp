/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "ble_scanner.h"

#include <stdio.h>
#include <string.h>

#include <atomic>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#include "sdkconfig.h"

static const char *TAG = "ble_scanner";

static constexpr uint32_t kBackoffMinMs = 1000;
static constexpr uint32_t kBackoffMaxMs = 30000;
static constexpr TickType_t kPeriod = pdMS_TO_TICKS(500);

// Written once by ble_scanner_start() before the task and the host start, read-only afterwards
static ble_scanner_config_t s_cfg;
static TaskHandle_t s_task;

static std::atomic<uint32_t> s_pause{ 0 };
static std::atomic<int64_t> s_hold_until_ms{ 0 };
static std::atomic<bool> s_disc_complete{ false }; // set by the GAP callback when the stack ended the discovery
static std::atomic<int64_t> s_disc_complete_ms{ 0 };
static std::atomic<int64_t> s_last_adv_ms{ 0 };

static std::atomic<uint32_t> s_adv_reports{ 0 };
static std::atomic<uint32_t> s_ibeacon_reports{ 0 };
static std::atomic<uint32_t> s_tlm_reports{ 0 };
static std::atomic<uint32_t> s_starts{ 0 };
static std::atomic<uint32_t> s_start_failures{ 0 };
static std::atomic<uint32_t> s_unexpected_stops{ 0 };
static std::atomic<uint32_t> s_watchdog_restarts{ 0 };
static std::atomic<int> s_last_error{ 0 };
static std::atomic<int> s_state{ BLE_SCANNER_STOPPED };

// Scan clock, written by the scan task only
static portMUX_TYPE s_clock_mux = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_clock_accum_ms;
static int64_t s_run_start_ms;
static bool s_clock_running;

static int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

static void notify_task()
{
    if (s_task != nullptr) {
        xTaskNotifyGive(s_task);
    }
}

static void clock_start(int64_t at)
{
    portENTER_CRITICAL(&s_clock_mux);
    s_clock_running = true;
    s_run_start_ms = at;
    portEXIT_CRITICAL(&s_clock_mux);
}

static void clock_stop(int64_t at)
{
    portENTER_CRITICAL(&s_clock_mux);
    if (s_clock_running) {
        if (at > s_run_start_ms) {
            s_clock_accum_ms += static_cast<uint64_t>(at - s_run_start_ms);
        }
        s_clock_running = false;
    }
    portEXIT_CRITICAL(&s_clock_mux);
}

uint64_t ble_scanner_scan_clock_ms(void)
{
    const int64_t now = now_ms();
    portENTER_CRITICAL(&s_clock_mux);
    uint64_t v = s_clock_accum_ms;
    if (s_clock_running && now > s_run_start_ms) {
        v += static_cast<uint64_t>(now - s_run_start_ms);
    }
    portEXIT_CRITICAL(&s_clock_mux);
    return v;
}

// ---------------------------------------------------------------------------------------------------------------------
// NimBLE host task context

static void handle_report(bool scan_response, const ble_addr_t &addr, int8_t rssi, const uint8_t *data, uint8_t len)
{
    const int64_t now = now_ms();
    s_adv_reports.fetch_add(1, std::memory_order_relaxed);
    s_last_adv_ms.store(now, std::memory_order_relaxed);

    ble_adv_report_t report;
    ble_addr_from_le(addr.val, report.addr);
    report.addr_type = addr.type;
    if (s_cfg.filter != nullptr && !s_cfg.filter(report.addr, report.addr_type)) {
        return;
    }
    report.rssi = rssi;
    report.scan_response = scan_response;
    report.time_ms = now;
    report.has_ibeacon = ibeacon_parse(data, len, &report.ibeacon);
    report.has_tlm = !report.has_ibeacon && eddystone_tlm_parse(data, len, &report.tlm);
    report.name[0] = '\0';
    if (s_cfg.parse_names) {
        adv_parse_name(data, len, report.name, sizeof(report.name));
    }
    if (report.has_ibeacon) {
        s_ibeacon_reports.fetch_add(1, std::memory_order_relaxed);
    } else if (report.has_tlm) {
        s_tlm_reports.fetch_add(1, std::memory_order_relaxed);
    } else if (report.name[0] == '\0') {
        return; // nothing a sink uses
    }
    s_cfg.sink(&report);
}

static int gap_event(struct ble_gap_event *event, void *)
{
    switch (event->type) {
    case BLE_GAP_EVENT_DISC:
        handle_report(event->disc.event_type == BLE_HCI_ADV_RPT_EVTYPE_SCAN_RSP, event->disc.addr, event->disc.rssi,
                      event->disc.data, event->disc.length_data);
        return 0;
#if CONFIG_BT_NIMBLE_EXT_ADV
    case BLE_GAP_EVENT_EXT_DISC:
        // Legacy advertisements arrive here too when extended advertising is compiled in
        if (event->ext_disc.data_status == BLE_GAP_EXT_ADV_DATA_STATUS_COMPLETE) {
            handle_report((event->ext_disc.props & BLE_HCI_ADV_SCAN_RSP_MASK) != 0, event->ext_disc.addr,
                          event->ext_disc.rssi, event->ext_disc.data, event->ext_disc.length_data);
        }
        return 0;
#endif
    case BLE_GAP_EVENT_DISC_COMPLETE:
        // ble_gap_disc_cancel() does not report this event: here the stack ended the discovery (host reset,
        // controller error), reason in event->disc_complete.reason.
        s_disc_complete_ms.store(now_ms());
        s_disc_complete.store(true);
        notify_task();
        return 0;
    default:
        return 0;
    }
}

// CONFIG mode only: this module owns the host
static void host_task(void *)
{
    nimble_port_run(); // returns when nimble_port_stop() is called
    nimble_port_freertos_deinit();
}

static void on_host_sync(void)
{
    ESP_LOGI(TAG, "BLE host synced");
    notify_task();
}

static void on_host_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset, reason %d", reason);
    notify_task();
}

// ---------------------------------------------------------------------------------------------------------------------
// Scan task

// HCI scan interval and window are in units of 0.625 ms, 0x0004-0x4000
static uint16_t ms_to_units(uint32_t ms)
{
    uint32_t units = (ms * 1000u + 312u) / 625u;
    if (units < 0x0004) {
        units = 0x0004;
    }
    if (units > 0x4000) {
        units = 0x4000;
    }
    return static_cast<uint16_t>(units);
}

static int start_discovery()
{
    uint8_t own_addr_type = BLE_OWN_ADDR_PUBLIC;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        own_addr_type = BLE_OWN_ADDR_PUBLIC; // only used by scan requests (active scanning)
    }
    struct ble_gap_disc_params params = {};
    params.itvl = ms_to_units(s_cfg.interval_ms);
    params.window = ms_to_units(s_cfg.window_ms);
    params.filter_policy = 0; // no accept list
    params.limited = 0;
    params.passive = s_cfg.active ? 0 : 1;
    // Every advertisement, identical repeats included: a beacon that keeps advertising keeps its slot on
    params.filter_duplicates = 0;
    // Observer mode: all reports, whatever their Flags (some beacons advertise without discoverable flags)
    params.disable_observer_mode = 0;
    return ble_gap_disc(own_addr_type, BLE_HS_FOREVER, &params, gap_event, nullptr);
}

static bool s_scanning; // scan task only

static void stop_discovery(int64_t now)
{
    const int rc = ble_gap_disc_cancel();
    if (rc != 0 && rc != BLE_HS_EALREADY && rc != BLE_HS_EDISABLED) {
        ESP_LOGW(TAG, "ble_gap_disc_cancel() failed: %d", rc);
    }
    s_scanning = false;
    clock_stop(now);
    // A completion that raced with the cancel is not an unexpected stop
    s_disc_complete.store(false);
}

static uint32_t next_backoff(uint32_t *backoff_ms)
{
    *backoff_ms = *backoff_ms == 0 ? kBackoffMinMs : (*backoff_ms * 2 > kBackoffMaxMs ? kBackoffMaxMs : *backoff_ms * 2);
    return *backoff_ms;
}

static void scan_task(void *)
{
    uint32_t backoff_ms = 0;
    int64_t retry_at_ms = 0;
    bool was_synced = false;

    for (;;) {
        ulTaskNotifyTake(pdTRUE, kPeriod);
        const int64_t now = now_ms();

        if (s_disc_complete.exchange(false) && s_scanning) {
            s_scanning = false;
            clock_stop(s_disc_complete_ms.load());
            s_unexpected_stops.fetch_add(1);
            retry_at_ms = now + next_backoff(&backoff_ms);
            ESP_LOGW(TAG, "Scan ended by the BLE stack: restarting in %u ms", static_cast<unsigned>(backoff_ms));
        }

        const bool synced = ble_hs_synced();
        if (synced != was_synced) {
            ESP_LOGI(TAG, "BLE host %s", synced ? "ready" : "not synced");
            was_synced = synced;
        }
        const uint32_t pause = s_pause.load();
        const bool held = now < s_hold_until_ms.load();
        const bool want = synced && pause == 0 && !held;

        if (s_scanning && !want) {
            stop_discovery(now);
            char why[96];
            ESP_LOGI(TAG, "Scanning paused (%s)",
                     !synced ? "host not synced" : (pause ? ble_scanner_pause_text(pause, why, sizeof(why)) : "hold-off"));
        } else if (!s_scanning && want && now >= retry_at_ms) {
            const int rc = start_discovery();
            if (rc == 0) {
                s_scanning = true;
                clock_start(now);
                s_last_adv_ms.store(now);
                s_starts.fetch_add(1);
                backoff_ms = 0;
                ESP_LOGI(TAG, "Scanning %s, interval %u ms, window %u ms", s_cfg.active ? "actively" : "passively",
                         s_cfg.interval_ms, s_cfg.window_ms);
            } else {
                s_start_failures.fetch_add(1);
                s_last_error.store(rc);
                if (rc == BLE_HS_EALREADY) {
                    ble_gap_disc_cancel(); // a discovery we lost track of: cancel it, start ours next time
                }
                retry_at_ms = now + next_backoff(&backoff_ms);
                ESP_LOGW(TAG, "ble_gap_disc() failed: %d, retry in %u ms", rc, static_cast<unsigned>(backoff_ms));
            }
        }

        if (s_scanning && s_cfg.watchdog_s > 0 &&
            now - s_last_adv_ms.load() > static_cast<int64_t>(s_cfg.watchdog_s) * 1000) {
            ESP_LOGW(TAG, "No advertisement at all for %u s: restarting the scan",
                     static_cast<unsigned>(s_cfg.watchdog_s));
            stop_discovery(now);
            s_watchdog_restarts.fetch_add(1);
            retry_at_ms = now + kBackoffMinMs;
        }

        ble_scanner_state_t state;
        if (s_scanning) {
            state = BLE_SCANNER_RUNNING;
        } else if (!synced) {
            state = BLE_SCANNER_WAIT_HOST;
        } else if (pause != 0) {
            state = BLE_SCANNER_PAUSED;
        } else if (held) {
            state = BLE_SCANNER_HOLD_OFF;
        } else {
            state = BLE_SCANNER_BACKOFF;
        }
        s_state.store(state);
    }
}

esp_err_t ble_scanner_start(const ble_scanner_config_t *cfg)
{
    if (cfg == nullptr || cfg->sink == nullptr || s_task != nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    s_cfg = *cfg;
    s_pause.store(cfg->initial_pause);
    if (xTaskCreate(scan_task, "ble_scan", 3072, nullptr, 5, &s_task) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (cfg->own_host) {
        const esp_err_t err = nimble_port_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nimble_port_init() failed: %s", esp_err_to_name(err));
            return err;
        }
        ble_hs_cfg.reset_cb = on_host_reset;
        ble_hs_cfg.sync_cb = on_host_sync;
        nimble_port_freertos_init(host_task);
    }
    return ESP_OK;
}

void ble_scanner_pause(uint32_t reasons)
{
    const uint32_t before = s_pause.fetch_or(reasons);
    if ((before & reasons) != reasons) {
        notify_task();
    }
}

void ble_scanner_resume(uint32_t reasons)
{
    const uint32_t before = s_pause.fetch_and(~reasons);
    if ((before & reasons) != 0) {
        notify_task();
    }
}

void ble_scanner_hold_off(uint32_t ms)
{
    const int64_t until = now_ms() + ms;
    int64_t cur = s_hold_until_ms.load();
    while (cur < until && !s_hold_until_ms.compare_exchange_weak(cur, until)) {
    }
    notify_task();
}

void ble_scanner_get_stats(ble_scanner_stats_t *out)
{
    out->state = static_cast<ble_scanner_state_t>(s_state.load());
    out->pause_mask = s_pause.load();
    out->adv_reports = s_adv_reports.load();
    out->ibeacon_reports = s_ibeacon_reports.load();
    out->tlm_reports = s_tlm_reports.load();
    out->starts = s_starts.load();
    out->start_failures = s_start_failures.load();
    out->unexpected_stops = s_unexpected_stops.load();
    out->watchdog_restarts = s_watchdog_restarts.load();
    out->last_error = s_last_error.load();
    out->scan_clock_ms = ble_scanner_scan_clock_ms();
    out->interval_ms = s_cfg.interval_ms;
    out->window_ms = s_cfg.window_ms;
    out->active = s_cfg.active;
}

const char *ble_scanner_state_name(ble_scanner_state_t state)
{
    switch (state) {
    case BLE_SCANNER_WAIT_HOST:
        return "wait_host";
    case BLE_SCANNER_PAUSED:
        return "paused";
    case BLE_SCANNER_HOLD_OFF:
        return "hold_off";
    case BLE_SCANNER_BACKOFF:
        return "backoff";
    case BLE_SCANNER_RUNNING:
        return "running";
    default:
        return "stopped";
    }
}

const char *ble_scanner_pause_text(uint32_t mask, char *buf, size_t size)
{
    static const struct {
        uint32_t bit;
        const char *name;
    } kNames[] = {
        { SCAN_PAUSE_MATTER_STARTING, "matter_starting" }, { SCAN_PAUSE_NOT_COMMISSIONED, "not_commissioned" },
        { SCAN_PAUSE_COMMISSIONING, "commissioning" },     { SCAN_PAUSE_BLE_CONNECTION, "ble_connection" },
        { SCAN_PAUSE_NETWORK_DOWN, "network_down" },       { SCAN_PAUSE_USER, "user" },
        { SCAN_PAUSE_NO_STATION, "no_station" },
    };
    if (size == 0) {
        return buf;
    }
    buf[0] = '\0';
    size_t used = 0;
    for (const auto &n : kNames) {
        if ((mask & n.bit) == 0) {
            continue;
        }
        const int w = snprintf(&buf[used], size - used, "%s%s", used ? "," : "", n.name);
        if (w < 0 || static_cast<size_t>(w) >= size - used) {
            break;
        }
        used += static_cast<size_t>(w);
    }
    return buf;
}
