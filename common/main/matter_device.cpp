/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "matter_device.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include <atomic>

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_system.h>
#include <esp_timer.h>
#if CONFIG_ENABLE_CHIP_SHELL
#include <esp_matter_console.h>
#endif
#if CONFIG_ENABLE_MATTER_OVER_THREAD
#include <esp_openthread.h>
#include <esp_openthread_lock.h>
#include <openthread/link.h>
#include <openthread/thread.h>
#endif
#if CONFIG_ENABLE_WIFI_STATION
#include <esp_event.h>
#include <esp_wifi.h>
#endif

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>
#include <data_model_provider/clusters/occupancy_sensing/integration.h>
#include <platform/CHIPDeviceLayer.h>
#if CONFIG_ENABLE_MATTER_OVER_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#endif
#include <setup_payload/OnboardingCodesUtil.h>

#include "sdkconfig.h"

#include "ble_scanner.h"
#include "presence_config.h"
#if CONFIG_PRESENCE_ROLE_MATTER_UART
#include "matter_bridge.h"
#endif

// Two-chip version, Matter node: the ESP32-C3 scans with its own radio, so nothing here pauses a scanner
#if CONFIG_PRESENCE_ROLE_MATTER_UART
static void gate_pause(uint32_t) {}
static void gate_resume(uint32_t) {}
static void gate_hold_off(uint32_t) {}
#else
static void gate_pause(uint32_t reasons)
{
    ble_scanner_pause(reasons);
}
static void gate_resume(uint32_t reasons)
{
    ble_scanner_resume(reasons);
}
static void gate_hold_off(uint32_t ms)
{
    ble_scanner_hold_off(ms);
}
#endif

// ESP32-C6 version: Thread; ESP32-C3 version: Wi-Fi station (ble_presence.cmake)
#if defined(CONFIG_ENABLE_MATTER_OVER_THREAD) == defined(CONFIG_ENABLE_WIFI_STATION)
#error "Build with exactly one Matter network: Thread (ESP32-C6 version) or Wi-Fi station (ESP32-C3 version)"
#endif

static const char *TAG = "matter";

using namespace esp_matter;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;
using chip::DeviceLayer::ConnectivityManager;
using chip::DeviceLayer::ConnectivityMgr;

// Window reopened after the last fabric is removed
static constexpr uint32_t kRecommissioningWindowSec = 300;
#if CONFIG_PRESENCE_ROLE_MATTER_UART
static std::atomic<bool> s_window_open{ false };
#endif

static std::atomic<bool> s_started{ false };
static std::atomic<bool> s_initialized{ false }; // on_matter_started() done: the status below is the real one
static std::atomic<uint32_t> s_network_ups{ 0 };
static std::atomic<uint32_t> s_network_downs{ 0 };
// For matter_device_get_status() (any task)
static std::atomic<bool> s_commissioned{ false };
static std::atomic<bool> s_network_up{ false };
static std::atomic<uint8_t> s_fabrics{ 0 };
#if CONFIG_ENABLE_WIFI_STATION
static std::atomic<uint16_t> s_wifi_disconnect_reason{ 0 }; // wifi_err_reason_t of the last disconnection
#endif

#if CONFIG_PRESENCE_ROLE_MATTER_UART
// Onboarding codes for the scanner's page: written once in the Matter task, then only read
static plink_pairing_t s_pairing;
static std::atomic<bool> s_pairing_valid{ false };
#else
// Matter "Common Number" semantic tag namespace (data_model/1.6.1/namespaces/Namespace-Common-Number.xml)
static constexpr uint8_t kNamespaceCommonNumber = 0x07;
static uint16_t s_endpoint_ids[PRESENCE_SLOT_COUNT];
// TagList: the labels must stay valid for the life of the node
static char s_tag_labels[PRESENCE_SLOT_COUNT][16];
static chip::app::DataModel::Provider::SemanticTag s_tags[PRESENCE_SLOT_COUNT];
#endif

static uint32_t occupancy_feature()
{
#if CONFIG_PRESENCE_OCCUPANCY_FEATURE_OTHER
    return cluster::occupancy_sensing::feature::other::get_id();
#else
    return cluster::occupancy_sensing::feature::rf_sensing::get_id();
#endif
}

