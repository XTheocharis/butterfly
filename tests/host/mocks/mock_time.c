#include "mock_time.h"

static uint64_t g_now_us = 0;

void mock_time_reset(void) {
    g_now_us = 0;
}

void mock_time_set_us(uint64_t us) {
    g_now_us = us;
}

uint64_t mock_time_now_us(void) {
    return g_now_us;
}

void mock_time_advance_us(uint64_t delta_us) {
    g_now_us += delta_us;
}

uint64_t mock_time_now_ms(void) {
    return g_now_us / 1000ull;
}
