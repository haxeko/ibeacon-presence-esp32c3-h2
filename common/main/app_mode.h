/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// NORMAL / CONFIG mode, chosen once at boot. Switching is always a controlled restart:
//   NORMAL --triple click--> request CONFIG in RTC memory, restart --> CONFIG (Matter not started, its data and
//   network untouched) --"restart to Matter" on the page, triple click, or no interaction for
//   CONFIG_PRESENCE_CONFIG_IDLE_TIMEOUT_MIN--> restart --> NORMAL.
// The request is valid for one software restart only (app_mode_logic.h): power-on always starts NORMAL.

#pragma once

#include <stdint.h>

#include <esp_err.h>

typedef enum {
    APP_MODE_NORMAL = 0,
    APP_MODE_CONFIG = 1,
} app_mode_t;

// Reads and clears the CONFIG request. Called first thing in app_main.
app_mode_t app_mode_init(void);
app_mode_t app_mode_get(void);
const char *app_mode_name(app_mode_t mode);
const char *app_mode_reset_reason_name(void);

// Starts the task that carries out restarts and, in CONFIG mode, the idle timeout.
esp_err_t app_mode_start(void);

// Restarts into target after delay_ms (lets an HTTP response or a log line go out). Non-blocking, callable from
// any task or timer callback; the first request wins.
void app_mode_request_restart(app_mode_t target, uint32_t delay_ms, const char *reason);

// BOOT triple click: NORMAL -> CONFIG, CONFIG -> NORMAL
void app_mode_on_triple_click(void);

// CONFIG mode: the user did something on the page, the idle timeout starts over
void app_mode_note_activity(void);
uint32_t app_mode_idle_remaining_s(void);