static bool is_commissioned()
{
    return chip::Server::GetInstance().GetFabricTable().FabricCount() > 0;
}

static void hold_off_scanner()
{
    gate_hold_off(CONFIG_PRESENCE_SCAN_RESUME_DELAY_SEC * 1000);
}

// Matter task
static void update_commissioning_gate()
{
    const uint8_t fabrics = chip::Server::GetInstance().GetFabricTable().FabricCount();
    s_fabrics = fabrics;
    s_commissioned = fabrics > 0;
#if CONFIG_PRESENCE_ROLE_MATTER_UART
    matter_bridge_set_commissioned(fabrics > 0); // memory reserve of the bridge
#endif
    if (fabrics > 0) {
        gate_resume(SCAN_PAUSE_NOT_COMMISSIONED);
    } else {
        gate_pause(SCAN_PAUSE_NOT_COMMISSIONED);
    }
}

// Matter task: Thread attached / Wi-Fi connected, or not
static void set_network_up(bool up)
{
    s_network_up = up;
    if (up) {
        hold_off_scanner();
        gate_resume(SCAN_PAUSE_NETWORK_DOWN);
    } else {
        gate_pause(SCAN_PAUSE_NETWORK_DOWN);
    }
}

#if CONFIG_ENABLE_MATTER_OVER_THREAD

// Thread role ("child", "detached"...)
static const char *network_state(char *, size_t)
{
    otInstance *ot = esp_openthread_get_instance();
    if (ot == nullptr) {
        return "none";
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    const char *role = otThreadDeviceRoleToString(otThreadGetDeviceRole(ot));
    esp_openthread_lock_release();
    return role;
}

static const char *device_type_name(ConnectivityManager::ThreadDeviceType type)
{
    switch (type) {
    case ConnectivityManager::kThreadDeviceType_Router:
        return "router";
    case ConnectivityManager::kThreadDeviceType_FullEndDevice:
        return "full end device";
    case ConnectivityManager::kThreadDeviceType_MinimalEndDevice:
        return "minimal end device";
    case ConnectivityManager::kThreadDeviceType_SleepyEndDevice:
        return "sleepy end device";
    default:
        return "other";
    }
}

#else // Wi-Fi station

// Signal and channel of the access point, written to buf
static const char *network_state(char *buf, size_t size)
{
    wifi_ap_record_t ap = {};
    if (esp_wifi_sta_get_ap_info(&ap) != ESP_OK) {
        return "not connected";
    }
    snprintf(buf, size, "RSSI %d dBm, channel %u", ap.rssi, ap.primary);
    return buf;
}

// Event loop task: only the reason is kept, for the logs
static void on_wifi_disconnected(void *, esp_event_base_t, int32_t, void *data)
{
    s_wifi_disconnect_reason.store(static_cast<const wifi_event_sta_disconnected_t *>(data)->reason);
}

#endif

#if CONFIG_PRESENCE_ROLE_MATTER_UART
// Matter task: the codes printed at boot, kept for the configuration page of the scanner (no console needed)
static void store_pairing_codes()
{
    const auto flags = chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE);
    char manual[PLINK_MANUAL_CODE_LEN + 1] = {};
    char qr[PLINK_QR_CODE_MAX + 1] = {};
    chip::MutableCharSpan manual_span(manual);
    chip::MutableCharSpan qr_span(qr);
    if (GetManualPairingCode(manual_span, flags) != CHIP_NO_ERROR || manual_span.size() != PLINK_MANUAL_CODE_LEN) {
        ESP_LOGW(TAG, "Manual pairing code unavailable: not shown on the scanner's page");
        return;
    }
    memcpy(s_pairing.manual_code, manual, PLINK_MANUAL_CODE_LEN);
    if (GetQRCode(qr_span, flags) == CHIP_NO_ERROR && qr_span.size() <= PLINK_QR_CODE_MAX) {
        memcpy(s_pairing.qr_code, qr, qr_span.size());
    }
    s_pairing_valid.store(true, std::memory_order_release);
}
#endif

