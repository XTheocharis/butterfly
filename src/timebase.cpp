/*
 * timebase.cpp - 64-bit monotonic microsecond timebase.
 *
 * Converts a 32-bit hardware counter (TIMER4 or RTC) to a 64-bit
 * microsecond value via remainder accumulation. The tick() function
 * must be called at least once per counter wrap period.
 *
 * now_us()/now_ms() are READ-ONLY: they return the most recent value
 * derived from tick(). The caller is responsible for ticking at a
 * rate fast enough to not miss wraps (see timebase_max_service_gap_us).
 * Earlier versions refreshed the value inside now_us(); that hid the
 * tick requirement and made the cost of a read non-obvious.
 *
 * No SDK deps. The counter read is a backend function pointer.
 */
#include "timebase.h"

static timebase_backend_t s_backend;
static uint32_t s_lastCounter;
static uint32_t s_remainder;     /* carried fractional us from last conversion */
static uint64_t s_totalUs;
static uint32_t s_wrapMask;      /* (1 << bits) - 1 */
static uint32_t s_maxGapUs;      /* max interval before a wrap is missed */

static inline uint32_t readMasked(void) {
    return s_backend.read() & s_wrapMask;
}

static void update(void) {
    uint32_t now = readMasked();
    uint32_t delta = (now - s_lastCounter) & s_wrapMask;
    s_lastCounter = now;

    /* delta_ticks * 1e6 / freq, with remainder carry to avoid truncation. */
    uint64_t scaled = (uint64_t)delta * 1000000ull + s_remainder;
    uint64_t deltaUs = scaled / s_backend.frequency;
    s_remainder = (uint32_t)(scaled % s_backend.frequency);
    s_totalUs += deltaUs;
}

void timebase_init(const timebase_backend_t *backend) {
    s_backend = *backend;
    s_wrapMask = (backend->bits >= 32) ? 0xFFFFFFFFu
                                       : ((1u << backend->bits) - 1u);
    s_lastCounter = readMasked();
    s_remainder = 0;
    s_totalUs = 0;

    /* Max gap = wrap period / 2, in microseconds. */
    uint64_t wrapUs = ((uint64_t)1 << backend->bits) * 1000000ull
                      / backend->frequency;
    s_maxGapUs = (uint32_t)(wrapUs / 2);
}

void timebase_tick(void) {
    if (s_backend.read) update();
}

uint64_t timebase_now_us(void) {
	/* Read-only: caller-driven tick() is the only path that advances
	 * s_totalUs. Returning without refreshing keeps read cost constant
	 * and forces tick rate to be a conscious caller decision. */
	return s_totalUs;
}

uint64_t timebase_now_ms(void) {
	return s_totalUs / 1000ull;
}

uint32_t timebase_max_service_gap_us(void) {
    return s_maxGapUs;
}
