// esp_log.h for the host tests: messages go to stdout when HOST_TEST_VERBOSE is set
#pragma once
#include <stdio.h>
#include <stdlib.h>

#define HOST_LOG(level, tag, fmt, ...)                                                                                 \
    do {                                                                                                               \
        if (getenv("HOST_TEST_VERBOSE")) {                                                                             \
            printf("%s (%s) " fmt "\n", level, tag, ##__VA_ARGS__);                                                    \
        }                                                                                                              \
    } while (0)
#define ESP_LOGE(tag, fmt, ...) HOST_LOG("E", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGW(tag, fmt, ...) HOST_LOG("W", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGI(tag, fmt, ...) HOST_LOG("I", tag, fmt, ##__VA_ARGS__)
#define ESP_LOGD(tag, fmt, ...) HOST_LOG("D", tag, fmt, ##__VA_ARGS__)
