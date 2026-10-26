/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// Matter device with five Occupancy Sensor endpoints ("Presence 1".."Presence 5"), each following one iBeacon heard
// by BLE passive scanning. ESP32-C6 version: Matter over Thread; ESP32-C3 version: Matter over Wi-Fi. Either way
// BLE and the network share the single 2.4 GHz radio of the chip.
//
// Two modes, chosen at boot (app_mode.h):
//   NORMAL  Matter (Thread Minimal End Device / Wi-Fi station) + NimBLE passive scanning (Matter's own host) +
//           presence. No SoftAP.
//   CONFIG  Triple click on BOOT. Matter is not started (its data, network included, stays untouched). Wi-Fi SoftAP
//           "VN-Presence-XXXX", web page at http://192.168.4.1, BLE scanning of the beacons around (own host).
//           Back to NORMAL by the page, another triple click, or after CONFIG_PRESENCE_CONFIG_IDLE_TIMEOUT_MIN
//           without interaction.
//
// Two-chip version, scanner (CONFIG_PRESENCE_ROLE_SCANNER, ESP32-C3): the same two modes without Matter, 20 slots.
// In NORMAL the scanner has the radio to itself (own NimBLE host, no Wi-Fi) and the presence, battery and model of
// each beacon go to the ESP32-H2 over the UART link (presence_link.h); the Matter node is app_main_h2.cpp.

#include <esp_app_desc.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_system.h>
#include <nvs_flash.h>

#include "sdkconfig.h"

#include "app_mode.h"
#include "ble_scanner.h"
#include "button_handler.h"
#include "config_store.h"
#include "diag.h"
#include "matter_device.h"
#include "presence_config.h"
#if CONFIG_PRESENCE_ROLE_SCANNER
#include "presence_link.h"
#endif
#include "presence_manager.h"
#include "scan_cache.h"
#include "web_server.h"
#include "wifi_config.h"

static const char *TAG = "app_main";

#if CONFIG_PRESENCE_CONFIG_ACTIVE_SCAN
static constexpr bool kConfigActiveScan = true;
#else
static constexpr bool kConfigActiveScan = false;
#endif

static char s_ssid[WIFI_CONFIG_SSID_SIZE];

static void check(esp_err_t err, const char *what)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s failed: %s", what, esp_err_to_name(err));
    }
}

static uint32_t active_mask(const presence_config_t &cfg)
{
    uint32_t mask = 0;
    for (int i = 0; i < PRESENCE_SLOT_COUNT; i++) {
        mask |= presence_slot_is_active(&cfg.slots[i]) ? 1u << i : 0;
    }
    return mask;
}

// The configuration the presence runs with, read once at boot: off the main task's stack (2 KB with 20 slots)
static presence_config_t s_cfg;

#if CONFIG_PRESENCE_ROLE_SCANNER

static void run_normal(esp_err_t)
{
    presence_config_t &cfg = s_cfg;
    config_store_get(&cfg);
    const uint32_t mask = active_mask(cfg);

    // Presence first: its queue exists before the scanner can deliver anything. The link always runs, so the
    // Matter node knows this chip is alive even with no beacon assigned.
    check(presence_manager_start(&cfg, presence_link_report), "Presence task");
    // The devices of the ESP32-H2's bridge, only from a configuration read from flash
    presence_link_set_slots(&cfg, config_store_status() == CONFIG_STORE_OK);
    check(presence_link_start_scanner(false, &cfg), "UART link");
    if (mask == 0) {
        ESP_LOGW(TAG, "No beacon assigned: BLE scanner not started");
        return;
    }
    ble_scanner_config_t scan = {};
    scan.own_host = true; // no Matter on this chip: the scanner owns NimBLE and the radio
    scan.active = false;  // passive: never transmits
    scan.interval_ms = cfg.scan_interval_ms;
    scan.window_ms = cfg.scan_window_ms;
    scan.parse_names = true; // the model of a bridged device without one configured: the name the beacon advertises
    scan.initial_pause = 0;
    scan.watchdog_s = CONFIG_PRESENCE_SCAN_WATCHDOG_SEC;
    scan.filter = presence_manager_filter;
    scan.sink = presence_manager_sink;
    check(ble_scanner_start(&scan), "BLE scanner");
}

#else