// Matter task, once, right after esp_matter::start() (Server::Init done, fabrics loaded). kServerReady is not used
// for this: it waits for DNS-SD, which on the Thread version means waiting for an SRP server to answer. That cannot
// happen before commissioning (no Thread network yet: the pairing codes would never be printed) and never happens
// while the SRP server of the Border Router is unreachable (the scanner would stay paused).
static void on_matter_started(intptr_t)
{
#if CONFIG_ENABLE_MATTER_OVER_THREAD
    // MTD build: Minimal End Device keeps the receiver on when idle (no polling delay; the device is mains
    // powered). It is the OpenThread default for an MTD, set explicitly so a future SDK default cannot change it.
    const CHIP_ERROR err = ConnectivityMgr().SetThreadDeviceType(ConnectivityManager::kThreadDeviceType_MinimalEndDevice);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "Setting the Thread device type failed: %" CHIP_ERROR_FORMAT, err.Format());
    }
    ESP_LOGI(TAG, "Thread device type: %s", device_type_name(ConnectivityMgr().GetThreadDeviceType()));
    const bool network_up = ConnectivityMgr().IsThreadAttached();
#else
    const bool network_up = ConnectivityMgr().IsWiFiStationConnected();
#endif

    gate_resume(SCAN_PAUSE_MATTER_STARTING);
    update_commissioning_gate();
    set_network_up(network_up);
#if CONFIG_PRESENCE_ROLE_MATTER_UART
    s_window_open = chip::Server::GetInstance().GetCommissioningWindowManager().IsCommissioningWindowOpen();
    matter_bridge_load();
    store_pairing_codes();
#endif
    s_initialized = true;
    if (!is_commissioned()) {
        ESP_LOGI(TAG, "Not commissioned: commissioning window open (BLE), beacon scanning paused until done");
        PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
    } else {
        char state[40];
        ESP_LOGI(TAG, "Commissioned (%u fabric(s)), %s %s",
                 static_cast<unsigned>(chip::Server::GetInstance().GetFabricTable().FabricCount()),
                 MATTER_DEVICE_NETWORK_NAME, network_state(state, sizeof(state)));
    }
}

// Matter task: kBLEDeinitialized seen (CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING, two-chip version)
static bool s_ble_released;

static void restart_cb(void *)
{
    esp_restart();
}

// Matter task. The SDK released BLE after commissioning and cannot bring it back (BLEManagerImpl keeps
// kBleDeinitAndMemReleased): a window opened now would advertise on DNS-SD only, while the printed code says BLE.
// With no fabric left, a restart boots the device uncommissioned with BLE and its normal commissioning window.
// Nothing is erased: the Thread dataset and the other settings stay.
static void restart_for_commissioning()
{
    static esp_timer_handle_t timer;
    ESP_LOGW(TAG, "Last fabric removed: restarting in 2 s so the device can be commissioned over BLE again");
    esp_timer_create_args_t args = {};
    args.callback = restart_cb;
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = "recommission";
    if (timer == nullptr && esp_timer_create(&args, &timer) != ESP_OK) {
        esp_restart();
    }
    esp_timer_start_once(timer, 2000 * 1000); // the answer to RemoveFabric goes out first
}

// Matter task
static void open_commissioning_window()
{
    chip::CommissioningWindowManager &mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
    if (mgr.IsCommissioningWindowOpen()) {
        return;
    }
    // BLE is still initialised (CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING=n, or not commissioned since boot): advertise
    // over BLE and DNS-SD
    const CHIP_ERROR err = mgr.OpenBasicCommissioningWindow(chip::System::Clock::Seconds32(kRecommissioningWindowSec),
                                                            chip::CommissioningWindowAdvertisement::kAllSupported);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to open the commissioning window: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }
    PrintOnboardingCodes(chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
}

