/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "config_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <bootloader_random.h>
#include <esp_log.h>
#include <esp_random.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <nvs.h>
#include <nvs_flash.h>

#include "sdkconfig.h"

static const char *TAG = "config_store";

static const char kPartition[] = CONFIG_PRESENCE_CFG_PARTITION;
static const char kNamespace[] = "presence";
static const char kKeyConfig[] = "cfg";
static const char kKeyApPassword[] = "ap_pw";
static const char kKeyWatch[] = "mac_watch";
static constexpr size_t kGeneratedPasswordLen = 12;

static SemaphoreHandle_t s_lock;
// Guarded by s_lock: written by config_store_save() / config_store_format() (web server task), read by any task.
static presence_config_t s_config;
static config_store_status_t s_status = CONFIG_STORE_UNAVAILABLE;
static char s_detail[40];
static char s_ap_password[CONFIG_STORE_AP_PASSWORD_SIZE];

namespace {

class Locked
{
public:
    Locked() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Locked() { xSemaphoreGive(s_lock); }
};

// The blob and a decoded configuration take about 2 KB each with 20 slots: on the heap, not on the stack of the
// task that loads (main task) or saves (web server)
struct Scratch {
    uint8_t blob[PRESENCE_CONFIG_BLOB_MAX_SIZE];
    uint8_t check[PRESENCE_CONFIG_BLOB_MAX_SIZE];
    presence_config_t cfg;
};

class ScratchBuffer
{
public:
    ScratchBuffer() : mScratch(static_cast<Scratch *>(malloc(sizeof(Scratch)))) {}
    ~ScratchBuffer() { free(mScratch); }
    Scratch *get() { return mScratch; }

private:
    Scratch *mScratch;
};

} // namespace

static void set_state(config_store_status_t status, const char *detail)
{
    Locked lock;
    s_status = status;
    snprintf(s_detail, sizeof(s_detail), "%s", detail);
}

static void load()
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(kPartition, kNamespace, NVS_READONLY, &handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        set_state(CONFIG_STORE_DEFAULTS, ""); // namespace not created yet: first boot
        return;
    }
    if (err != ESP_OK) {
        set_state(CONFIG_STORE_UNAVAILABLE, esp_err_to_name(err));
        return;
    }
    ScratchBuffer buffer;
    Scratch *sc = buffer.get();
    if (sc == nullptr) {
        nvs_close(handle);
        set_state(CONFIG_STORE_INVALID, esp_err_to_name(ESP_ERR_NO_MEM));
        return;
    }
    size_t len = sizeof(sc->blob);
    err = nvs_get_blob(handle, kKeyConfig, sc->blob, &len);
    nvs_close(handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        set_state(CONFIG_STORE_DEFAULTS, "");
        return;
    }
    if (err != ESP_OK) {
        // ESP_ERR_NVS_INVALID_LENGTH: larger than any format this firmware knows
        set_state(CONFIG_STORE_INVALID, esp_err_to_name(err));
        return;
    }
    const presence_decode_result_t result = presence_config_decode(sc->blob, len, &sc->cfg);
    if (result != PRESENCE_DECODE_OK) {
        set_state(CONFIG_STORE_INVALID, presence_decode_result_name(result));
        return;
    }
    {
        Locked lock;
        s_config = sc->cfg;
    }
    set_state(CONFIG_STORE_OK, "");
}

esp_err_t config_store_init(void)
{
    if (s_lock == nullptr) {
        s_lock = xSemaphoreCreateMutex();
        if (s_lock == nullptr) {
            return ESP_ERR_NO_MEM;
        }
    }
    {
        Locked lock;
        presence_config_set_defaults(&s_config);
    }

    const esp_err_t err = nvs_flash_init_partition(kPartition);
    if (err != ESP_OK) {
        set_state(CONFIG_STORE_UNAVAILABLE, esp_err_to_name(err));
        ESP_LOGE(TAG,
                 "Partition '%s' cannot be used (%s): all slots off. It is NOT erased automatically; format it "
                 "from the configuration page (it holds the beacon settings only, never the Matter pairing)",
                 kPartition, esp_err_to_name(err));
        return err;
    }
    load();
    switch (config_store_status()) {
    case CONFIG_STORE_OK:
        ESP_LOGI(TAG, "Beacon configuration loaded from '%s'", kPartition);
        break;
    case CONFIG_STORE_DEFAULTS:
        ESP_LOGI(TAG, "No beacon configuration saved yet: all slots off until configured (triple-click BOOT)");
        break;
    default:
        ESP_LOGE(TAG, "Saved beacon configuration rejected (%s): all slots off, the data is left in flash",
                 config_store_status_detail());
        break;
    }
    return ESP_OK;
}

