/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// CONFIG mode web server (esp_http_server, port 80): the page (web/index.html, embedded) and a small JSON API.
//
//   GET  /                  page (Vietnamese, works offline, no CDN)
//   GET  /api/status        mode, idle countdown, heap, scanner and storage state
//   GET  /api/scan          beacons heard (scan_cache)
//   POST /api/scan          {"action":"start"|"stop"|"clear"}
//   GET  /api/config        the five slots, scan settings, limits
//   POST /api/config        validate (presence_config_validate) and save (config_store_save)
//   POST /api/restart       restart into NORMAL once the response is sent
//   POST /api/keepalive     the user is still on the page
//   POST /api/format        {"confirm":"FORMAT"}: erase the app_cfg partition (only when it is unusable/unreadable)
//
// Only requests to the SoftAP address are served (403 otherwise) and every POST must be application/json (415),
// see http_guard.h. Request bodies are limited (2 KB for the configuration, 256 bytes otherwise; 413 beyond).
// Every value is checked again here whatever the page did; text sent back is JSON-escaped (json_escape()).
// Requests with ?auto=1 (periodic polls) do not count as interaction for the idle timeout.

#pragma once

#include <esp_err.h>

typedef struct {
    const char *ssid;
    esp_err_t nvs_main_err; // nvs_flash_init() of the Matter partition, shown on the page
} web_server_info_t;

esp_err_t web_server_start(const web_server_info_t *info);
