/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "wifi_config.h"

#include <stdio.h>
#include <string.h>

#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_wifi.h>

#include "sdkconfig.h"

static const char *TAG = "wifi_config";

static esp_netif_t *s_netif;
static wifi_config_stations_fn_t s_on_stations;

void wifi_config_ssid(char ssid[WIFI_CONFIG_SSID_SIZE])
{
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(ssid, WIFI_CONFIG_SSID_SIZE, "%s%02X%02X", CONFIG_PRESENCE_AP_SSID_PREFIX, mac[4], mac[5]);
}

// Stations associated, the one leaving left out (whether or not the driver still lists it); -1 on error
static int stations_except(const uint8_t *leaving)
{
    wifi_sta_list_t list = {};
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK) {
        return -1;
    }
    int count = 0;
    for (int i = 0; i < list.num; i++) {
        if (leaving == nullptr || memcmp(list.sta[i].mac, leaving, sizeof(list.sta[i].mac)) != 0) {
            count++;
        }
    }
    return count;
}

static void wifi_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base != WIFI_EVENT) {
        return;
    }
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        const auto *e = static_cast<wifi_event_ap_staconnected_t *>(data);
        const int count = stations_except(nullptr);
        ESP_LOGI(TAG, "Station " MACSTR " connected (%d connected)", MAC2STR(e->mac), count);
        if (s_on_stations) {
            s_on_stations(count > 0 ? count : 1, true); // at least the one that just joined
        }
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        const auto *e = static_cast<wifi_event_ap_stadisconnected_t *>(data);
        const int count = stations_except(e->mac);
        ESP_LOGI(TAG, "Station " MACSTR " left (reason %d, %d still connected)", MAC2STR(e->mac), e->reason, count);
        if (s_on_stations && count >= 0) {
            s_on_stations(count, false);
        }
    }
}

esp_err_t wifi_config_start(const char *password, wifi_config_stations_fn_t on_stations)
{
    const size_t pw_len = strlen(password);
    if (pw_len < 8 || pw_len > 63) {
        ESP_LOGE(TAG, "SoftAP password must be 8-63 characters: SoftAP not started");
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err = esp_netif_init();
    if (err == ESP_OK) {
        err = esp_event_loop_create_default();
    }
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        return err;
    }
    s_netif = esp_netif_create_default_wifi_ap();
    if (s_netif == nullptr) {
        return ESP_FAIL;
    }

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    init.nvs_enable = 0;
    err = esp_wifi_init(&init);
    if (err != ESP_OK) {
        return err;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    s_on_stations = on_stations;
    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr, nullptr);

    char ssid[WIFI_CONFIG_SSID_SIZE];
    wifi_config_ssid(ssid);
    wifi_config_t cfg = {};
    memcpy(cfg.ap.ssid, ssid, strlen(ssid));
    cfg.ap.ssid_len = static_cast<uint8_t>(strlen(ssid));
    memcpy(cfg.ap.password, password, pw_len);
    cfg.ap.channel = CONFIG_PRESENCE_AP_CHANNEL;
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.max_connection = CONFIG_PRESENCE_AP_MAX_CONNECTIONS;
    cfg.ap.pmf_cfg.required = false; // capable, not required: older phones still connect

    err = esp_wifi_set_mode(WIFI_MODE_AP);
    if (err == ESP_OK) {
        err = esp_wifi_set_config(WIFI_IF_AP, &cfg);
    }
    if (err == ESP_OK) {
        // Not fatal: the SoftAP also works on 40 MHz
        const esp_err_t bw = esp_wifi_set_bandwidth(WIFI_IF_AP, WIFI_BW20);
        if (bw != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_set_bandwidth(20 MHz): %s", esp_err_to_name(bw));
        }
        err = esp_wifi_start();
    }
    if (err != ESP_OK) {
        return err;
    }
#if CONFIG_PRESENCE_AP_LIMIT_TX_POWER
    const esp_err_t pw = esp_wifi_set_max_tx_power(CONFIG_PRESENCE_AP_MAX_TX_POWER); // only after esp_wifi_start()
    if (pw != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_set_max_tx_power(%d): %s", CONFIG_PRESENCE_AP_MAX_TX_POWER, esp_err_to_name(pw));
    }
#endif
    int8_t power = 0;
    esp_wifi_get_max_tx_power(&power);
    char ip[WIFI_CONFIG_IP_SIZE];
    wifi_config_ip(ip);
    ESP_LOGI(TAG, "SoftAP \"%s\" on channel %d (WPA2, 20 MHz, max %d.%02d dBm), page at http://%s", ssid,
             CONFIG_PRESENCE_AP_CHANNEL, power / 4, (power % 4) * 25, ip);
    return ESP_OK;
}

void wifi_config_ip(char ip[WIFI_CONFIG_IP_SIZE])
{
    esp_netif_ip_info_t info = {};
    ip[0] = '\0';
    if (s_netif != nullptr && esp_netif_get_ip_info(s_netif, &info) == ESP_OK) {
        snprintf(ip, WIFI_CONFIG_IP_SIZE, IPSTR, IP2STR(&info.ip));
    }
}

int wifi_config_station_count(void)
{
    wifi_sta_list_t list = {};
    return esp_wifi_ap_get_sta_list(&list) == ESP_OK ? list.num : 0;
}

void wifi_config_log_diagnostics(void)
{
    wifi_sta_list_t list = {};
    int8_t power = 0;
    if (esp_wifi_ap_get_sta_list(&list) != ESP_OK || esp_wifi_get_max_tx_power(&power) != ESP_OK) {
        return;
    }
    ESP_LOGI(TAG, "SoftAP channel %d, max %d.%02d dBm, %d station(s)", CONFIG_PRESENCE_AP_CHANNEL, power / 4,
             (power % 4) * 25, list.num);
    for (int i = 0; i < list.num; i++) {
        ESP_LOGI(TAG, "  station " MACSTR ", RSSI %d dBm", MAC2STR(list.sta[i].mac), list.sta[i].rssi);
    }
}
