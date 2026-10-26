/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

// UTF-8 checks and JSON string escaping for the configuration web page. No ESP-IDF dependency (host tests).
//
// Every text that can come from outside (beacon names received over the air, slot labels typed in the page) goes
// through these before it is stored or sent: invalid UTF-8 and control characters never reach the page, and
// '<', '>', '&', '"', '\'' are escaped as \u00XX, so a name cannot close the JSON string or inject markup even if
// the page were to insert it as HTML (it does not: it uses textContent).

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Length (1-4) of the well-formed UTF-8 sequence at s (RFC 3629: no overlong forms, no surrogates, at most
// U+10FFFF), 0 if the bytes there are not one.
size_t utf8_sequence_length(const uint8_t *s, size_t remaining);

// Well-formed UTF-8 without control characters (below U+0020, and U+007F)
bool utf8_is_valid_text(const char *s, size_t len);

// Copies in to out as well-formed UTF-8 text: every byte of an invalid sequence and every control character
// becomes '?'. out is always NUL-terminated and never ends in the middle of a sequence. Returns strlen(out).
size_t utf8_sanitize(const uint8_t *in, size_t in_len, char *out, size_t out_size);

// Escapes s (NUL-terminated) for use inside a JSON string, without the quotes. Invalid UTF-8 bytes become '?'.
// Stops before an escape that would not fit, never splitting one. out is always NUL-terminated (out_size > 0).
// Returns strlen(out).
size_t json_escape(const char *s, char *out, size_t out_size);
