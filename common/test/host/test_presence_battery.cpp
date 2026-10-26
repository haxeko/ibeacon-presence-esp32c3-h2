// Beacon battery level (main/presence_battery.cpp): voltage to percent, charge level, and the filtering of the
// readings (median, published value that only steps down, or up for a new battery).
#include "test.h"

#include "presence_battery.h"

static const presence_battery_config_t kCfg = { 2200, 3000, 20, 10, 2, 15 };

static void percent()
{
    CHECK(presence_battery_percent(&kCfg, 0) == PRESENCE_BATTERY_UNKNOWN); // not measured
    CHECK(presence_battery_percent(&kCfg, 1500) == 0);
    CHECK(presence_battery_percent(&kCfg, 2200) == 0);
    CHECK(presence_battery_percent(&kCfg, 2600) == 50);
    CHECK(presence_battery_percent(&kCfg, 2204) == 1); // rounded to the nearest percent
    CHECK(presence_battery_percent(&kCfg, 2203) == 0);
    CHECK(presence_battery_percent(&kCfg, 2996) == 100);
    CHECK(presence_battery_percent(&kCfg, 3000) == 100);
    CHECK(presence_battery_percent(&kCfg, 3300) == 100); // fresh cell above "full"
    CHECK(presence_battery_percent(&kCfg, 65535) == 100);
    for (uint32_t mv = 1; mv < 4000; mv++) {
        CHECK(presence_battery_percent(&kCfg, static_cast<uint16_t>(mv)) <= 100);
    }
    // Inverted range: never a division by zero, full or empty
    const presence_battery_config_t bad = { 3000, 3000, 20, 10, 2, 15 };
    CHECK(presence_battery_percent(&bad, 2999) == 0 && presence_battery_percent(&bad, 3000) == 100);
}

static void level()
{
    CHECK(presence_battery_level(&kCfg, PRESENCE_BATTERY_UNKNOWN) == PRESENCE_BATTERY_OK);
    CHECK(presence_battery_level(&kCfg, 100) == PRESENCE_BATTERY_OK);
    CHECK(presence_battery_level(&kCfg, 21) == PRESENCE_BATTERY_OK);
    CHECK(presence_battery_level(&kCfg, 20) == PRESENCE_BATTERY_WARNING);
    CHECK(presence_battery_level(&kCfg, 11) == PRESENCE_BATTERY_WARNING);
    CHECK(presence_battery_level(&kCfg, 10) == PRESENCE_BATTERY_CRITICAL);
    CHECK(presence_battery_level(&kCfg, 0) == PRESENCE_BATTERY_CRITICAL);
}

static void filtering()
{
    presence_battery_t b;
    presence_battery_init(&b);
    CHECK(b.published == PRESENCE_BATTERY_UNKNOWN && b.median_mv == 0);
    CHECK(!presence_battery_add(&b, &kCfg, 0)); // "not measured" readings never count
    CHECK(b.count == 0);

    // First reading: published at once
    CHECK(presence_battery_add(&b, &kCfg, 2600));
    CHECK(b.published == 50 && b.median_mv == 2600);

    // Noise of a few mV around it, and a single low spike: no change
    CHECK(!presence_battery_add(&b, &kCfg, 2608));
    CHECK(!presence_battery_add(&b, &kCfg, 2592));
    CHECK(!presence_battery_add(&b, &kCfg, 2400)); // spike: the median of 4 is still about 2592
    CHECK(b.published == 50);
    CHECK(!presence_battery_add(&b, &kCfg, 2604));
    CHECK(b.count == PRESENCE_BATTERY_MEDIAN_OF && b.published == 50);

    // Upward noise never raises it
    for (int i = 0; i < 10; i++) {
        presence_battery_add(&b, &kCfg, 2680); // 60 %: up 10, below the 15 of a new battery
    }
    CHECK(b.published == 50);

    // A real decline: published in steps of 2 % or more
    for (int i = 0; i < 10; i++) {
        presence_battery_add(&b, &kCfg, 2584); // 48 %
    }
    CHECK(b.published == 48);
    for (int i = 0; i < 10; i++) {
        presence_battery_add(&b, &kCfg, 2576); // 47 %: only 1 below
    }
    CHECK(b.published == 48);
    for (int i = 0; i < 10; i++) {
        presence_battery_add(&b, &kCfg, 2296); // 12 %
    }
    CHECK(b.published == 12);
    CHECK(presence_battery_level(&kCfg, b.published) == PRESENCE_BATTERY_WARNING);

    // New battery: three readings of the new voltage move the median of five, then it is published
    CHECK(!presence_battery_add(&b, &kCfg, 3100));
    CHECK(!presence_battery_add(&b, &kCfg, 3100));
    CHECK(presence_battery_add(&b, &kCfg, 3100));
    CHECK(b.published == 100);
}

int main()
{
    RUN(percent);
    RUN(level);
    RUN(filtering);
    return test_summary("presence_battery");
}