// Matter task: kThreadConnectivityChange / kWiFiConnectivityChange
static void on_connectivity_change(chip::DeviceLayer::ConnectivityChange result)
{
    char state[40];
    if (result == chip::DeviceLayer::kConnectivity_Established) {
        s_network_ups.fetch_add(1);
        ESP_LOGI(TAG, "%s up (%s): scanning resumes in %d s", MATTER_DEVICE_NETWORK_NAME,
                 network_state(state, sizeof(state)), CONFIG_PRESENCE_SCAN_RESUME_DELAY_SEC);
        set_network_up(true);
    } else if (result == chip::DeviceLayer::kConnectivity_Lost) {
        s_network_downs.fetch_add(1);
#if CONFIG_ENABLE_WIFI_STATION
        ESP_LOGW(TAG, "Wi-Fi down (reason %u): scanner paused until connected again",
                 static_cast<unsigned>(s_wifi_disconnect_reason.load()));
#else
        ESP_LOGW(TAG, "Thread detached: scanner paused until attached again");
#endif
        set_network_up(false);
    }
}

static void app_event_cb(const ChipDeviceEvent *event, intptr_t)
{
    namespace DeviceEventType = chip::DeviceLayer::DeviceEventType;
    switch (event->Type) {
    case DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Commissioning session started: scanner paused");
        gate_pause(SCAN_PAUSE_COMMISSIONING);
        break;

    case DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "Commissioning session stopped");
        hold_off_scanner();
        gate_resume(SCAN_PAUSE_COMMISSIONING);
        break;

    case DeviceEventType::kCHIPoBLEConnectionEstablished:
        ESP_LOGI(TAG, "BLE connection from a controller: scanner paused");
        gate_pause(SCAN_PAUSE_BLE_CONNECTION);
        break;

    case DeviceEventType::kCHIPoBLEConnectionClosed:
        ESP_LOGI(TAG, "BLE connection closed");
        hold_off_scanner();
        gate_resume(SCAN_PAUSE_BLE_CONNECTION);
        break;

    case DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete: scanner resumes once the network is up");
        hold_off_scanner();
        gate_resume(SCAN_PAUSE_COMMISSIONING);
        update_commissioning_gate();
        break;

    case DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGW(TAG, "Commissioning failed: fail-safe timer expired");
        gate_resume(SCAN_PAUSE_COMMISSIONING);
        update_commissioning_gate();
        break;

    case DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Commissioning window opened");
#if CONFIG_PRESENCE_ROLE_MATTER_UART
        s_window_open = true;
#endif
        break;

    case DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
#if CONFIG_PRESENCE_ROLE_MATTER_UART
        s_window_open = false;
#endif
        break;

    case DeviceEventType::kFabricRemoved:
        ESP_LOGI(TAG, "Fabric removed");
        update_commissioning_gate();
        if (!is_commissioned()) {
            if (s_ble_released) {
                restart_for_commissioning();
            } else {
                open_commissioning_window();
            }
        }
        break;

    case DeviceEventType::kFabricCommitted:
        update_commissioning_gate();
        break;

#if CONFIG_ENABLE_MATTER_OVER_THREAD
    case DeviceEventType::kThreadConnectivityChange:
        on_connectivity_change(event->ThreadConnectivityChange.Result);
        break;

    case DeviceEventType::kThreadStateChange:
        if (event->ThreadStateChange.RoleChanged) {
            char state[40];
            ESP_LOGI(TAG, "Thread role: %s", network_state(state, sizeof(state)));
        }
        break;
#else
    case DeviceEventType::kWiFiConnectivityChange:
        on_connectivity_change(event->WiFiConnectivityChange.Result);
        break;

    case DeviceEventType::kInterfaceIpAddressChanged:
        switch (event->InterfaceIpAddressChanged.Type) {
        case chip::DeviceLayer::InterfaceIpChangeType::kIpV4_Assigned:
            ESP_LOGI(TAG, "Wi-Fi: IPv4 address assigned");
            break;
        case chip::DeviceLayer::InterfaceIpChangeType::kIpV4_Lost:
            ESP_LOGW(TAG, "Wi-Fi: IPv4 address lost");
            break;
        case chip::DeviceLayer::InterfaceIpChangeType::kIpV6_Assigned:
            ESP_LOGI(TAG, "Wi-Fi: IPv6 address assigned");
            break;
        case chip::DeviceLayer::InterfaceIpChangeType::kIpV6_Lost:
            ESP_LOGW(TAG, "Wi-Fi: IPv6 address lost");
            break;
        default:
            break;
        }
        break;
#endif

    case DeviceEventType::kBLEDeinitialized:
        s_ble_released = true;
#if CONFIG_PRESENCE_ROLE_MATTER_UART
        // Two-chip version: BLE was only needed for commissioning (CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING)
        ESP_LOGI(TAG, "BLE released after commissioning");
#else
        // Only with CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING, which this firmware turns off
        ESP_LOGE(TAG, "BLE was released by the Matter SDK: beacon scanning is no longer possible");
#endif
        break;

    default:
        break;
    }
}

