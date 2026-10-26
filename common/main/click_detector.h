/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// Multi-click and hold detection on a polled button. No ESP-IDF dependency (host tests); button_handler.cpp
// (BOOT of the ESP32-C6 / C3) and status_ui.cpp (BOOT of the ESP32-H2) feed it.
//
//   - debounce: a level counts once it held for debounce_ms; bounces in between are ignored. Press and release
//     times are those of the level change itself, not of the end of the debounce.
//   - a click is a complete press and release; a press longer than max_press_ms is a hold, never a click, and it
//     cancels the clicks counted so far (a long hold can never add up to several clicks)
//   - clicks may be slow: each press only has to start within gap_ms of the previous release. A longer pause ends
//     the sequence and that press starts a new one, so there is no limit on the sequence as a whole.
//   - clicks_needed clicks fire once; the next press starts a new sequence
//   - hold_ms (0: off): a press reaching hold_ms reports a hold once while still pressed, then its release
//   - a button already pressed when the detector starts (or at boot) is ignored until it is released

#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t debounce_ms;
    uint32_t gap_ms;       // longest pause from a release to the next press of the same sequence
    uint32_t max_press_ms;
    uint8_t clicks_needed;
    uint32_t hold_ms; // 0: no hold events
} click_detector_config_t;

typedef enum {
    CLICK_EVENT_NONE = 0,
    CLICK_EVENT_CLICKS,        // the clicks_needed-th click of a sequence was released
    CLICK_EVENT_HOLD,          // the press reached hold_ms (still pressed)
    CLICK_EVENT_HOLD_RELEASED, // released after CLICK_EVENT_HOLD
} click_event_t;

typedef struct {
    click_detector_config_t cfg;
    bool raw;            // last sampled level
    uint32_t raw_since_ms;
    bool pressed;        // debounced level
    bool armed;          // false until a release is seen after start
    uint32_t press_start_ms;
    uint32_t release_ms; // release of the last click counted
    uint8_t clicks;      // clicks counted in the current sequence
    bool held;           // the current press reached hold_ms
} click_detector_t;

void click_detector_init(click_detector_t *cd, const click_detector_config_t *cfg, bool pressed_now, uint32_t now_ms);

// Feeds one sample (true = pressed). Returns each event once. Times may wrap: only differences are used.
click_event_t click_detector_update(click_detector_t *cd, bool pressed, uint32_t now_ms);