static void run_normal(esp_err_t nvs_err)
{
    if (nvs_err != ESP_OK) {
        // Matter would fail on this partition. It is left as it is (the pairing may still be recoverable):
        // README, "Lỗi NVS".
        ESP_LOGE(TAG,
                 "Matter NOT started: partition 'nvs' (fabrics, network) cannot be opened (%s). It is not "
                 "erased automatically. Triple-click BOOT for the configuration page.",
                 esp_err_to_name(nvs_err));
        return;
    }
    presence_config_t &cfg = s_cfg;
    config_store_get(&cfg);

    // Presence first: its queue exists before the scanner can deliver anything
    check(presence_manager_start(&cfg, matter_device_set_occupancy), "Presence task");

    if (active_mask(cfg) != 0) {
        // Before Matter: the Matter event callback drives the scanner's pause reasons from the first event on.
        ble_scanner_config_t scan = {};
        scan.own_host = false; // NimBLE belongs to the Matter SDK
        scan.active = false;   // passive: never transmits to scan
        scan.interval_ms = cfg.scan_interval_ms;
        scan.window_ms = cfg.scan_window_ms;
        scan.parse_names = false;
        scan.initial_pause = SCAN_PAUSE_MATTER_STARTING | SCAN_PAUSE_NETWORK_DOWN;
        scan.watchdog_s = CONFIG_PRESENCE_SCAN_WATCHDOG_SEC;
        scan.filter = presence_manager_filter;
        scan.sink = presence_manager_sink;
        check(ble_scanner_start(&scan), "BLE scanner");
    } else {
        ESP_LOGW(TAG, "No beacon assigned: BLE scanner not started, the radio is left to Matter");
    }

    check(matter_device_start(), "Matter");
}

#endif

// SoftAP next to BLE scanning is "C1, supported but the performance is unstable" in the ESP-IDF coexistence table
// once a station connects. So BLE scans only while a phone is on the SoftAP, and starts a few seconds after it
// joined: probe requests and connection attempts get the radio alone, then the phone its DHCP and first page.
static void on_ap_stations(int count, bool joined)
{
    if (joined) {
        ble_scanner_hold_off(CONFIG_PRESENCE_CONFIG_SCAN_HOLD_OFF_SEC * 1000); // before the resume below
    }
    if (count > 0) {
        ble_scanner_resume(SCAN_PAUSE_NO_STATION);
    } else {
        ble_scanner_pause(SCAN_PAUSE_NO_STATION);
    }
}

static void run_config(const char *ap_password, esp_err_t nvs_err)
{
    check(scan_cache_start(), "Beacon list");

    ble_scanner_config_t scan = {};
    scan.own_host = true; // Matter is not running: initialise NimBLE here
    scan.active = kConfigActiveScan;
    scan.interval_ms = CONFIG_PRESENCE_CONFIG_SCAN_INTERVAL_MS;
    scan.window_ms = CONFIG_PRESENCE_CONFIG_SCAN_WINDOW_MS;
    scan.parse_names = true;
    scan.initial_pause = SCAN_PAUSE_NO_STATION;
    scan.watchdog_s = CONFIG_PRESENCE_SCAN_WATCHDOG_SEC;
    scan.filter = nullptr;
    scan.sink = scan_cache_sink;
    check(ble_scanner_start(&scan), "BLE scanner");

#if CONFIG_PRESENCE_ROLE_SCANNER
    config_store_get(&s_cfg);
    presence_link_set_slots(&s_cfg, config_store_status() == CONFIG_STORE_OK); // updated again by every save
    check(presence_link_start_scanner(true, nullptr), "UART link"); // tells the Matter node to keep its states
#endif
    check(wifi_config_start(ap_password, on_ap_stations), "SoftAP");
    const web_server_info_t info = { s_ssid, nvs_err };
    check(web_server_start(&info), "Web server");
}

extern "C" void app_main(void)
{
    const app_mode_t mode = app_mode_init();
    ESP_LOGI(TAG, "%s %s, firmware %s, ESP-IDF %s", CONFIG_PRESENCE_VENDOR_NAME, CONFIG_PRESENCE_PRODUCT_NAME,
             esp_app_get_description()->version, esp_get_idf_version());

    // "nvs" holds the Matter fabrics and keys, the Thread dataset or the Wi-Fi credentials, and the PHY calibration.
    // Unlike the ESP-IDF examples, no nvs_flash_erase() on ESP_ERR_NVS_NO_FREE_PAGES / ESP_ERR_NVS_NEW_VERSION_FOUND:
    // that would silently drop the pairing. The error is reported instead.
    const esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init(): %s. Partition 'nvs' left untouched.", esp_err_to_name(nvs_err));
    }

    // Beacon configuration, own partition (never erased automatically either)
    config_store_init();
    // Before any radio starts (may use the RNG entropy source)
    char ap_password[CONFIG_STORE_AP_PASSWORD_SIZE];
    config_store_ap_password(ap_password);
    wifi_config_ssid(s_ssid);
    ESP_LOGI(TAG, "Configuration page: triple-click BOOT, join Wi-Fi \"%s\" (password \"%s\"), open http://192.168.4.1",
             s_ssid, ap_password);

    check(app_mode_start(), "Mode task");
    check(button_handler_start(app_mode_on_triple_click), "BOOT button");

    if (mode == APP_MODE_CONFIG) {
        run_config(ap_password, nvs_err);
    } else {
        run_normal(nvs_err);
    }
    check(diag_start(mode), "Diagnostics");
}