static esp_err_t identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                   uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identify endpoint %u: type %u, effect %u", endpoint_id, type, effect_id);
    return ESP_OK;
}

static esp_err_t attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                     uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    return ESP_OK; // no writable attribute drives anything here
}

#if !CONFIG_PRESENCE_ROLE_MATTER_UART
static esp_err_t create_presence_endpoints(node_t *node)
{
    for (int i = 0; i < PRESENCE_SLOT_COUNT; i++) {
        occupancy_sensor::config_t config;
        config.occupancy_sensing.feature_flags = occupancy_feature();
        endpoint_t *ep = occupancy_sensor::create(node, &config, ENDPOINT_FLAG_NONE, nullptr);
        if (ep == nullptr) {
            ESP_LOGE(TAG, "Cannot create the Occupancy Sensor endpoint of Presence %d", i + 1);
            return ESP_FAIL;
        }
        s_endpoint_ids[i] = endpoint::get_id(ep);

        snprintf(s_tag_labels[i], sizeof(s_tag_labels[i]), "Presence %d", i + 1);
        s_tags[i].namespaceID = kNamespaceCommonNumber;
        s_tags[i].tag = static_cast<uint8_t>(i + 1);
        s_tags[i].label.SetValue(
            chip::app::DataModel::MakeNullable(chip::CharSpan::fromCharString(s_tag_labels[i])));
        if (endpoint::set_semantic_tags(ep, &s_tags[i], 1) != ESP_OK) {
            ESP_LOGW(TAG, "TagList not set on endpoint %u", s_endpoint_ids[i]);
        }
        if (s_endpoint_ids[i] != i + 1) {
            ESP_LOGW(TAG, "Presence %d got endpoint %u instead of %d", i + 1, s_endpoint_ids[i], i + 1);
        }
    }
    ESP_LOGI(TAG, "Occupancy Sensor endpoints %u-%u (Presence 1-5), feature %s", s_endpoint_ids[0],
             s_endpoint_ids[PRESENCE_SLOT_COUNT - 1],
#if CONFIG_PRESENCE_OCCUPANCY_FEATURE_OTHER
             "Other"
#else
             "RFSensing"
#endif
    );
    return ESP_OK;
}
#endif

esp_err_t matter_device_start(void)
{
    node::config_t node_config;
    node_t *node = node::create(&node_config, attribute_update_cb, identification_cb);
    if (node == nullptr) {
        ESP_LOGE(TAG, "Failed to create the Matter node");
        return ESP_FAIL;
    }
#if CONFIG_PRESENCE_ROLE_MATTER_UART
    esp_err_t err = matter_bridge_create_aggregator(node, occupancy_feature());
#else
    esp_err_t err = create_presence_endpoints(node);
#endif
    if (err != ESP_OK) {
        return err;
    }

#if CONFIG_ENABLE_MATTER_OVER_THREAD
    esp_openthread_platform_config_t ot_config = {};
    ot_config.radio_config.radio_mode = RADIO_MODE_NATIVE;
    ot_config.host_config.host_connection_mode = HOST_CONNECTION_MODE_NONE;
    ot_config.port_config.storage_partition_name = "nvs";
    ot_config.port_config.netif_queue_size = 10;
    ot_config.port_config.task_queue_size = 10;
    set_openthread_platform_config(&ot_config);
#else
    // The default event loop esp_matter::start() would create, created here so that the reason of a disconnection is
    // recorded from the first connection attempt on
    err = esp_event_loop_create_default();
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) {
        esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_wifi_disconnected, nullptr);
    }
