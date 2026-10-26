// Minimal test harness for the host tests (no framework to install): CHECK() counts and reports failures,
// test_summary() prints the result and gives the exit status.
#pragma once

#include <stdio.h>
#include <string.h>

static int g_checks;
static int g_failures;
static const char *g_current_test = "";

#define CHECK(cond)                                                                                                    \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (!(cond)) {                                                                                                 \
            g_failures++;                                                                                              \
            printf("  FAIL [%s] %s:%d: %s\n", g_current_test, __FILE__, __LINE__, #cond);                              \
        }                                                                                                              \
    } while (0)

#define CHECK_STR(a, b)                                                                                                \
    do {                                                                                                               \
        g_checks++;                                                                                                    \
        if (strcmp((a), (b)) != 0) {                                                                                   \
            g_failures++;                                                                                              \
            printf("  FAIL [%s] %s:%d: \"%s\" != \"%s\"\n", g_current_test, __FILE__, __LINE__, (a), (b));             \
        }                                                                                                              \
    } while (0)

#define RUN(fn)                                                                                                        \
    do {                                                                                                               \
        g_current_test = #fn;                                                                                          \
        fn();                                                                                                          \
    } while (0)

static inline int test_summary(const char *suite)
{
    printf("%-18s %4d checks, %d failed\n", suite, g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
