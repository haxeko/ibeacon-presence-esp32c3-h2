/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "button_handler.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <esp_timer.h>

#include "sdkconfig.h"

#include "click_detector.h"

static const char *TAG = "button";

#if CONFIG_PRESENCE_BOOT_ACTIVE_LOW
static constexpr bool kActiveLow = true;
#else
static constexpr bool kActiveLow = false;
#endif
#if CONFIG_PRESENCE_BOOT_PULLUP
static constexpr bool kPullUp = true;
#else
static constexpr bool kPullUp = false;
#endif

static click_detector_t s_detector;
static uint8_t s_clicks_logged;
static void (*s_on_triple_click)(void);
static esp_timer_handle_t s_timer;

static bool read_pressed()
{
    const int level = gpio_get_level(static_cast<gpio_num_t>(CONFIG_PRESENCE_BOOT_GPIO));
    return kActiveLow ? level == 0 : level == 1;
}

static uint32_t now_ms()
{
    return static_cast<uint32_t>(esp_timer_get_time() / 1000);
}

// esp_timer task: sample, debounce, count. Only hands the triple click over.
static void poll_cb(void *)
{
    const bool fired = click_detector_update(&s_detector, read_pressed(), now_ms()) == CLICK_EVENT_CLICKS;
    if (s_detector.clicks != s_clicks_logged) {
        s_clicks_logged = s_detector.clicks;
        if (s_clicks_logged > 0) {
            ESP_LOGI(TAG, "BOOT click %u of %u (next press within %d ms)", static_cast<unsigned>(s_clicks_logged),
                     static_cast<unsigned>(s_detector.cfg.clicks_needed), CONFIG_PRESENCE_BOOT_CLICK_GAP_MS);
        }
    }
    if (fired) {
        ESP_LOGI(TAG, "Triple click on BOOT");
        if (s_on_triple_click) {
            s_on_triple_click();
        }
    }
}

esp_err_t button_handler_start(void (*on_triple_click)(void))
{
#if CONFIG_PRESENCE_BOOT_GPIO < 0
    (void)on_triple_click;
    ESP_LOGW(TAG, "No BOOT button configured: CONFIG mode reachable only after reflashing");
    return ESP_OK;
#else
    if (!GPIO_IS_VALID_GPIO(CONFIG_PRESENCE_BOOT_GPIO)) {
        ESP_LOGE(TAG, "GPIO%d is not a valid GPIO", CONFIG_PRESENCE_BOOT_GPIO);
        return ESP_ERR_INVALID_ARG;
    }
    s_on_triple_click = on_triple_click;

    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << CONFIG_PRESENCE_BOOT_GPIO;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = kPullUp ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        return err;
    }

    const click_detector_config_t cfg = {
        .debounce_ms = CONFIG_PRESENCE_BOOT_DEBOUNCE_MS,
        .gap_ms = CONFIG_PRESENCE_BOOT_CLICK_GAP_MS,
        .max_press_ms = CONFIG_PRESENCE_BOOT_CLICK_MAX_PRESS_MS,
        .clicks_needed = 3,
        .hold_ms = 0,
    };
    // A button already held now (it was not held at reset: that would have been the download mode) is ignored
    // until released.
    click_detector_init(&s_detector, &cfg, read_pressed(), now_ms());

    const esp_timer_create_args_t args = {
        .callback = poll_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "boot_button",
        .skip_unhandled_events = true,
    };
    err = esp_timer_create(&args, &s_timer);
    if (err == ESP_OK) {
        err = esp_timer_start_periodic(s_timer, CONFIG_PRESENCE_BOOT_POLL_MS * 1000);
    }
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "BOOT button on GPIO%d (active %s, pull-up %s): triple click switches NORMAL/CONFIG",
                 CONFIG_PRESENCE_BOOT_GPIO, kActiveLow ? "low" : "high",
                 kPullUp ? "on" : "off");
    }
    return err;
#endif
}