#endif

    err = esp_matter::start(app_event_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start Matter: %s", esp_err_to_name(err));
        return err;
    }
    s_started = true;
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(on_matter_started) != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Cannot schedule the start-up work: beacon scanning stays paused");
    }

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::factoryreset_register_commands();
#if CONFIG_OPENTHREAD_CLI
    esp_matter::console::otcli_register_commands();
#endif
#if CONFIG_ENABLE_WIFI_STATION
    esp_matter::console::wifi_register_commands();
#endif
    esp_matter::console::init();
#endif
    return ESP_OK;
}

void matter_device_get_status(bool *started, bool *commissioned, bool *network_up, uint8_t *fabrics)
{
    *started = s_initialized;
    *commissioned = s_commissioned;
    *network_up = s_network_up;
    *fabrics = s_fabrics;
}

bool matter_device_set_occupancy(int slot, bool occupied)
{
    if (!s_started || slot < 0 || slot >= PRESENCE_SLOT_COUNT) {
        return false;
    }
    lock::ScopedChipStackLock lock(portMAX_DELAY);
#if CONFIG_PRESENCE_ROLE_MATTER_UART
    if (!matter_bridge_ready()) {
        return false; // the restored devices are not back yet: retried
    }
    const uint16_t endpoint_id = matter_bridge_endpoint(slot);
    if (endpoint_id == 0) {
        return true; // no device for this slot on the bridge: nothing to show (set again when it is added)
    }
#else
    const uint16_t endpoint_id = s_endpoint_ids[slot];
#endif
    OccupancySensingCluster *cluster = OccupancySensing::FindClusterOnEndpoint(endpoint_id);
    if (cluster == nullptr) {
        return false;
    }
    cluster->SetOccupancy(occupied);
    return true;
}

#if CONFIG_PRESENCE_ROLE_MATTER_UART

bool matter_device_bridge_step(int slot, const plink_slot_t *want, bool *added)
{
    *added = false;
    if (!s_started) {
        return false;
    }
    lock::ScopedChipStackLock lock(portMAX_DELAY);
    return matter_bridge_step(slot, want, added);
}

void matter_device_get_bridge(plink_status_t *status)
{
    if (!s_started) {
        status->bridged_mask = 0;
        status->waiting_mask = 0;
        status->error_mask = 0;
        memset(status->endpoint, 0, sizeof(status->endpoint));
        return;
    }
    lock::ScopedChipStackLock lock(portMAX_DELAY);
    matter_bridge_get_status(status);
}

void matter_device_set_reachable(bool reachable)
{
    if (!s_started) {
        return;
    }
    lock::ScopedChipStackLock lock(portMAX_DELAY);
    matter_bridge_set_reachable(reachable);
}

bool matter_device_get_pairing(plink_pairing_t *out)
{
    if (!s_pairing_valid.load(std::memory_order_acquire)) {
        return false;
    }
    *out = s_pairing;
    return true;
}

bool matter_device_window_open(void)
{
    return s_window_open;
}

// Matter task
static void open_pairing_window_work(intptr_t)
{
    chip::CommissioningWindowManager &mgr = chip::Server::GetInstance().GetCommissioningWindowManager();
    if (mgr.IsCommissioningWindowOpen()) {
        ESP_LOGI(TAG, "Pairing mode asked: the commissioning window is already open");
        s_window_open = true;
        return;
    }
    // BLE is released once commissioned (CONFIG_USE_BLE_ONLY_FOR_COMMISSIONING): the window is then announced on
    // the Thread network only, where a second controller finds it (the bridge stays in the first one)
    const auto advertisement = s_ble_released ? chip::CommissioningWindowAdvertisement::kDnssdOnly
                                              : chip::CommissioningWindowAdvertisement::kAllSupported;
    const CHIP_ERROR err = mgr.OpenBasicCommissioningWindow(
        chip::System::Clock::Seconds32(CONFIG_PRESENCE_PAIRING_WINDOW_SEC), advertisement);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to open the commissioning window: %" CHIP_ERROR_FORMAT, err.Format());
        return;
    }
    ESP_LOGI(TAG, "Pairing mode: commissioning window open for %d s over %s", CONFIG_PRESENCE_PAIRING_WINDOW_SEC,
             s_ble_released ? "the Thread network" : "BLE and the Thread network");
    if (s_ble_released && !s_network_up) {
        ESP_LOGW(TAG, "Thread is not attached: no controller can find the window until it is");
    }
    PrintOnboardingCodes(chip::RendezvousInformationFlags(
        s_ble_released ? chip::RendezvousInformationFlag::kOnNetwork : chip::RendezvousInformationFlag::kBLE));
}

