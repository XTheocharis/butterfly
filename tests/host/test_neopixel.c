/*
 * test_neopixel.c - verify WS2812B PWM duty cycle encoding for CLUE NeoPixel.
 *
 * Checks duty counts, polarity bit, GRB byte/bit ordering, sequence length,
 * reset duration, and a full-frame round-trip.
 *
 * No SDK dependencies — includes neopixel_encode.h directly.
 */
#include "test_framework.h"
#include "../../src/neopixel_encode.h"

/* ---- Duty cycle values ---- */

static void test_duty_constants(void)
{
	/* Polarity bit (15) must be set on all data values. */
	TEST_ASSERT(NEOPIXEL_DUTY_0 & 0x8000, "0-bit polarity bit set");
	TEST_ASSERT(NEOPIXEL_DUTY_1 & 0x8000, "1-bit polarity bit set");
	TEST_ASSERT(NEOPIXEL_DUTY_RESET & 0x8000, "reset polarity bit set");

	/* Compare values match the verified CLUE BSP pattern. */
	TEST_ASSERT_EQ_INT(5,  (int)(NEOPIXEL_DUTY_0 & 0x7FFF));
	TEST_ASSERT_EQ_INT(13, (int)(NEOPIXEL_DUTY_1 & 0x7FFF));
	TEST_ASSERT_EQ_INT(0,  (int)(NEOPIXEL_DUTY_RESET & 0x7FFF));

	/* Duty values are distinct. */
	TEST_ASSERT(NEOPIXEL_DUTY_0 != NEOPIXEL_DUTY_1, "0 and 1 duty differ");
	TEST_ASSERT(NEOPIXEL_DUTY_RESET != NEOPIXEL_DUTY_0, "reset differs from 0");
}

/* ---- Pulse width timing ---- */

static void test_pulse_widths(void)
{
	double period_us = (double)NEOPIXEL_COUNTERTOP / (double)NEOPIXEL_CLK_HZ * 1e6;

	/* Period should be 1.25 us at 16 MHz / COUNTERTOP 20. */
	TEST_ASSERT(period_us > 1.20 && period_us < 1.30,
	            "period within 1.2-1.3 us");

	double t0h = (NEOPIXEL_DUTY_0 & 0x7FFF) * period_us / NEOPIXEL_COUNTERTOP;
	double t1h = (NEOPIXEL_DUTY_1 & 0x7FFF) * period_us / NEOPIXEL_COUNTERTOP;

	/* WS2812B T0H: 0.25-0.55 us. */
	TEST_ASSERT(t0h >= 0.25 && t0h <= 0.55, "T0H within WS2812 spec");

	/* WS2812B T1H: 0.65-0.95 us. */
	TEST_ASSERT(t1h >= 0.65 && t1h <= 0.95, "T1H within WS2812 spec");
}

/* ---- Reset duration ---- */

static void test_reset_duration(void)
{
	double period_us = (double)NEOPIXEL_COUNTERTOP / (double)NEOPIXEL_CLK_HZ * 1e6;
	double reset_us = (double)NEOPIXEL_RESET_PERIODS_MIN * period_us;

	/* WS2812B reset: >= 50 us. Plan requires >= 80 us (conservative). */
	TEST_ASSERT(reset_us >= 80.0, "reset >= 80 us");
	TEST_ASSERT(reset_us >= 50.0, "reset >= 50 us (datasheet)");
}

/* ---- Sequence length ---- */

static void test_sequence_length(void)
{
	TEST_ASSERT_EQ_INT(24, (int)NEOPIXEL_BITS_PER_PIXEL);
	TEST_ASSERT_EQ_INT(88, (int)NEOPIXEL_SEQ_LEN);
}

/* ---- GRB byte ordering ---- */

static void test_grb_order_green_first(void)
{
	uint16_t buf[NEOPIXEL_BITS_PER_PIXEL];

	/* Pure green: G=0xFF, R=0x00, B=0x00.
	 * GRB order => first 8 bits are all 1, next 16 bits are all 0. */
	neopixel_encode_grb(buf, 0, 255, 0);
	for (int i = 0; i < 8; i++) {
		TEST_ASSERT_EQ_INT((int)NEOPIXEL_DUTY_1, (int)buf[i]);
	}
	for (int i = 8; i < 24; i++) {
		TEST_ASSERT_EQ_INT((int)NEOPIXEL_DUTY_0, (int)buf[i]);
	}
}

static void test_grb_order_red_middle(void)
{
	uint16_t buf[NEOPIXEL_BITS_PER_PIXEL];

	/* Pure red: G=0x00, R=0xFF, B=0x00.
	 * GRB order => first 8 bits 0, next 8 bits 1, last 8 bits 0. */
	neopixel_encode_grb(buf, 255, 0, 0);
	for (int i = 0; i < 8; i++) {
		TEST_ASSERT_EQ_INT((int)NEOPIXEL_DUTY_0, (int)buf[i]);
	}
	for (int i = 8; i < 16; i++) {
		TEST_ASSERT_EQ_INT((int)NEOPIXEL_DUTY_1, (int)buf[i]);
	}
	for (int i = 16; i < 24; i++) {
		TEST_ASSERT_EQ_INT((int)NEOPIXEL_DUTY_0, (int)buf[i]);
	}
}

