/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// Battery level of a beacon from the voltage in its Eddystone-TLM frames. No ESP-IDF dependency (host tests);
// presence_manager.cpp runs it for each slot.
//
//   - percent: linear from empty_mv (0 %) to full_mv (100 %), clamped. A coin cell (CR2032, CR2477) or two
//     alkaline cells stay near 3 V for most of their life and drop quickly at the end: the percentage reads high
//     for months, then falls.
//   - the voltage of the frames moves by tens of mV (radio load, temperature): the percentage is computed from
//     the median of the last readings, and the published one only goes down by step_pct or more at a time, or up
//     by replaced_pct or more (a new battery). Noise never makes it flap.
//   - level (Matter BatChargeLevel): OK, Warning at warning_pct or below, Critical at critical_pct or below.

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define PRESENCE_BATTERY_UNKNOWN 0xFF // percent not known (no TLM frame yet, or the beacon does not measure)
#define PRESENCE_BATTERY_MEDIAN_OF 5

// Values of the Matter Power Source BatChargeLevelEnum
typedef enum {
    PRESENCE_BATTERY_OK = 0,
    PRESENCE_BATTERY_WARNING = 1,
    PRESENCE_BATTERY_CRITICAL = 2,
} presence_battery_level_t;

typedef struct {
    uint16_t empty_mv;
    uint16_t full_mv;
    uint8_t warning_pct;
    uint8_t critical_pct;
    uint8_t step_pct;     // smallest decrease published
    uint8_t replaced_pct; // smallest increase published (battery replaced)
} presence_battery_config_t;

typedef struct {
    uint16_t readings[PRESENCE_BATTERY_MEDIAN_OF]; // mV, newest last
    uint8_t count;
    uint8_t published; // percent, PRESENCE_BATTERY_UNKNOWN until the first reading
    uint16_t median_mv; // of the readings, 0 until the first one
} presence_battery_t;

void presence_battery_init(presence_battery_t *b);

// 0-100; PRESENCE_BATTERY_UNKNOWN for 0 mV (Eddystone-TLM: "not measured", e.g. a mains powered beacon)
uint8_t presence_battery_percent(const presence_battery_config_t *cfg, uint16_t mv);

// PRESENCE_BATTERY_OK for an unknown percent
presence_battery_level_t presence_battery_level(const presence_battery_config_t *cfg, uint8_t percent);

// A reading from a TLM frame. Returns true when the published percent changed. 0 mV readings are ignored.
bool presence_battery_add(presence_battery_t *b, const presence_battery_config_t *cfg, uint16_t mv);
