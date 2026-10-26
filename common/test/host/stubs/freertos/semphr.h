// semphr.h for the host tests: single-threaded. A FreeRTOS mutex is not recursive, so taking it twice from the
// same task would deadlock on the device: the stub aborts instead, which fails the test.
#pragma once
#include <stdio.h>
#include <stdlib.h>

#include "FreeRTOS.h"
typedef int *SemaphoreHandle_t;
static int s_host_mutex_depth;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void) { return &s_host_mutex_depth; }
static inline int xSemaphoreTake(SemaphoreHandle_t m, TickType_t)
{
    if (*m != 0) {
        printf("mutex taken twice: deadlock on the device\n");
        abort();
    }
    (*m)++;
    return 1;
}
static inline int xSemaphoreGive(SemaphoreHandle_t m) { (*m)--; return 1; }
