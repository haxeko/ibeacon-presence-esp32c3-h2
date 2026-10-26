/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// Periodic diagnostics log (CONFIG_PRESENCE_DIAG_PERIOD_SEC), for the long-run hardware tests:
//   heap free / minimum / largest block
//   scanner state, pause reasons, reports per second, starts, failures, unexpected stops, watchdog restarts
//   NORMAL: per slot state, last RSSI, age, packets; queue drops; Thread role and MAC / MLE counters, or Wi-Fi
//           signal and disconnections
//   CONFIG: beacons in the list, SoftAP stations, idle countdown

#pragma once

#include <esp_err.h>

#include "app_mode.h"

esp_err_t diag_start(app_mode_t mode);
