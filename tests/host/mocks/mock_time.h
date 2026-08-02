/*
 * mock_time.h - monotonic microsecond clock for code-under-test.
 *
 * Replaces SDK app_timer / TIMER3/4 capture reads with a deterministic
 * test-controllable counter. Used by Madgwick fusion, journal timestamps,
 * stream rate validation, and anywhere production code calls into a clock.
 */
#ifndef MOCK_TIME_H
#define MOCK_TIME_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void     mock_time_reset(void);
void     mock_time_set_us(uint64_t us);
uint64_t mock_time_now_us(void);
void     mock_time_advance_us(uint64_t delta_us);

/* Convenience: time in milliseconds. */
uint64_t mock_time_now_ms(void);

#ifdef __cplusplus
}
#endif
#endif /* MOCK_TIME_H */
