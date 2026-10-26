// nvs_flash.h for the host tests (implemented by fake_nvs.cpp)
#pragma once
#include "esp_err.h"

esp_err_t nvs_flash_init_partition(const char *partition_label);
esp_err_t nvs_flash_erase_partition(const char *part_name);
esp_err_t nvs_flash_erase(void);
