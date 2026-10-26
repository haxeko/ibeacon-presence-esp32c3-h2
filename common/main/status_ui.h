/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// Two-chip version, Matter node (ESP32-H2, CONFIG_PRESENCE_ROLE_MATTER_UART): its BOOT button and status LED. On the
// ESP32-H2 SuperMini: BOOT on GPIO9, RGB LED (WS2812) on GPIO8 (Kconfig "Matter node button and LED").
//
//   double click on BOOT   pairing mode: the commissioning window opens (matter_device_open_pairing_window())
//   hold BOOT 5 s          factory reset of the Matter pairing (fabrics, Thread network, bridged devices; the beacon
//                          configuration is on the ESP32-C3 and stays). Past 5 s the LED turns red: release the
//                          button and the reset runs (GPIO9 is a strapping pin: the chip never restarts while it is
//                          held low). After the restart the LED blinks red three times once the device is checked
//                          to be no longer commissioned, then blinks blue: it waits to be paired.
//   LED                    blinks blue while a commissioning window is open (pairing mode), off otherwise.
//
// Polled every 10 ms by an esp_timer (click_detector.h), as the BOOT button of the ESP32-C6 / C3 versions.

#pragma once

#include <esp_err.h>

// After matter_device_start()
esp_err_t status_ui_start(void);

// A factory reset is about to run (button or the scanner's configuration page): the LED confirms it after the
// restart. Any task.
void status_ui_mark_factory_reset(void);

// The factory reset marked above could not be started: the mark is removed and the LED stays red for 2 s. Any task.
void status_ui_cancel_factory_reset(void);
