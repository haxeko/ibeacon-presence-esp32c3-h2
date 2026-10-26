/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "click_detector.h"

#include <string.h>

void click_detector_init(click_detector_t *cd, const click_detector_config_t *cfg, bool pressed_now, uint32_t now_ms)
{
    memset(cd, 0, sizeof(*cd));
    cd->cfg = *cfg;
    cd->raw = pressed_now;
    cd->raw_since_ms = now_ms;
    cd->pressed = pressed_now;
    cd->armed = !pressed_now;
}

click_event_t click_detector_update(click_detector_t *cd, bool pressed, uint32_t now_ms)
{
    // Debounce
    if (pressed != cd->raw) {
        cd->raw = pressed;
        cd->raw_since_ms = now_ms;
    }
    if (cd->raw == cd->pressed || now_ms - cd->raw_since_ms < cd->cfg.debounce_ms) {
        // No new level: a press may reach the hold time
        if (cd->pressed && cd->armed && cd->cfg.hold_ms > 0 && !cd->held &&
            now_ms - cd->press_start_ms >= cd->cfg.hold_ms) {
            cd->held = true;
            cd->clicks = 0; // a hold ends any sequence
            return CLICK_EVENT_HOLD;
        }
        return CLICK_EVENT_NONE;
    }
    cd->pressed = cd->raw;
    const uint32_t edge_ms = cd->raw_since_ms; // when the level changed, not when the debounce confirmed it

    if (cd->pressed) {
        // A pause longer than gap_ms ended the previous sequence: this press starts a new one
        if (cd->clicks > 0 && edge_ms - cd->release_ms > cd->cfg.gap_ms) {
            cd->clicks = 0;
        }
        cd->press_start_ms = edge_ms;
        cd->held = false;
        return CLICK_EVENT_NONE;
    }

    if (!cd->armed) {
        cd->armed = true; // first release after start-up: the press began before the detector
        return CLICK_EVENT_NONE;
    }
    if (cd->held) {
        cd->held = false;
        cd->clicks = 0;
        return CLICK_EVENT_HOLD_RELEASED;
    }
    if (edge_ms - cd->press_start_ms > cd->cfg.max_press_ms) {
        cd->clicks = 0; // a long press: neither a click nor part of a sequence
        return CLICK_EVENT_NONE;
    }
    cd->clicks++;
    cd->release_ms = edge_ms;
    if (cd->clicks < cd->cfg.clicks_needed) {
        return CLICK_EVENT_NONE;
    }
    cd->clicks = 0;
    return CLICK_EVENT_CLICKS;
}
