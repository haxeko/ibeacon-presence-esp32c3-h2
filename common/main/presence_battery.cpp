/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "presence_battery.h"

#include <string.h>

void presence_battery_init(presence_battery_t *b)
{
    memset(b, 0, sizeof(*b));
    b->published = PRESENCE_BATTERY_UNKNOWN;
}

uint8_t presence_battery_percent(const presence_battery_config_t *cfg, uint16_t mv)
{
    if (mv == 0) {
        return PRESENCE_BATTERY_UNKNOWN;
    }
    if (cfg->full_mv <= cfg->empty_mv) {
        return mv >= cfg->full_mv ? 100 : 0; // misconfigured range: full or empty
    }
    if (mv <= cfg->empty_mv) {
        return 0;
    }
    if (mv >= cfg->full_mv) {
        return 100;
    }
    const uint32_t span = cfg->full_mv - cfg->empty_mv;
    return static_cast<uint8_t>(((mv - cfg->empty_mv) * 100u + span / 2) / span);
}

presence_battery_level_t presence_battery_level(const presence_battery_config_t *cfg, uint8_t percent)
{
    if (percent == PRESENCE_BATTERY_UNKNOWN) {
        return PRESENCE_BATTERY_OK;
    }
    if (percent <= cfg->critical_pct) {
        return PRESENCE_BATTERY_CRITICAL;
    }
    return percent <= cfg->warning_pct ? PRESENCE_BATTERY_WARNING : PRESENCE_BATTERY_OK;
}

static uint16_t median(const uint16_t *values, uint8_t n)
{
    uint16_t sorted[PRESENCE_BATTERY_MEDIAN_OF];
    memcpy(sorted, values, n * sizeof(uint16_t));
    for (uint8_t i = 1; i < n; i++) {
        const uint16_t v = sorted[i];
        uint8_t j = i;
        while (j > 0 && sorted[j - 1] > v) {
            sorted[j] = sorted[j - 1];
            j--;
        }
        sorted[j] = v;
    }
    // Even count: the lower of the two middle values (the battery is never better than measured)
    return sorted[(n - 1) / 2];
}

bool presence_battery_add(presence_battery_t *b, const presence_battery_config_t *cfg, uint16_t mv)
{
    if (mv == 0) {
        return false;
    }
    if (b->count < PRESENCE_BATTERY_MEDIAN_OF) {
        b->readings[b->count++] = mv;
    } else {
        memmove(&b->readings[0], &b->readings[1], (PRESENCE_BATTERY_MEDIAN_OF - 1) * sizeof(uint16_t));
        b->readings[PRESENCE_BATTERY_MEDIAN_OF - 1] = mv;
    }
    b->median_mv = median(b->readings, b->count);
    const uint8_t percent = presence_battery_percent(cfg, b->median_mv);
    const uint8_t old = b->published;
    if (old == PRESENCE_BATTERY_UNKNOWN || percent + cfg->step_pct <= old || percent >= old + cfg->replaced_pct) {
        b->published = percent;
    }
    return b->published != old;
}
