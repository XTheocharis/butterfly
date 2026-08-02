/*
 * test_framework.c - implementation. See test_framework.h for the contract.
 *
 * Host-only. Built with the same host toolchain as the test files; never
 * pulled into firmware builds.
 */
#include "test_framework.h"

#include <stdio.h>
#include <stdlib.h>

#ifndef TEST_SEED
#define TEST_SEED 0xC0FFEEu
#endif

int      g_tests_run       = 0;
int      g_tests_failed    = 0;
int      g_test_jmp_valid  = 0;
jmp_buf  g_test_jmp;

static uint32_t g_prng_state = 0;

void test_framework_init(void) {
    uint32_t seed = (uint32_t)TEST_SEED;
    g_prng_state = seed ? seed : 0xC0FFEEu;
    g_tests_run = 0;
    g_tests_failed = 0;
    g_test_jmp_valid = 0;
    printf("[framework] deterministic seed = 0x%08X (%u)\n",
           seed, seed);
    fflush(stdout);
}

int test_framework_finish(void) {
    printf("[framework] %d run, %d failed\n", g_tests_run, g_tests_failed);
    fflush(stdout);
    return (g_tests_failed == 0) ? 0 : 1;
}

void test_framework_run__(const char *name, void (*fn)(void)) {
    ++g_tests_run;
    printf("[run] %s ... ", name);
    fflush(stdout);

    g_test_jmp_valid = 1;
    if (setjmp(g_test_jmp) == 0) {
        fn();
        printf("OK\n");
    } else {
        /* The assertion backend already printed details. */
        printf("    (test aborted via TEST_ASSERT/TEST_FAIL)\n");
    }
    g_test_jmp_valid = 0;
    fflush(stdout);
}

void test_framework_assert__(int cond, const char *msg,
                             const char *file, int line) {
    if (cond) return;
    ++g_tests_failed;
    printf("\n[FAIL] %s:%d: %s\n",
           file, line, msg ? msg : "(no message)");
    fflush(stdout);
    if (g_test_jmp_valid) longjmp(g_test_jmp, 1);
    fprintf(stderr, "abort: TEST_ASSERT used outside RUN_TEST\n");
    abort();
}

void test_framework_assert_eq_int__(long long expected, long long actual,
                                    const char *file, int line,
                                    const char *expr) {
    if (expected == actual) return;
    ++g_tests_failed;
    printf("\n[FAIL] %s:%d: %s (expected %lld, got %lld)\n",
           file, line, expr ? expr : "?", expected, actual);
    fflush(stdout);
    if (g_test_jmp_valid) longjmp(g_test_jmp, 1);
    fprintf(stderr, "abort: TEST_ASSERT used outside RUN_TEST\n");
    abort();
}

void test_framework_fail__(const char *msg, const char *file, int line) {
    ++g_tests_failed;
    printf("\n[FAIL] %s:%d: %s\n",
           file, line, msg ? msg : "(no message)");
    fflush(stdout);
    if (g_test_jmp_valid) longjmp(g_test_jmp, 1);
    fprintf(stderr, "abort: TEST_FAIL used outside RUN_TEST\n");
    abort();
}

void test_framework_srand(uint32_t seed) {
    /* xorshift32 cannot start at zero; pick a stable nonzero fallback. */
    g_prng_state = seed ? seed : 0xC0FFEEu;
}

uint32_t test_framework_rand(void) {
    uint32_t x = g_prng_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    g_prng_state = x;
    return x;
}
