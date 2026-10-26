/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// Checks the configuration web server applies to every request, against pages of other sites open in the browser
// of a phone joined to the SoftAP (it may still reach the Internet over mobile data). No ESP-IDF dependency (host
// tests).
//
// - DNS rebinding: a site can make its own name resolve to 192.168.4.1 and then use the API as if it were the
//   page of the device; its requests still carry that name in Host. Only the SoftAP address is served.
// - Cross-site requests: without asking first (CORS preflight) a site may only send text/plain, form data or no
//   body type. Requiring application/json on every POST makes the browser ask with OPTIONS, which the server
//   does not answer, so the POST is never sent.

#pragma once

#include <stdbool.h>

// Host header value (may be NULL) equal to ap_ip, optionally followed by ":80"
bool http_host_allowed(const char *host, const char *ap_ip);

// Content-Type header value (may be NULL) is application/json, with or without parameters (charset)
bool http_content_type_is_json(const char *content_type);
