/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// Matter SDK overrides, included by CHIPConfig.h through CONFIG_CHIP_PROJECT_CONFIG.

#pragma once

#include "sdkconfig.h"

// Basic Information VendorName / ProductName (the example device instance info provider returns these)
#define CHIP_DEVICE_CONFIG_DEVICE_VENDOR_NAME CONFIG_PRESENCE_VENDOR_NAME
#define CHIP_DEVICE_CONFIG_DEVICE_PRODUCT_NAME CONFIG_PRESENCE_PRODUCT_NAME

// Name in commissionable node discovery (DNS-SD "DN"), shown by some controllers before pairing
#define CHIP_DEVICE_CONFIG_ENABLE_COMMISSIONABLE_DEVICE_NAME 1
#define CHIP_DEVICE_CONFIG_DEVICE_NAME CONFIG_PRESENCE_DISCOVERY_NAME
