/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// BOOT button: polled every CONFIG_PRESENCE_BOOT_POLL_MS by an esp_timer, no GPIO interrupt. The triple click
// (click_detector.h) calls on_triple_click from the esp_timer task: the callback must only hand the work over
// (app_mode_on_triple_click() queues a restart request), never block or do the work itself.
// There is deliberately no factory reset on this button: a factory reset is done from the Matter controller or
// the console ("matter esp factoryreset").

#pragma once

#include <esp_err.h>

esp_err_t button_handler_start(void (*on_triple_click)(void));
