/*
 * timebase.h - 64-bit monotonic microsecond timebase for CLUE dual-runtime.
 *
 * Raw-WHAD wraps TIMER4 CC5 (32-bit, 1 MHz). BLE-HID uses RTC1/app_timer.
 * RTC2 provides early-startup time. All three feed one 64-bit accumulator
 * via remainder-accumulation conversion that never moves backward or
 * accumulates systematic truncation.
 *
 * Wrap detection is serviced from timebase_tick(), called by a guaranteed
 * periodic scheduler callback — NOT from incidental now_us() calls.
 *
 * Pure logic: the counter read is a pluggable backend function pointer.
 * No SDK deps. Compiles on host for unit testing.
 *
 * LFRC error bound: CLUE has no LF crystal. LFRC runs at ~31.250 kHz
 * (target 32.768 kHz, ±3100 ppm). RTC-derived microseconds are suitable
 * for UI/scheduling but NOT raw-radio symbol timing. Keep radio deadlines
 * on TIMER4/HFCLK (±0 ppm at 1 MHz from external 32 MHz crystal).
 */
#ifndef TIMEBASE_H
#define TIMEBASE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t (*read)(void);  /* read raw hardware counter */
    uint32_t frequency;      /* counter Hz (1 000 000 or ~32 768) */
    uint8_t  bits;           /* counter width: 24 (RTC) or 32 (TIMER) */
} timebase_backend_t;

void     timebase_init(const timebase_backend_t *backend);
void     timebase_tick(void);          /* periodic scheduler callback */
uint64_t timebase_now_us(void);        /* 64-bit monotonic microseconds */
uint64_t timebase_now_ms(void);        /* convenience milliseconds */

/* Maximum tick interval (us) before a single wrap could be missed. */
uint32_t timebase_max_service_gap_us(void);

#ifdef __cplusplus
}
#endif
#endif /* TIMEBASE_H */
