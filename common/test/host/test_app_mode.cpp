// One-shot CONFIG request in RTC memory (main/app_mode_logic.cpp): valid only after a software restart, only
// once, and never from the random content RTC memory has after power-on.
#include "test.h"

#include <stdlib.h>

#include "app_mode_logic.h"

static void request_after_software_restart()
{
    app_mode_rtc_t rtc;
    memset(&rtc, 0xA5, sizeof(rtc));
    app_mode_rtc_arm_config(&rtc);
    CHECK(app_mode_rtc_consume(&rtc, true));
}

static void request_is_one_shot()
{
    app_mode_rtc_t rtc;
    app_mode_rtc_arm_config(&rtc);
    CHECK(app_mode_rtc_consume(&rtc, true));
    CHECK(!app_mode_rtc_consume(&rtc, true)); // the next boot is NORMAL again
}

static void power_on_ignores_request()
{
    // Power cut right after the request: RTC memory may even still hold it, but the reset is not a software one
    app_mode_rtc_t rtc;
    app_mode_rtc_arm_config(&rtc);
    CHECK(!app_mode_rtc_consume(&rtc, false));
    CHECK(rtc.magic == 0 && rtc.crc == 0); // cleared anyway
    app_mode_rtc_arm_config(&rtc);
    CHECK(app_mode_rtc_consume(&rtc, true)); // a later armed request still works
}

static void corrupted_records_rejected()
{
    for (int field = 0; field < 4; field++) {
        app_mode_rtc_t rtc;
        app_mode_rtc_arm_config(&rtc);
        reinterpret_cast<uint32_t *>(&rtc)[field] ^= 0x00010000u;
        CHECK(!app_mode_rtc_consume(&rtc, true));
    }
}

static void random_memory_rejected()
{
    srand(7);
    int accepted = 0;
    for (int i = 0; i < 100000; i++) {
        app_mode_rtc_t rtc;
        uint8_t *p = reinterpret_cast<uint8_t *>(&rtc);
        for (size_t b = 0; b < sizeof(rtc); b++) {
            p[b] = static_cast<uint8_t>(rand());
        }
        accepted += app_mode_rtc_consume(&rtc, true) ? 1 : 0;
    }
    CHECK(accepted == 0);
}

int main()
{
    RUN(request_after_software_restart);
    RUN(request_is_one_shot);
    RUN(power_on_ignores_request);
    RUN(corrupted_records_rejected);
    RUN(random_memory_rejected);
    return test_summary("app_mode");
}
