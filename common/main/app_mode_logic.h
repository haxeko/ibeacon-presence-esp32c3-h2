/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// One-shot request for the CONFIG mode, kept in RTC memory across esp_restart(). No ESP-IDF dependency (host
// tests); app_mode.cpp places the record in RTC_NOINIT memory.
//
// RTC_NOINIT memory keeps its content over a software restart but holds random data after power-on, so the
// request is accepted only when all of these hold:
//   - the reset was a software restart (esp_restart(); not power-on, brown-out, watchdog or panic)
//   - magic and its complement are both right
//   - the CRC-32 over the record matches
// and it is cleared before the decision is returned, so it applies to exactly one boot. A crash in the CONFIG
// mode, a power cut or any other reset leads back to NORMAL. Nothing about the mode is written to flash, so the
// device can never be stuck in CONFIG.

#pragma once

#include <stdbool.h>
#include <stdint.h>

#define APP_MODE_RTC_MAGIC 0x50524E43u          // "PRNC"
#define APP_MODE_RTC_REQUEST_CONFIG 0x43464721u // "CFG!"

typedef struct {
    uint32_t magic;
    uint32_t magic_inv;
    uint32_t request;
    uint32_t crc;
} app_mode_rtc_t;

// Records a CONFIG request for the next boot.
void app_mode_rtc_arm_config(app_mode_rtc_t *rtc);

// Returns true when a valid CONFIG request is recorded and the reset was a software restart. Always clears the
// record.
bool app_mode_rtc_consume(app_mode_rtc_t *rtc, bool software_reset);
