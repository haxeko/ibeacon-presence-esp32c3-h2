/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "json_util.h"

#include <stdio.h>
#include <string.h>

static bool is_continuation(uint8_t b)
{
    return (b & 0xC0) == 0x80;
}

size_t utf8_sequence_length(const uint8_t *s, size_t remaining)
{
    if (remaining == 0) {
        return 0;
    }
    const uint8_t b0 = s[0];
    if (b0 < 0x80) {
        return 1;
    }
    if (b0 >= 0xC2 && b0 <= 0xDF) {
        return (remaining >= 2 && is_continuation(s[1])) ? 2 : 0;
    }
    if (b0 >= 0xE0 && b0 <= 0xEF) {
        if (remaining < 3 || !is_continuation(s[1]) || !is_continuation(s[2])) {
            return 0;
        }
        if (b0 == 0xE0 && s[1] < 0xA0) {
            return 0; // overlong
        }
        if (b0 == 0xED && s[1] > 0x9F) {
            return 0; // UTF-16 surrogates
        }
        return 3;
    }
    if (b0 >= 0xF0 && b0 <= 0xF4) {
        if (remaining < 4 || !is_continuation(s[1]) || !is_continuation(s[2]) || !is_continuation(s[3])) {
            return 0;
        }
        if (b0 == 0xF0 && s[1] < 0x90) {
            return 0; // overlong
        }
        if (b0 == 0xF4 && s[1] > 0x8F) {
            return 0; // above U+10FFFF
        }
        return 4;
    }
    return 0; // continuation byte, 0xC0/0xC1, 0xF5-0xFF
}

static bool is_control(uint8_t b)
{
    return b < 0x20 || b == 0x7F;
}

bool utf8_is_valid_text(const char *s, size_t len)
{
    const uint8_t *p = reinterpret_cast<const uint8_t *>(s);
    size_t i = 0;
    while (i < len) {
        const size_t n = utf8_sequence_length(&p[i], len - i);
        if (n == 0 || (n == 1 && is_control(p[i]))) {
            return false;
        }
        i += n;
    }
    return true;
}

size_t utf8_sanitize(const uint8_t *in, size_t in_len, char *out, size_t out_size)
{
    if (out_size == 0) {
        return 0;
    }
    size_t o = 0;
    size_t i = 0;
    while (i < in_len) {
        size_t n = utf8_sequence_length(&in[i], in_len - i);
        const bool replace = n == 0 || (n == 1 && is_control(in[i]));
        const size_t out_len = replace ? 1 : n;
        if (o + out_len >= out_size) {
            break;
        }
        if (replace) {
            out[o++] = '?';
            i += 1;
        } else {
            memcpy(&out[o], &in[i], n);
            o += n;
            i += n;
        }
    }
    out[o] = '\0';
    return o;
}

size_t json_escape(const char *s, char *out, size_t out_size)
{
    if (out_size == 0) {
        return 0;
    }
    const uint8_t *p = reinterpret_cast<const uint8_t *>(s);
    const size_t len = strlen(s);
    size_t o = 0;
    size_t i = 0;
    while (i < len) {
        char esc[8];
        const char *piece = esc;
        size_t piece_len;
        size_t consumed = 1;
        const uint8_t b = p[i];
        if (b == '"' || b == '\\') {
            esc[0] = '\\';
            esc[1] = static_cast<char>(b);
            piece_len = 2;
        } else if (b == '\n') {
            memcpy(esc, "\\n", 2);
            piece_len = 2;
        } else if (is_control(b) || b == '<' || b == '>' || b == '&' || b == '\'') {
            snprintf(esc, sizeof(esc), "\\u%04x", b);
            piece_len = 6;
        } else if (b < 0x80) {
            esc[0] = static_cast<char>(b);
            piece_len = 1;
        } else {
            const size_t n = utf8_sequence_length(&p[i], len - i);
            if (n == 0) {
                esc[0] = '?';
                piece_len = 1;
            } else {
                piece = reinterpret_cast<const char *>(&p[i]);
                piece_len = n;
                consumed = n;
            }
        }
        if (o + piece_len >= out_size) {
            break;
        }
        memcpy(&out[o], piece, piece_len);
        o += piece_len;
        i += consumed;
    }
    out[o] = '\0';
    return o;
}