static void test_grb_order_blue_last(void)
{
	uint16_t buf[NEOPIXEL_BITS_PER_PIXEL];

	/* Pure blue: G=0x00, R=0x00, B=0xFF.
	 * GRB order => first 16 bits 0, last 8 bits 1. */
	neopixel_encode_grb(buf, 0, 0, 255);
	for (int i = 0; i < 16; i++) {
		TEST_ASSERT_EQ_INT((int)NEOPIXEL_DUTY_0, (int)buf[i]);
	}
	for (int i = 16; i < 24; i++) {
		TEST_ASSERT_EQ_INT((int)NEOPIXEL_DUTY_1, (int)buf[i]);
	}
}

/* ---- MSB-first within each byte ---- */

static void test_msb_first(void)
{
	uint16_t buf[NEOPIXEL_BITS_PER_PIXEL];

	/* G=0x80 (only MSB set), R=0, B=0.
	 * MSB-first: first bit is 1, next 7 are 0. */
	neopixel_encode_grb(buf, 0, 0x80, 0);
	TEST_ASSERT_EQ_INT((int)NEOPIXEL_DUTY_1, (int)buf[0]);
	for (int i = 1; i < 8; i++) {
		TEST_ASSERT_EQ_INT((int)NEOPIXEL_DUTY_0, (int)buf[i]);
	}
}

static void test_msb_first_lsb(void)
{
	uint16_t buf[NEOPIXEL_BITS_PER_PIXEL];

	/* G=0x01 (only LSB set), R=0, B=0.
	 * MSB-first: first 7 bits 0, 8th bit (index 7) is 1. */
	neopixel_encode_grb(buf, 0, 0x01, 0);
	for (int i = 0; i < 7; i++) {
		TEST_ASSERT_EQ_INT((int)NEOPIXEL_DUTY_0, (int)buf[i]);
	}
	TEST_ASSERT_EQ_INT((int)NEOPIXEL_DUTY_1, (int)buf[7]);
}

/* ---- Reset encoding ---- */

static void test_reset_encoding(void)
{
	uint16_t rbuf[8];
	uint32_t n = neopixel_encode_reset(rbuf, 8);
	TEST_ASSERT_EQ_INT(8, (int)n);
	for (uint32_t i = 0; i < n; i++) {
		TEST_ASSERT_EQ_INT((int)NEOPIXEL_DUTY_RESET, (int)rbuf[i]);
	}
}

/* ---- Full frame round-trip ---- */

static void test_full_frame(void)
{
	uint16_t buf[NEOPIXEL_SEQ_LEN];
	uint32_t data_len = neopixel_encode_grb(buf, 0xAB, 0xCD, 0xEF);
	TEST_ASSERT_EQ_INT(24, (int)data_len);

	uint32_t reset_len = neopixel_encode_reset(buf + NEOPIXEL_BITS_PER_PIXEL,
	                                            NEOPIXEL_RESET_PERIODS_MIN);
	TEST_ASSERT_EQ_INT((int)NEOPIXEL_RESET_PERIODS_MIN, (int)reset_len);

	/* Total sequence length matches NEOPIXEL_SEQ_LEN. */
	TEST_ASSERT_EQ_INT((int)NEOPIXEL_SEQ_LEN,
	                   (int)(data_len + reset_len));

	/* Verify green byte 0xCD (11001101) in MSB-first order. */
	TEST_ASSERT_EQ_INT((int)NEOPIXEL_DUTY_1, (int)buf[0]);  /* bit 7 = 1 */
	TEST_ASSERT_EQ_INT((int)NEOPIXEL_DUTY_1, (int)buf[1]);  /* bit 6 = 1 */
	TEST_ASSERT_EQ_INT((int)NEOPIXEL_DUTY_0, (int)buf[2]);  /* bit 5 = 0 */
	TEST_ASSERT_EQ_INT((int)NEOPIXEL_DUTY_0, (int)buf[3]);  /* bit 4 = 0 */
	TEST_ASSERT_EQ_INT((int)NEOPIXEL_DUTY_1, (int)buf[4]);  /* bit 3 = 1 */
	TEST_ASSERT_EQ_INT((int)NEOPIXEL_DUTY_1, (int)buf[5]);  /* bit 2 = 1 */
	TEST_ASSERT_EQ_INT((int)NEOPIXEL_DUTY_0, (int)buf[6]);  /* bit 1 = 0 */
	TEST_ASSERT_EQ_INT((int)NEOPIXEL_DUTY_1, (int)buf[7]);  /* bit 0 = 1 */

	/* All reset entries at the tail. */
	for (uint32_t i = data_len; i < NEOPIXEL_SEQ_LEN; i++) {
		TEST_ASSERT_EQ_INT((int)NEOPIXEL_DUTY_RESET, (int)buf[i]);
	}
}

/* ---- Black frame (all zeros) ---- */

static void test_black_frame(void)
{
	uint16_t buf[NEOPIXEL_BITS_PER_PIXEL];
	neopixel_encode_grb(buf, 0, 0, 0);
	for (uint32_t i = 0; i < NEOPIXEL_BITS_PER_PIXEL; i++) {
		TEST_ASSERT_EQ_INT((int)NEOPIXEL_DUTY_0, (int)buf[i]);
	}
}

int main(void)
{
	test_framework_init();
	RUN_TEST(test_duty_constants);
	RUN_TEST(test_pulse_widths);
	RUN_TEST(test_reset_duration);
	RUN_TEST(test_sequence_length);
	RUN_TEST(test_grb_order_green_first);
	RUN_TEST(test_grb_order_red_middle);
	RUN_TEST(test_grb_order_blue_last);
	RUN_TEST(test_msb_first);
	RUN_TEST(test_msb_first_lsb);
	RUN_TEST(test_reset_encoding);
	RUN_TEST(test_full_frame);
	RUN_TEST(test_black_frame);
	return test_framework_finish();
}
