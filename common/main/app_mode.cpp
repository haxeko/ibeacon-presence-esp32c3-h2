/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "app_mode.h"

#include <string.h>

#include <atomic>

#include <esp_attr.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>

#include "sdkconfig.h"

#include "app_mode_logic.h"

static const char *TAG = "app_mode";

// Survives esp_restart(), random after power-on: app_mode_rtc_consume() validates it.
static RTC_NOINIT_ATTR app_mode_rtc_t s_rtc_request;

static app_mode_t s_mode = APP_MODE_NORMAL;
static esp_reset_reason_t s_reset_reason = ESP_RST_UNKNOWN;
static QueueHandle_t s_requests;
static std::atomic<int64_t> s_last_activity_ms{ 0 };

namespace {
struct RestartRequest {
    app_mode_t target;
    uint32_t delay_ms;
    const char *reason; // string literal
};
} // namespace

static int64_t now_ms()
{
    return esp_timer_get_time() / 1000;
}

const char *app_mode_name(app_mode_t mode)
{
    return mode == APP_MODE_CONFIG ? "CONFIG" : "NORMAL";
}

const char *app_mode_reset_reason_name(void)
{
    switch (s_reset_reason) {
    case ESP_RST_POWERON:
        return "power-on";
    case ESP_RST_EXT:
        return "external";
    case ESP_RST_SW:
        return "software";
    case ESP_RST_PANIC:
        return "panic";
    case ESP_RST_INT_WDT:
        return "interrupt watchdog";
    case ESP_RST_TASK_WDT:
        return "task watchdog";
    case ESP_RST_WDT:
        return "watchdog";
    case ESP_RST_DEEPSLEEP:
        return "deep sleep";
    case ESP_RST_BROWNOUT:
        return "brown-out";
    case ESP_RST_USB:
        return "USB";
    case ESP_RST_JTAG:
        return "JTAG";
    default:
        return "other";
    }
}

app_mode_t app_mode_init(void)
{
    s_reset_reason = esp_reset_reason();
    const bool config = app_mode_rtc_consume(&s_rtc_request, s_reset_reason == ESP_RST_SW);
    s_mode = config ? APP_MODE_CONFIG : APP_MODE_NORMAL;
    s_last_activity_ms = now_ms();
    ESP_LOGI(TAG, "Boot in %s mode (reset: %s)", app_mode_name(s_mode), app_mode_reset_reason_name());
    return s_mode;
}

app_mode_t app_mode_get(void)
{
    return s_mode;
}

[[noreturn]] static void restart_now(const RestartRequest &req)
{
    if (req.delay_ms > 0) {
        vTaskDelay(pdMS_TO_TICKS(req.delay_ms));
    }
    if (req.target == APP_MODE_CONFIG) {
        app_mode_rtc_arm_config(&s_rtc_request);
    } else {
        memset(&s_rtc_request, 0, sizeof(s_rtc_request));
    }
    ESP_LOGW(TAG, "Restarting into %s mode (%s)", app_mode_name(req.target), req.reason);
    vTaskDelay(pdMS_TO_TICKS(100)); // let the log line out
    esp_restart();
}

static void mode_task(void *)
{
    const int64_t idle_timeout_ms = static_cast<int64_t>(CONFIG_PRESENCE_CONFIG_IDLE_TIMEOUT_MIN) * 60 * 1000;
    for (;;) {
        RestartRequest req;
        if (xQueueReceive(s_requests, &req, pdMS_TO_TICKS(1000)) == pdTRUE) {
            restart_now(req);
        }
        if (s_mode == APP_MODE_CONFIG && now_ms() - s_last_activity_ms.load() > idle_timeout_ms) {
            ESP_LOGW(TAG, "No interaction for %d min in CONFIG mode", CONFIG_PRESENCE_CONFIG_IDLE_TIMEOUT_MIN);
            restart_now({ APP_MODE_NORMAL, 0, "idle timeout" });
        }
    }
}

esp_err_t app_mode_start(void)
{
    s_requests = xQueueCreate(1, sizeof(RestartRequest));
    if (s_requests == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    return xTaskCreate(mode_task, "app_mode", 3072, nullptr, 6, nullptr) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void app_mode_request_restart(app_mode_t target, uint32_t delay_ms, const char *reason)
{
    if (s_requests == nullptr) {
        restart_now({ target, delay_ms, reason }); // before app_mode_start(): only from app_main
    }
    const RestartRequest req = { target, delay_ms, reason };
    if (xQueueSend(s_requests, &req, 0) != pdTRUE) {
        ESP_LOGW(TAG, "Restart already pending, %s ignored", reason);
    }
}

void app_mode_on_triple_click(void)
{
    const app_mode_t target = s_mode == APP_MODE_NORMAL ? APP_MODE_CONFIG : APP_MODE_NORMAL;
    app_mode_request_restart(target, 0, "BOOT triple click");
}

void app_mode_note_activity(void)
{
    s_last_activity_ms = now_ms();
}

uint32_t app_mode_idle_remaining_s(void)
{
    const int64_t idle_timeout_ms = static_cast<int64_t>(CONFIG_PRESENCE_CONFIG_IDLE_TIMEOUT_MIN) * 60 * 1000;
    const int64_t left = idle_timeout_ms - (now_ms() - s_last_activity_ms.load());
    return left > 0 ? static_cast<uint32_t>(left / 1000) : 0;
}
