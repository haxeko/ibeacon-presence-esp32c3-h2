/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// Beacon configuration in flash: its own NVS partition (CONFIG_PRESENCE_CFG_PARTITION, "app_cfg"), separate from
// the "nvs" partition that holds the Matter fabrics, the network (Thread dataset / Wi-Fi credentials) and the PHY
// calibration, and from the factory partitions. Nothing here ever touches those.
//
// Error handling never erases anything on its own: a partition that NVS cannot open (no free pages, newer NVS
// format, missing partition) or a blob that does not decode leaves the device running with the defaults (every
// slot off) and the data untouched in flash; the web page shows the error and offers config_store_format(),
// which erases this partition only, when the user asks for it.

#pragma once

#include <stddef.h>

#include <esp_err.h>

#include "mac_watch.h"
#include "presence_config.h"

typedef enum {
    CONFIG_STORE_OK = 0,      // configuration read from flash
    CONFIG_STORE_DEFAULTS,    // nothing saved yet: defaults
    CONFIG_STORE_INVALID,     // saved data unreadable (CRC, newer format, values): defaults, data kept in flash
    CONFIG_STORE_UNAVAILABLE, // partition cannot be used: defaults, nothing can be saved
} config_store_status_t;

#define CONFIG_STORE_AP_PASSWORD_SIZE 64 // WPA2 passphrase: 8-63 characters and the NUL

// Opens the partition (never erases it) and loads the configuration. The device can run whatever it returns.
esp_err_t config_store_init(void);

// Copy of the configuration in use
void config_store_get(presence_config_t *out);

config_store_status_t config_store_status(void);
const char *config_store_status_name(config_store_status_t status);
// nvs_flash_init_partition() result, or why the stored blob was rejected ("bad CRC", ...); "" when fine
const char *config_store_status_detail(void);

// Writes the whole configuration as one blob, commits, reads it back and compares. Only on success does it become
// the configuration in use. The caller validates it first (presence_config_validate()).
esp_err_t config_store_save(const presence_config_t *cfg);

// Erases the app_cfg partition only (beacon configuration and SoftAP password), re-initialises it and keeps
// the SoftAP password of this boot. For an unusable or unreadable partition, on explicit request from the page.
esp_err_t config_store_format(void);

// MAC watch records of n slots (mac_watch.h), a blob of their own beside the configuration (the NORMAL mode updates
// them, the configuration page shows them and seeds them). load: ESP_ERR_NVS_NOT_FOUND when none was saved yet,
// ESP_ERR_INVALID_CRC when unreadable; the records are all reset then. A blob of another slot count gives its first
// records. Never erased on error.
esp_err_t config_store_load_watch(mac_watch_t *records, size_t n);
esp_err_t config_store_save_watch(const mac_watch_t *records, size_t n);

// SoftAP password: CONFIG_PRESENCE_AP_PASSWORD when set, else a random per-device password created once and kept
// in the app_cfg partition. Call before any radio starts (it may enable the RNG entropy source).
void config_store_ap_password(char out[CONFIG_STORE_AP_PASSWORD_SIZE]);
