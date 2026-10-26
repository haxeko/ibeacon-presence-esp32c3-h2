// Simulated NVS (see fake_nvs.h). Writes stay pending until nvs_commit(); reads see pending writes first, as a
// handle does on the device.
#include "fake_nvs.h"

#include <string.h>

#include "bootloader_random.h"
#include "esp_random.h"
#include "nvs.h"
#include "nvs_flash.h"

FakeNvs g_nvs;

struct Handle {
    std::string ns;
    bool writable;
};
static std::map<nvs_handle_t, Handle> s_handles;
static nvs_handle_t s_next_handle = 1;
static int s_random_enabled;

void fake_nvs_reset()
{
    g_nvs = FakeNvs();
    s_handles.clear();
}

const char *esp_err_to_name(esp_err_t code)
{
    switch (code) {
    case ESP_OK:
        return "ESP_OK";
    case ESP_ERR_NVS_NOT_FOUND:
        return "ESP_ERR_NVS_NOT_FOUND";
    case ESP_ERR_NVS_NO_FREE_PAGES:
        return "ESP_ERR_NVS_NO_FREE_PAGES";
    case ESP_ERR_NVS_NEW_VERSION_FOUND:
        return "ESP_ERR_NVS_NEW_VERSION_FOUND";
    case ESP_ERR_NVS_NOT_ENOUGH_SPACE:
        return "ESP_ERR_NVS_NOT_ENOUGH_SPACE";
    case ESP_ERR_NVS_INVALID_LENGTH:
        return "ESP_ERR_NVS_INVALID_LENGTH";
    case ESP_ERR_INVALID_CRC:
        return "ESP_ERR_INVALID_CRC";
    default:
        return "ESP_ERR_OTHER";
    }
}

esp_err_t nvs_flash_init_partition(const char *label)
{
    if (g_nvs.partition != label) {
        g_nvs.foreign_partition_calls++;
        return ESP_ERR_NVS_PART_NOT_FOUND;
    }
    g_nvs.initialized = g_nvs.init_result == ESP_OK;
    return g_nvs.init_result;
}

esp_err_t nvs_flash_erase_partition(const char *label)
{
    if (g_nvs.partition != label) {
        g_nvs.foreign_partition_calls++;
        return ESP_ERR_NVS_PART_NOT_FOUND;
    }
    g_nvs.erase_calls++;
    g_nvs.committed.clear();
    g_nvs.pending.clear();
    g_nvs.initialized = false;
    g_nvs.init_result = g_nvs.init_result_after_erase;
    return ESP_OK;
}

esp_err_t nvs_flash_erase(void)
{
    g_nvs.erase_all_calls++;
    return ESP_OK;
}

esp_err_t nvs_open_from_partition(const char *part, const char *ns, nvs_open_mode_t mode, nvs_handle_t *out)
{
    if (g_nvs.partition != part) {
        g_nvs.foreign_partition_calls++;
        return ESP_ERR_NVS_PART_NOT_FOUND;
    }
    if (!g_nvs.initialized) {
        return ESP_ERR_NVS_NOT_INITIALIZED;
    }
    if (g_nvs.open_result != ESP_OK) {
        return g_nvs.open_result;
    }
    if (mode == NVS_READONLY) {
        // Opening a namespace that was never written fails read-only, as on the device
        bool exists = false;
        const std::string prefix = std::string(ns) + "/";
        for (auto &kv : g_nvs.committed) {
            exists = exists || kv.first.compare(0, prefix.size(), prefix) == 0;
        }
        if (!exists) {
            return ESP_ERR_NVS_NOT_FOUND;
        }
    }
    *out = s_next_handle++;
    s_handles[*out] = { ns, mode == NVS_READWRITE };
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle)
{
    s_handles.erase(handle);
}

static std::string key_of(nvs_handle_t handle, const char *key)
{
    return s_handles[handle].ns + "/" + key;
}

static const std::vector<uint8_t> *lookup(nvs_handle_t handle, const char *key)
{
    const std::string k = key_of(handle, key);
    auto it = g_nvs.pending.find(k);
    if (it != g_nvs.pending.end()) {
        return &it->second;
    }
    auto jt = g_nvs.committed.find(k);
    return jt == g_nvs.committed.end() ? nullptr : &jt->second;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length)
{
    if (g_nvs.set_result != ESP_OK) {
        return g_nvs.set_result;
    }
    const uint8_t *p = static_cast<const uint8_t *>(value);
    g_nvs.pending[key_of(handle, key)] = std::vector<uint8_t>(p, p + length);
    return ESP_OK;
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out, size_t *length)
{
    const std::vector<uint8_t> *v = lookup(handle, key);
    if (v == nullptr) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (out == nullptr) { // as NVS: the size only
        *length = v->size();
        return ESP_OK;
    }
    if (*length < v->size()) {
        *length = v->size();
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out, v->data(), v->size());
    if (g_nvs.corrupt_read && !v->empty()) {
        static_cast<uint8_t *>(out)[v->size() / 2] ^= 0x01;
    }
    *length = v->size();
    return ESP_OK;
}

esp_err_t nvs_set_str(nvs_handle_t handle, const char *key, const char *value)
{
    return nvs_set_blob(handle, key, value, strlen(value) + 1);
}

esp_err_t nvs_get_str(nvs_handle_t handle, const char *key, char *out, size_t *length)
{
    const bool corrupt = g_nvs.corrupt_read;
    g_nvs.corrupt_read = false;
    const esp_err_t err = nvs_get_blob(handle, key, out, length);
    g_nvs.corrupt_read = corrupt;
    return err;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    if (g_nvs.commit_result != ESP_OK) {
        // The pending write is lost, the committed data is unchanged
        g_nvs.pending.clear();
        return g_nvs.commit_result;
    }
    for (auto &kv : g_nvs.pending) {
        g_nvs.committed[kv.first] = kv.second;
    }
    g_nvs.pending.clear();
    return ESP_OK;
}

void bootloader_random_enable(void)
{
    s_random_enabled++;
}

void bootloader_random_disable(void)
{
    s_random_enabled--;
}

void esp_fill_random(void *buf, size_t len)
{
    static uint32_t state = 0x12345678u;
    uint8_t *p = static_cast<uint8_t *>(buf);
    for (size_t i = 0; i < len; i++) {
        state = state * 1664525u + 1013904223u;
        p[i] = static_cast<uint8_t>(state >> 24);
    }
}
