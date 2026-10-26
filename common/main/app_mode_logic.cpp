/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "app_mode_logic.h"

#include <stddef.h>
#include <string.h>

#include "crc32.h"

static uint32_t record_crc(const app_mode_rtc_t *rtc)
{
    return crc32_ieee(rtc, offsetof(app_mode_rtc_t, crc));
}

void app_mode_rtc_arm_config(app_mode_rtc_t *rtc)
{
    rtc->magic = APP_MODE_RTC_MAGIC;
    rtc->magic_inv = ~APP_MODE_RTC_MAGIC;
    rtc->request = APP_MODE_RTC_REQUEST_CONFIG;
    rtc->crc = record_crc(rtc);
}

bool app_mode_rtc_consume(app_mode_rtc_t *rtc, bool software_reset)
{
    const bool valid = rtc->magic == APP_MODE_RTC_MAGIC && rtc->magic_inv == static_cast<uint32_t>(~APP_MODE_RTC_MAGIC) &&
                       rtc->request == APP_MODE_RTC_REQUEST_CONFIG && rtc->crc == record_crc(rtc);
    memset(rtc, 0, sizeof(*rtc));
    return valid && software_reset;
}
