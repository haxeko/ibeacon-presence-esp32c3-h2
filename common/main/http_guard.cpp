/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "http_guard.h"

#include <string.h>
#include <strings.h>

static const char *skip_blanks(const char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

bool http_host_allowed(const char *host, const char *ap_ip)
{
    if (host == nullptr || ap_ip == nullptr || ap_ip[0] == '\0') {
        return false;
    }
    host = skip_blanks(host);
    const size_t n = strlen(ap_ip);
    if (strncmp(host, ap_ip, n) != 0) {
        return false;
    }
    return host[n] == '\0' || strcmp(&host[n], ":80") == 0;
}

bool http_content_type_is_json(const char *content_type)
{
    static const char kJson[] = "application/json";
    static const size_t kLen = sizeof(kJson) - 1;
    if (content_type == nullptr) {
        return false;
    }
    content_type = skip_blanks(content_type);
    if (strncasecmp(content_type, kJson, kLen) != 0) {
        return false;
    }
    const char next = content_type[kLen];
    return next == '\0' || next == ';' || next == ' ' || next == '\t';
}
