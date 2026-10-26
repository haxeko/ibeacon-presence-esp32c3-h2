// Simulated NVS for the host tests of config_store.cpp: one partition in memory, with injectable failures.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "esp_err.h"

struct FakeNvs {
    std::string partition = "app_cfg";
    esp_err_t init_result = ESP_OK; // returned by nvs_flash_init_partition()
    esp_err_t init_result_after_erase = ESP_OK;
    esp_err_t open_result = ESP_OK;
    esp_err_t set_result = ESP_OK;
    esp_err_t commit_result = ESP_OK;
    bool corrupt_read = false; // nvs_get_blob() returns the data with one bit flipped
    bool initialized = false;
    int erase_calls = 0;          // nvs_flash_erase_partition() calls on the app partition
    int erase_all_calls = 0;      // nvs_flash_erase() calls: must stay 0
    int foreign_partition_calls = 0; // calls on any other partition: must stay 0
    std::map<std::string, std::vector<uint8_t>> committed; // key "namespace/key"
    std::map<std::string, std::vector<uint8_t>> pending;
};

extern FakeNvs g_nvs;
void fake_nvs_reset();