void matter_device_open_pairing_window(void)
{
    if (!s_started) {
        ESP_LOGE(TAG, "Pairing mode asked while Matter is not running: not done");
        return;
    }
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(open_pairing_window_work) != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Cannot schedule the opening of the commissioning window");
    }
}

static void factory_reset_work(intptr_t)
{
    ESP_LOGW(TAG, "Factory reset: removing the Matter pairing, the Thread network and the bridged devices");
    matter_bridge_erase();
    esp_matter::factory_reset(); // erases the Matter and Thread data, then restarts
}

bool matter_device_factory_reset(void)
{
    if (!s_started) {
        // Matter could not start (e.g. "nvs" unusable): nothing is erased (the page refuses the request then)
        ESP_LOGE(TAG, "Factory reset asked while Matter is not running: not done");
        return false;
    }
    if (chip::DeviceLayer::PlatformMgr().ScheduleWork(factory_reset_work) != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Cannot schedule the factory reset");
        return false;
    }
    return true;
}

#endif

#if CONFIG_ENABLE_MATTER_OVER_THREAD

void matter_device_log_diagnostics(void)
{
    otInstance *ot = s_started ? esp_openthread_get_instance() : nullptr;
    if (ot == nullptr) {
        return;
    }
    esp_openthread_lock_acquire(portMAX_DELAY);
    const otDeviceRole role = otThreadGetDeviceRole(ot);
    const otLinkModeConfig mode = otThreadGetLinkMode(ot);
    int8_t parent_rssi = 0;
    const bool has_parent_rssi = otThreadGetParentAverageRssi(ot, &parent_rssi) == OT_ERROR_NONE;
    const otMacCounters mac = *otLinkGetCounters(ot);
    const otMleCounters mle = *otThreadGetMleCounters(ot);
    esp_openthread_lock_release();

    ESP_LOGI(TAG, "Thread %s, rx-on-when-idle %d, full device %d, parent RSSI %d dBm%s; attached %u, detached %u",
             otThreadDeviceRoleToString(role), mode.mRxOnWhenIdle, mode.mDeviceType, parent_rssi,
             has_parent_rssi ? "" : " (n/a)", static_cast<unsigned>(s_network_ups.load()),
             static_cast<unsigned>(s_network_downs.load()));
    ESP_LOGI(TAG,
             "MAC tx %" PRIu32 " retry %" PRIu32 " cca-err %" PRIu32 " no-ack-expiry %" PRIu32 ", rx %" PRIu32
             " no-frame %" PRIu32 " fcs-err %" PRIu32 "; MLE detached %u child %u attach-attempts %u parent-changes %u",
             mac.mTxTotal, mac.mTxRetry, mac.mTxErrCca, mac.mTxDirectMaxRetryExpiry, mac.mRxTotal, mac.mRxErrNoFrame,
             mac.mRxErrFcs, mle.mDetachedRole, mle.mChildRole, mle.mAttachAttempts, mle.mParentChanges);
#if CONFIG_PRESENCE_ROLE_MATTER_UART
    lock::ScopedChipStackLock lock(portMAX_DELAY);
    matter_bridge_log_diagnostics();
#endif
}

#else // Wi-Fi station

void matter_device_log_diagnostics(void)
{
    if (!s_started) {
        return;
    }
    char state[40];
    ESP_LOGI(TAG, "Wi-Fi %s; connected %u, disconnected %u, last disconnection reason %u",
             network_state(state, sizeof(state)), static_cast<unsigned>(s_network_ups.load()),
             static_cast<unsigned>(s_network_downs.load()), static_cast<unsigned>(s_wifi_disconnect_reason.load()));
}

#endif