void config_store_get(presence_config_t *out)
{
    Locked lock;
    *out = s_config;
}

config_store_status_t config_store_status(void)
{
    Locked lock;
    return s_status;
}

const char *config_store_status_name(config_store_status_t status)
{
    switch (status) {
    case CONFIG_STORE_OK:
        return "ok";
    case CONFIG_STORE_DEFAULTS:
        return "defaults";
    case CONFIG_STORE_INVALID:
        return "invalid";
    default:
        return "unavailable";
    }
}

const char *config_store_status_detail(void)
{
    // Only replaced together with the status, by the task that saves; reading the static buffer is safe enough
    // for a diagnostic string.
    return s_detail;
}

esp_err_t config_store_save(const presence_config_t *cfg)
{
    if (config_store_status() == CONFIG_STORE_UNAVAILABLE) {
        return ESP_ERR_INVALID_STATE;
    }
    ScratchBuffer buffer;
    Scratch *sc = buffer.get();
    if (sc == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const size_t len = presence_config_encode(cfg, sc->blob, sizeof(sc->blob));
    if (len == 0) {
        return ESP_ERR_INVALID_SIZE;
    }

    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(kPartition, kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Save failed: cannot open '%s' (%s)", kPartition, esp_err_to_name(err));
        return err;
    }
    err = nvs_set_blob(handle, kKeyConfig, sc->blob, len);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    if (err != ESP_OK) {
        nvs_close(handle);
        ESP_LOGE(TAG, "Save failed: %s (the previous configuration stays in use and in flash)", esp_err_to_name(err));
        return err;
    }

    // Read back what NVS now returns and check it is exactly what was written
    size_t check_len = sizeof(sc->check);
    err = nvs_get_blob(handle, kKeyConfig, sc->check, &check_len);
    nvs_close(handle);
    if (err == ESP_OK &&
        (check_len != len || memcmp(sc->check, sc->blob, len) != 0 ||
         presence_config_decode(sc->check, check_len, &sc->cfg) != PRESENCE_DECODE_OK)) {
        err = ESP_ERR_INVALID_CRC;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Save failed: read back check %s", esp_err_to_name(err));
        return err;
    }

    {
        Locked lock;
        s_config = sc->cfg;
    }
    set_state(CONFIG_STORE_OK, "");
    ESP_LOGI(TAG, "Beacon configuration saved and verified (%u bytes)", static_cast<unsigned>(len));
    return ESP_OK;
}

esp_err_t config_store_load_watch(mac_watch_t *records, size_t n)
{
    memset(records, 0, n * sizeof(*records));
    if (config_store_status() == CONFIG_STORE_UNAVAILABLE) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(kPartition, kNamespace, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return err; // ESP_ERR_NVS_NOT_FOUND: namespace not created yet
    }
    // Its size first: written by a build with more slots, it is larger than this build's own
    size_t len = 0;
    err = nvs_get_blob(handle, kKeyWatch, nullptr, &len);
    uint8_t *buf = err == ESP_OK && len > 0 && len <= MAC_WATCH_BLOB_SIZE(255) ? static_cast<uint8_t *>(malloc(len))
                                                                              : nullptr;
    if (err == ESP_OK) {
        err = buf == nullptr ? ESP_ERR_NO_MEM : nvs_get_blob(handle, kKeyWatch, buf, &len);
    }
    nvs_close(handle);
    if (err == ESP_OK && !mac_watch_decode(buf, len, records, n)) {
        err = ESP_ERR_INVALID_CRC;
    }
    free(buf);
    return err;
}

esp_err_t config_store_save_watch(const mac_watch_t *records, size_t n)
{
    if (config_store_status() == CONFIG_STORE_UNAVAILABLE) {
        return ESP_ERR_INVALID_STATE;
    }
    const size_t size = MAC_WATCH_BLOB_SIZE(n);
    uint8_t *buf = static_cast<uint8_t *>(malloc(size));
    if (buf == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    const size_t len = mac_watch_encode(records, n, buf, size);
    nvs_handle_t handle;
    esp_err_t err =
        len == 0 ? ESP_ERR_INVALID_SIZE : nvs_open_from_partition(kPartition, kNamespace, NVS_READWRITE, &handle);
    if (err == ESP_OK) {
        err = nvs_set_blob(handle, kKeyWatch, buf, len);
        if (err == ESP_OK) {
            err = nvs_commit(handle);
        }
        nvs_close(handle);
    }
    free(buf);
    return err;
}

static bool password_valid(const char *pw)
{
    const size_t len = strnlen(pw, CONFIG_STORE_AP_PASSWORD_SIZE);
    if (len < 8 || len > CONFIG_STORE_AP_PASSWORD_SIZE - 1) {
        return false;
    }
    for (size_t i = 0; i < len; i++) {
        if (pw[i] < 0x20 || pw[i] > 0x7E) {
            return false;
        }
    }
    return true;
}

static esp_err_t store_password(const char *pw)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open_from_partition(kPartition, kNamespace, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, kKeyApPassword, pw);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static bool read_password(char *out, size_t size)
{
    nvs_handle_t handle;
    if (nvs_open_from_partition(kPartition, kNamespace, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    size_t len = size;
    const esp_err_t err = nvs_get_str(handle, kKeyApPassword, out, &len);
    nvs_close(handle);
    return err == ESP_OK && password_valid(out);
}

// 12 characters out of 32 (no 0/O, 1/l): 60 bits, typed easily on a phone
static void generate_password(char *out)
{
    static const char kAlphabet[] = "abcdefghijkmnpqrstuvwxyz23456789";
    static_assert(sizeof(kAlphabet) - 1 == 32, "alphabet of 32 symbols");
    uint8_t rnd[kGeneratedPasswordLen];
    // No radio is running yet: the SAR ADC noise source makes the hardware RNG truly random.
    bootloader_random_enable();
    esp_fill_random(rnd, sizeof(rnd));
    bootloader_random_disable();
    for (size_t i = 0; i < kGeneratedPasswordLen; i++) {
        out[i] = kAlphabet[rnd[i] & 0x1F];
    }
    out[kGeneratedPasswordLen] = '\0';
}

void config_store_ap_password(char out[CONFIG_STORE_AP_PASSWORD_SIZE])
{
    if (s_ap_password[0] == '\0') {
        const char *configured = CONFIG_PRESENCE_AP_PASSWORD;
        if (configured[0] != '\0' && password_valid(configured)) {
            snprintf(s_ap_password, sizeof(s_ap_password), "%s", configured);
        } else {
            if (configured[0] != '\0') {
                ESP_LOGE(TAG, "CONFIG_PRESENCE_AP_PASSWORD must be 8-63 printable characters: using the "
                              "per-device password instead");
            }
            const bool usable = config_store_status() != CONFIG_STORE_UNAVAILABLE;
            if (!usable || !read_password(s_ap_password, sizeof(s_ap_password))) {
                generate_password(s_ap_password);
                const esp_err_t err = usable ? store_password(s_ap_password) : ESP_ERR_INVALID_STATE;
                if (err != ESP_OK) {
                    ESP_LOGW(TAG, "SoftAP password not stored (%s): a new one is made at every boot",
                             esp_err_to_name(err));
                }
            }
        }
    }
    snprintf(out, CONFIG_STORE_AP_PASSWORD_SIZE, "%s", s_ap_password);
}

esp_err_t config_store_format(void)
{
    // nvs_flash_erase_partition() de-initialises the partition first if it is in use.
    esp_err_t err = nvs_flash_erase_partition(kPartition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Format of '%s' failed: %s", kPartition, esp_err_to_name(err));
        return err;
    }
    err = nvs_flash_init_partition(kPartition);
    {
        Locked lock;
        presence_config_set_defaults(&s_config);
    }
    if (err != ESP_OK) {
        set_state(CONFIG_STORE_UNAVAILABLE, esp_err_to_name(err));
        ESP_LOGE(TAG, "Partition '%s' still unusable after format: %s", kPartition, esp_err_to_name(err));
        return err;
    }
    set_state(CONFIG_STORE_DEFAULTS, "");
    // Keep the password the SoftAP of this boot is using, so the phone stays connected and the next CONFIG
    // session uses the same one.
    if (s_ap_password[0] != '\0' && CONFIG_PRESENCE_AP_PASSWORD[0] == '\0') {
        store_password(s_ap_password);
    }
    ESP_LOGW(TAG, "Partition '%s' formatted on request: beacon configuration back to defaults", kPartition);
    return ESP_OK;
}
