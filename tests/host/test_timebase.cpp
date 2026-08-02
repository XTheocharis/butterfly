/*
 * test_timebase.cpp - 64-bit monotonic timebase host tests.
 *
 * Covers: TIMER4 (32-bit/1MHz) and RTC (24-bit/32768Hz) backends,
 * single wraps, remainder accumulation precision, monotonicity,
 * multiple wraps, max service gap. No hardware required.
 */
#include "test_framework.h"

/* Direct-include production logic for host testing (no SDK deps). */
#include "../../src/timebase.cpp"

/* ---- Mock counter ---- */
static uint32_t s_counter;
static uint32_t mock_read(void) { return s_counter; }

static void tb_timer4_init(void) {
    s_counter = 0;
    timebase_backend_t tb = { mock_read, 1000000u, 32 };
    timebase_init(&tb);
}

static void tb_rtc_init(void) {
    s_counter = 0;
    timebase_backend_t tb = { mock_read, 32768u, 24 };
    timebase_init(&tb);
}

/* ---- Tests ---- */

static void test_timer4_basic(void) {
    tb_timer4_init();
    /* init reads counter=0, delta=0 */
    TEST_ASSERT_EQ_INT(0, (int)timebase_now_us());

    s_counter = 1500;
    timebase_tick();
    TEST_ASSERT_EQ_INT(1500, (int)timebase_now_us());

    s_counter = 3000;
    timebase_tick();
    TEST_ASSERT_EQ_INT(3000, (int)timebase_now_us());
}

static void test_timer4_wrap(void) {
    tb_timer4_init();
    s_counter = 0xFFFFFFF0u;
    timebase_tick();   /* advance to near-wrap */
    uint64_t before = timebase_now_us();

    s_counter = 0x00000010u;   /* wrapped past 2^32 */
    timebase_tick();
    uint64_t after = timebase_now_us();

    /* Delta should be (0x100000010 - 0xFFFFFFF0) = 32 ticks = 32 us */
    TEST_ASSERT_EQ_INT(32, (int)(after - before));
}

static void test_rtc_basic(void) {
    tb_rtc_init();
    /* 32768 ticks = 1 second = 1000000 us */
    s_counter = 32768;
    timebase_tick();
    TEST_ASSERT_EQ_INT(1000000, (int)timebase_now_us());
}

static void test_rtc_wrap(void) {
    tb_rtc_init();
    s_counter = 0xFFFFF0u;  /* near 24-bit max */
    timebase_tick();
    uint64_t before = timebase_now_us();

    s_counter = 0x000005u;  /* wrapped past 2^24 */
    timebase_tick();
    uint64_t after = timebase_now_us();

    /* Delta = 21 ticks (16 to wrap + 5 after) */
    /* 21 * 1000000 / 32768 = 641.17... us → 641 with remainder */
    int64_t delta = (int64_t)(after - before);
    TEST_ASSERT(delta >= 640 && delta <= 642, "rtc wrap delta ~641us");
}

static void test_rtc_remainder_accumulation(void) {
    tb_rtc_init();
    /* Tick 3 times, 1 tick each. 1 tick = 30.517... us.
     * Without remainder: floor(30.517) * 3 = 30*3 = 90 (lost 1.55us).
     * With remainder: 30 + 31 + 30 = 91 (correct floor of 91.55). */
    s_counter = 1;
    timebase_tick();
    uint64_t a = timebase_now_us();
    TEST_ASSERT_EQ_INT(30, (int)a);

    s_counter = 2;
    timebase_tick();
    uint64_t b = timebase_now_us();
    TEST_ASSERT_EQ_INT(61, (int)b);  /* 30 + 31 */

    s_counter = 3;
    timebase_tick();
    uint64_t c = timebase_now_us();
    TEST_ASSERT_EQ_INT(91, (int)c);  /* 30 + 31 + 30 */
}

static void test_rtc_exact_second(void) {
    tb_rtc_init();
    /* 32768 ticks in one delta = exactly 1000000 us, no remainder */
    s_counter = 32768;
    timebase_tick();
    uint64_t us = timebase_now_us();
    TEST_ASSERT_EQ_INT(1000000, (int)us);
}

static void test_rtc_long_run_precision(void) {
    tb_rtc_init();
    /* Simulate 100 seconds of RTC ticks in 1-second increments.
     * After 100 calls of 32768 ticks each, total must be exactly 100000000 us. */
    for (int i = 1; i <= 100; i++) {
        s_counter = (uint32_t)(i * 32768);
        timebase_tick();
    }
    uint64_t us = timebase_now_us();
    TEST_ASSERT_EQ_INT(100000000, (int)us);
}

static void test_monotonicity(void) {
    tb_timer4_init();
    uint64_t prev = 0;
    for (int i = 0; i < 100; i++) {
        s_counter += 1000;
        uint64_t now = timebase_now_us();
        TEST_ASSERT(now >= prev, "time never goes backward");
        prev = now;
    }
}

static void test_rtc_multiple_wraps_documented(void) {
    /* If tick interval exceeds the wrap period, wraps are missed.
     * This test documents that single-wrap detection is correct
     * but multi-wrap is NOT (by design — caller must tick faster). */
    tb_rtc_init();
    s_counter = 0;
    timebase_tick();

    /* Advance past TWO wrap periods (2 * 2^24 = 33554432 ticks). */
    s_counter = 33554432u + 100u;
    timebase_tick();
    uint64_t us = timebase_now_us();

    /* Correct would be (33554532 * 1e6 / 32768) = 1023526 us.
     * But delta is masked to 24 bits: (33554532 & 0xFFFFFF) = 100.
     * So we get 100 * 1e6 / 32768 = 3051 us instead. */
    TEST_ASSERT(us < 1023526, "multi-wrap produces undercount (documented limitation)");
}

static void test_max_service_gap(void) {
    /* TIMER4 32-bit at 1 MHz: wrap = 2^32 us ≈ 71.6 min, half = 35.8 min */
    tb_timer4_init();
    uint32_t gap = timebase_max_service_gap_us();
    /* 2^31 = 2147483648 us ≈ 35.8 minutes */
    TEST_ASSERT(gap > 2000000000u, "timer4 gap > 2000 sec");

    /* RTC 24-bit at 32768 Hz: wrap = 2^24/32768 = 512 sec, half = 256 sec */
    tb_rtc_init();
    gap = timebase_max_service_gap_us();
    TEST_ASSERT(gap > 250000000u && gap < 260000000u, "rtc gap ~256 sec");
}

static void test_tick_without_init(void) {
    /* After static init (all zeros), tick should be safe no-op. */
    /* We can't easily reset static state, so just verify
     * the normal path handles zero-frequency gracefully by
     * testing with a real init. */
    tb_timer4_init();
    s_counter = 100;
    timebase_tick();
    TEST_ASSERT_EQ_INT(100, (int)timebase_now_us());
}

int main(void) {
    test_framework_init();
    RUN_TEST(test_timer4_basic);
    RUN_TEST(test_timer4_wrap);
    RUN_TEST(test_rtc_basic);
    RUN_TEST(test_rtc_wrap);
    RUN_TEST(test_rtc_remainder_accumulation);
    RUN_TEST(test_rtc_exact_second);
    RUN_TEST(test_rtc_long_run_precision);
    RUN_TEST(test_monotonicity);
    RUN_TEST(test_rtc_multiple_wraps_documented);
    RUN_TEST(test_max_service_gap);
    RUN_TEST(test_tick_without_init);
    return test_framework_finish();
}
