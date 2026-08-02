/*
 * test_apds9960.cpp - host tests for APDS-9960 optical/gesture sensor
 * and extended sensor table coverage (IDs 6-12).
 *
 * Covers:
 *   Register golden values, ENABLE bit decode, mutual exclusion invariant,
 *   mode transition FSM (OFF→optical, optical→gesture quiesce, busy reject),
 *   RGBC parsing, proximity range, FIFO decode (UP/DOWN/LEFT/RIGHT/Near),
 *   FIFO overflow detection, absent-device ID check, gesture-to-proto mapping,
 *   sensor table extension (IDs 6-12), rate clamping for env/APDS sensors.
 *
 * No SDK deps, no hardware. Includes driver .cpp and boardMotionEval.c directly.
 */
#include "test_framework.h"
#include "../../src/sensors/apds9960.cpp"
#include "../../src/boardMotionEval.c"
#include <string.h>

/* ====================================================================== */
/*  Register golden values                                                */
/* ====================================================================== */

static void test_golden_enable_is_0x27(void) {
	TEST_ASSERT_EQ_INT(0x27, (int)APDS9960_GOLDEN_ENABLE);
}

static void test_golden_atime_is_0xFF(void) {
	TEST_ASSERT_EQ_INT(0xFF, (int)APDS9960_GOLDEN_ATIME);
}

static void test_golden_wtime_is_0xFF(void) {
	TEST_ASSERT_EQ_INT(0xFF, (int)APDS9960_GOLDEN_WTIME);
}

static void test_golden_ppulse_is_0xC9(void) {
	TEST_ASSERT_EQ_INT(0xC9, (int)APDS9960_GOLDEN_PPULSE);
}

static void test_golden_control_is_0x20(void) {
	TEST_ASSERT_EQ_INT(0x20, (int)APDS9960_GOLDEN_CONTROL);
}

static void test_golden_config2_is_0x00(void) {
	TEST_ASSERT_EQ_INT(0x00, (int)APDS9960_GOLDEN_CONFIG2);
}

static void test_golden_gpenth_is_40(void) {
	TEST_ASSERT_EQ_INT(40, (int)APDS9960_GOLDEN_GPENTH);
}

static void test_golden_gexth_is_30(void) {
	TEST_ASSERT_EQ_INT(30, (int)APDS9960_GOLDEN_GEXTH);
}

static void test_golden_gfifoth_is_4(void) {
	TEST_ASSERT_EQ_INT(4, (int)APDS9960_GOLDEN_GFIFOTH);
}

static void test_id_value_is_0xAB(void) {
	TEST_ASSERT_EQ_INT(0xAB, (int)APDS9960_ID_VALUE);
}

static void test_i2c_addr_is_0x39(void) {
	TEST_ASSERT_EQ_INT(0x39, (int)APDS9960_ADDR);
}

/* ====================================================================== */
/*  ENABLE register bit decode                                            */
/* ====================================================================== */

static void test_enable_pon_bit(void) {
	TEST_ASSERT(APDS9960_ENABLE_PON == 0x01, "PON = bit 0");
}

static void test_enable_aen_bit(void) {
	TEST_ASSERT(APDS9960_ENABLE_AEN == 0x02, "AEN = bit 1");
}

static void test_enable_pen_bit(void) {
	TEST_ASSERT(APDS9960_ENABLE_PEN == 0x04, "PEN = bit 2");
}

static void test_enable_gen_bit(void) {
	TEST_ASSERT(APDS9960_ENABLE_GEN == 0x40, "GEN = bit 6");
}

static void test_golden_enable_decodes_to_optical(void) {
	TEST_ASSERT(apds9960_is_optical_enabled(APDS9960_GOLDEN_ENABLE),
		"0x27 = PON+AEN+PEN+PIEN = valid optical config");
}

static void test_golden_enable_is_not_gesture(void) {
	TEST_ASSERT(!apds9960_is_gesture_enabled(APDS9960_GOLDEN_ENABLE),
		"0x27 does not have GEN set");
}

static void test_gesture_enable_value_decodes_to_gesture(void) {
	TEST_ASSERT(apds9960_is_gesture_enabled(APDS9960_ENABLE_GESTURE_MODE),
		"gesture mode ENABLE = PON+WEN+PEN+GEN");
}

static void test_gesture_enable_is_not_optical(void) {
	TEST_ASSERT(!apds9960_is_optical_enabled(APDS9960_ENABLE_GESTURE_MODE),
		"gesture mode does not have AEN");
}

/* ====================================================================== */
/*  Mutual exclusion invariant                                            */
/* ====================================================================== */

static void test_golden_enable_no_mutual_exclusion_violation(void) {
	TEST_ASSERT(!apds9960_is_mutually_exclusive_violated(APDS9960_GOLDEN_ENABLE),
		"optical config must not violate mutual exclusion");
}

static void test_gesture_enable_no_mutual_exclusion_violation(void) {
	TEST_ASSERT(!apds9960_is_mutually_exclusive_violated(APDS9960_ENABLE_GESTURE_MODE),
		"gesture config must not violate mutual exclusion");
}

static void test_aen_plus_gen_detected_as_violation(void) {
	uint8_t bad = APDS9960_ENABLE_PON | APDS9960_ENABLE_AEN | APDS9960_ENABLE_GEN;
	TEST_ASSERT(apds9960_is_mutually_exclusive_violated(bad),
		"AEN+GEN simultaneously = violation");
}

static void test_quiesced_state_no_violation(void) {
	TEST_ASSERT(!apds9960_is_mutually_exclusive_violated(APDS9960_ENABLE_QUIESCED),
		"quiesced = PON+PEN only, no violation");
}

/* ====================================================================== */
/*  Mode transition FSM                                                   */
/* ====================================================================== */

static void test_mode_init_sets_off(void) {
	apds9960_mode_state_t st;
	apds9960_mode_init(&st);
	TEST_ASSERT_EQ_INT((int)APDS9960_MODE_OFF, (int)st.current);
	TEST_ASSERT(!st.transition_pending, "no pending transition after init");
}

static void test_off_to_optical_is_immediate(void) {
	apds9960_mode_state_t st;
	apds9960_mode_init(&st);
	apds9960_transition_result_t r = apds9960_request_mode(&st, APDS9960_MODE_OPTICAL);
	TEST_ASSERT_EQ_INT((int)APDS9960_TRANSITION_OK, (int)r);
	TEST_ASSERT_EQ_INT((int)APDS9960_MODE_OPTICAL, (int)st.current);
	TEST_ASSERT(!st.transition_pending, "OFF→optical is immediate");
}

static void test_off_to_gesture_is_immediate(void) {
	apds9960_mode_state_t st;
	apds9960_mode_init(&st);
	apds9960_transition_result_t r = apds9960_request_mode(&st, APDS9960_MODE_GESTURE);
	TEST_ASSERT_EQ_INT((int)APDS9960_TRANSITION_OK, (int)r);
	TEST_ASSERT_EQ_INT((int)APDS9960_MODE_GESTURE, (int)st.current);
}

static void test_optical_to_gesture_requires_quiesce(void) {
	apds9960_mode_state_t st;
	apds9960_mode_init(&st);
	apds9960_request_mode(&st, APDS9960_MODE_OPTICAL);

	apds9960_transition_result_t r = apds9960_request_mode(&st, APDS9960_MODE_GESTURE);
	TEST_ASSERT_EQ_INT((int)APDS9960_TRANSITION_OK, (int)r);
	TEST_ASSERT_EQ_INT((int)APDS9960_MODE_QUIESCING, (int)st.current);
	TEST_ASSERT(st.transition_pending, "transition pending");
	TEST_ASSERT_EQ_INT((int)APDS9960_MODE_GESTURE, (int)st.target);
}

static void test_gesture_to_optical_requires_quiesce(void) {
	apds9960_mode_state_t st;
	apds9960_mode_init(&st);
	apds9960_request_mode(&st, APDS9960_MODE_GESTURE);

	apds9960_transition_result_t r = apds9960_request_mode(&st, APDS9960_MODE_OPTICAL);
	TEST_ASSERT_EQ_INT((int)APDS9960_TRANSITION_OK, (int)r);
	TEST_ASSERT_EQ_INT((int)APDS9960_MODE_QUIESCING, (int)st.current);
}

static void test_quiesce_complete_advances_to_target(void) {
	apds9960_mode_state_t st;
	apds9960_mode_init(&st);
	apds9960_request_mode(&st, APDS9960_MODE_OPTICAL);
	apds9960_request_mode(&st, APDS9960_MODE_GESTURE);

	apds9960_mode_t result = apds9960_complete_quiesce(&st);
	TEST_ASSERT_EQ_INT((int)APDS9960_MODE_GESTURE, (int)result);
	TEST_ASSERT_EQ_INT((int)APDS9960_MODE_GESTURE, (int)st.current);
	TEST_ASSERT(!st.transition_pending, "transition completed");
}

static void test_same_mode_returns_same(void) {
	apds9960_mode_state_t st;
	apds9960_mode_init(&st);
	apds9960_request_mode(&st, APDS9960_MODE_OPTICAL);

	apds9960_transition_result_t r = apds9960_request_mode(&st, APDS9960_MODE_OPTICAL);
	TEST_ASSERT_EQ_INT((int)APDS9960_TRANSITION_SAME, (int)r);
}

static void test_concurrent_transition_rejected(void) {
	apds9960_mode_state_t st;
	apds9960_mode_init(&st);
	apds9960_request_mode(&st, APDS9960_MODE_OPTICAL);
	apds9960_request_mode(&st, APDS9960_MODE_GESTURE);

	apds9960_transition_result_t r = apds9960_request_mode(&st, APDS9960_MODE_OPTICAL);
	TEST_ASSERT_EQ_INT((int)APDS9960_TRANSITION_BUSY, (int)r);
}

/* ====================================================================== */
/*  Register config validation                                            */
/* ====================================================================== */

static void test_validate_optical_config_passes_with_golden(void) {
	apds9960_reg_val_t config[] = {
		{APDS9960_REG_ENABLE,  APDS9960_GOLDEN_ENABLE},
		{APDS9960_REG_ATIME,   APDS9960_GOLDEN_ATIME},
		{APDS9960_REG_WTIME,   APDS9960_GOLDEN_WTIME},
		{APDS9960_REG_PPULSE,  APDS9960_GOLDEN_PPULSE},
		{APDS9960_REG_CONTROL, APDS9960_GOLDEN_CONTROL},
		{APDS9960_REG_CONFIG2, APDS9960_GOLDEN_CONFIG2},
		{APDS9960_REG_GPENTH,  APDS9960_GOLDEN_GPENTH},
		{APDS9960_REG_GEXTH,   APDS9960_GOLDEN_GEXTH},
	};
	TEST_ASSERT(apds9960_validate_optical_config(config, 8),
		"all golden values should validate");
}

static void test_validate_optical_config_fails_on_wrong_enable(void) {
	apds9960_reg_val_t config[] = {
		{APDS9960_REG_ENABLE, 0x00},
	};
	TEST_ASSERT(!apds9960_validate_optical_config(config, 1),
		"wrong ENABLE should fail");
}

static void test_validate_optical_config_fails_on_wrong_control(void) {
	apds9960_reg_val_t config[] = {
		{APDS9960_REG_CONTROL, 0x00},
	};
	TEST_ASSERT(!apds9960_validate_optical_config(config, 1),
		"wrong CONTROL should fail");
}

static void test_validate_optical_config_fails_on_wrong_ppulse(void) {
	apds9960_reg_val_t config[] = {
		{APDS9960_REG_PPULSE, 0x00},
	};
	TEST_ASSERT(!apds9960_validate_optical_config(config, 1),
		"wrong PPULSE should fail");
}

static void test_validate_optical_config_rejects_unknown_register(void) {
	apds9960_reg_val_t config[] = {
		{APDS9960_REG_PERS, 0x00},
	};
	TEST_ASSERT(!apds9960_validate_optical_config(config, 1),
		"unknown register should be rejected");
}

static void test_validate_optical_config_rejects_wrong_config2(void) {
	apds9960_reg_val_t config[] = {
		{APDS9960_REG_CONFIG2, 0xFF},
	};
	TEST_ASSERT(!apds9960_validate_optical_config(config, 1),
		"wrong CONFIG2 should fail");
}

static void test_validate_optical_config_rejects_null(void) {
	TEST_ASSERT(!apds9960_validate_optical_config(NULL, 0),
		"null config should fail");
}

/* ====================================================================== */
/*  RGBC parsing                                                          */
/* ====================================================================== */

static void test_rgbc_parse_all_zero(void) {
	uint8_t burst[8] = {0};
	apds9960_optical_sample_t s;
	apds9960_parse_rgbc(burst, &s);
	TEST_ASSERT_EQ_INT(0, (int)s.clear);
	TEST_ASSERT_EQ_INT(0, (int)s.red);
	TEST_ASSERT_EQ_INT(0, (int)s.green);
	TEST_ASSERT_EQ_INT(0, (int)s.blue);
}

static void test_rgbc_parse_known_values(void) {
	uint8_t burst[8] = {
		0x34, 0x12,  /* C = 0x1234 */
		0x78, 0x56,  /* R = 0x5678 */
		0xBC, 0x9A,  /* G = 0x9ABC */
		0xFF, 0xFF,  /* B = 0xFFFF */
	};
	apds9960_optical_sample_t s;
	apds9960_parse_rgbc(burst, &s);
	TEST_ASSERT_EQ_INT(0x1234, (int)s.clear);
	TEST_ASSERT_EQ_INT(0x5678, (int)s.red);
	TEST_ASSERT_EQ_INT(0x9ABC, (int)s.green);
	TEST_ASSERT_EQ_INT(0xFFFF, (int)s.blue);
}

static void test_rgbc_parse_max_values(void) {
	uint8_t burst[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	apds9960_optical_sample_t s;
	apds9960_parse_rgbc(burst, &s);
	TEST_ASSERT_EQ_INT(0xFFFF, (int)s.clear);
	TEST_ASSERT_EQ_INT(0xFFFF, (int)s.red);
	TEST_ASSERT_EQ_INT(0xFFFF, (int)s.green);
	TEST_ASSERT_EQ_INT(0xFFFF, (int)s.blue);
}

static void test_rgbc_parse_little_endian_order(void) {
	uint8_t burst[8] = {0x01, 0x00, 0, 0, 0, 0, 0, 0};
	apds9960_optical_sample_t s;
	apds9960_parse_rgbc(burst, &s);
	TEST_ASSERT_EQ_INT(1, (int)s.clear);
}

/* ====================================================================== */
/*  Proximity range                                                       */
/* ====================================================================== */

static void test_proximity_zero(void) {
	TEST_ASSERT_EQ_INT(0, (int)apds9960_parse_proximity(0));
}

static void test_proximity_max(void) {
	TEST_ASSERT_EQ_INT(255, (int)apds9960_parse_proximity(0xFF));
}

static void test_proximity_midrange(void) {
	TEST_ASSERT_EQ_INT(128, (int)apds9960_parse_proximity(128));
}

/* ====================================================================== */
/*  Gesture FIFO decode                                                   */
/* ====================================================================== */

static void test_gesture_up_from_fifo(void) {
	/* U dominates: U >> D in all datasets. */
	uint8_t u[4] = {200, 200, 200, 200};
	uint8_t d[4] = {10, 10, 10, 10};
	uint8_t l[4] = {50, 50, 50, 50};
	uint8_t r[4] = {50, 50, 50, 50};

	apds9960_gesture_t g = apds9960_decode_fifo(u, d, l, r, 4);
	TEST_ASSERT_EQ_INT((int)APDS9960_GESTURE_UP, (int)g);
}

static void test_gesture_down_from_fifo(void) {
	uint8_t u[4] = {10, 10, 10, 10};
	uint8_t d[4] = {200, 200, 200, 200};
	uint8_t l[4] = {50, 50, 50, 50};
	uint8_t r[4] = {50, 50, 50, 50};

	apds9960_gesture_t g = apds9960_decode_fifo(u, d, l, r, 4);
	TEST_ASSERT_EQ_INT((int)APDS9960_GESTURE_DOWN, (int)g);
}

static void test_gesture_left_from_fifo(void) {
	uint8_t u[4] = {50, 50, 50, 50};
	uint8_t d[4] = {50, 50, 50, 50};
	uint8_t l[4] = {200, 200, 200, 200};
	uint8_t r[4] = {10, 10, 10, 10};

	apds9960_gesture_t g = apds9960_decode_fifo(u, d, l, r, 4);
	TEST_ASSERT_EQ_INT((int)APDS9960_GESTURE_LEFT, (int)g);
}

static void test_gesture_right_from_fifo(void) {
	uint8_t u[4] = {50, 50, 50, 50};
	uint8_t d[4] = {50, 50, 50, 50};
	uint8_t l[4] = {10, 10, 10, 10};
	uint8_t r[4] = {200, 200, 200, 200};

	apds9960_gesture_t g = apds9960_decode_fifo(u, d, l, r, 4);
	TEST_ASSERT_EQ_INT((int)APDS9960_GESTURE_RIGHT, (int)g);
}

static void test_gesture_none_from_noise(void) {
	uint8_t u[4] = {50, 50, 50, 50};
	uint8_t d[4] = {50, 50, 50, 50};
	uint8_t l[4] = {50, 50, 50, 50};
	uint8_t r[4] = {50, 50, 50, 50};

	apds9960_gesture_t g = apds9960_decode_fifo(u, d, l, r, 4);
	TEST_ASSERT_EQ_INT((int)APDS9960_GESTURE_NONE, (int)g);
}

static void test_gesture_none_from_empty_fifo(void) {
	apds9960_gesture_t g = apds9960_decode_fifo(NULL, NULL, NULL, NULL, 0);
	TEST_ASSERT_EQ_INT((int)APDS9960_GESTURE_NONE, (int)g);
}

static void test_gesture_none_from_overflow_count(void) {
	uint8_t u[1] = {200};
	uint8_t d[1] = {10};
	uint8_t l[1] = {50};
	uint8_t r[1] = {50};

	apds9960_gesture_t g = apds9960_decode_fifo(u, d, l, r, 99);
	TEST_ASSERT_EQ_INT((int)APDS9960_GESTURE_NONE, (int)g);
}

static void test_gesture_decoder_incremental_feed(void) {
	apds9960_gesture_decoder_t dec;
	apds9960_gesture_dec_init(&dec);

	apds9960_gesture_dec_feed(&dec, 200, 10, 50, 50);
	apds9960_gesture_dec_feed(&dec, 200, 10, 50, 50);

	apds9960_gesture_t g = apds9960_gesture_dec_result(&dec);
	TEST_ASSERT_EQ_INT((int)APDS9960_GESTURE_UP, (int)g);
	TEST_ASSERT_EQ_INT(2, (int)dec.datasets);
}

static void test_gesture_decoder_vertical_dominant_over_horizontal(void) {
	/* Both axes have signal but vertical is stronger. */
	apds9960_gesture_decoder_t dec;
	apds9960_gesture_dec_init(&dec);

	apds9960_gesture_dec_feed(&dec, 220, 20, 70, 40);
	apds9960_gesture_dec_feed(&dec, 220, 20, 70, 40);

	apds9960_gesture_t g = apds9960_gesture_dec_result(&dec);
	TEST_ASSERT_EQ_INT((int)APDS9960_GESTURE_UP, (int)g);
}

/* ====================================================================== */
/*  FIFO overflow detection                                               */
/* ====================================================================== */

static void test_fifo_not_overflow_at_depth(void) {
	TEST_ASSERT(!apds9960_fifo_overflow(32), "32 = exactly at depth");
}

static void test_fifo_overflow_above_depth(void) {
	TEST_ASSERT(apds9960_fifo_overflow(33), "33 = overflow");
}

static void test_fifo_not_overflow_zero(void) {
	TEST_ASSERT(!apds9960_fifo_overflow(0), "0 = empty, no overflow");
}

/* ====================================================================== */
/*  Absent device detection                                               */
/* ====================================================================== */

static void test_id_check_matches_0xAB(void) {
	TEST_ASSERT(apds9960_check_id(0xAB), "0xAB = APDS-9960 present");
}

static void test_id_check_rejects_wrong_id(void) {
	TEST_ASSERT(!apds9960_check_id(0x00), "0x00 = absent");
	TEST_ASSERT(!apds9960_check_id(0xFF), "0xFF = wrong device");
	TEST_ASSERT(!apds9960_check_id(0xBA), "0xBA = wrong device");
}

/* ====================================================================== */
/*  Gesture-to-proto mapping                                              */
/* ====================================================================== */

static void test_gesture_to_proto_up(void) {
	TEST_ASSERT_EQ_INT(1, (int)apds9960_gesture_to_proto(APDS9960_GESTURE_UP));
}

static void test_gesture_to_proto_down(void) {
	TEST_ASSERT_EQ_INT(2, (int)apds9960_gesture_to_proto(APDS9960_GESTURE_DOWN));
}

static void test_gesture_to_proto_left(void) {
	TEST_ASSERT_EQ_INT(3, (int)apds9960_gesture_to_proto(APDS9960_GESTURE_LEFT));
}

static void test_gesture_to_proto_right(void) {
	TEST_ASSERT_EQ_INT(4, (int)apds9960_gesture_to_proto(APDS9960_GESTURE_RIGHT));
}

static void test_gesture_to_proto_near(void) {
	TEST_ASSERT_EQ_INT(5, (int)apds9960_gesture_to_proto(APDS9960_GESTURE_NEAR));
}

static void test_gesture_to_proto_far(void) {
	TEST_ASSERT_EQ_INT(6, (int)apds9960_gesture_to_proto(APDS9960_GESTURE_FAR));
}

static void test_gesture_to_proto_none(void) {
	TEST_ASSERT_EQ_INT(0, (int)apds9960_gesture_to_proto(APDS9960_GESTURE_NONE));
}

/* ====================================================================== */
/*  GCONF1 GFIFOTH encoding                                               */
/* ====================================================================== */

static void test_gfifoth_encode_threshold_1(void) {
	TEST_ASSERT_EQ_INT(0x00, (int)apds9960_encode_gfifoth(1));
}

static void test_gfifoth_encode_threshold_4(void) {
	TEST_ASSERT_EQ_INT(0x40, (int)apds9960_encode_gfifoth(4));
}

static void test_gfifoth_encode_threshold_8(void) {
	TEST_ASSERT_EQ_INT(0x80, (int)apds9960_encode_gfifoth(8));
}

static void test_gfifoth_encode_threshold_16(void) {
	TEST_ASSERT_EQ_INT(0xC0, (int)apds9960_encode_gfifoth(16));
}

/* ====================================================================== */
/*  Extended sensor table (IDs 6-12)                                      */
/* ====================================================================== */

static void test_sensor_table_has_expected_count(void) {
	uint32_t count = 0;
	const board_motion_sensor_info_t *table =
		board_motion_get_sensor_table(&count);
	TEST_ASSERT(table != NULL, "table should not be NULL");
	TEST_ASSERT_EQ_INT(BOARD_MOTION_SENSOR_COUNT, (int)count);
	TEST_ASSERT_EQ_INT(14, (int)count);
}

static void test_lookup_finds_pressure_id_6(void) {
	const board_motion_sensor_info_t *info =
		board_motion_lookup_sensor(BOARD_MOTION_SENSOR_PRESSURE);
	TEST_ASSERT(info != NULL, "pressure (6) should exist");
	TEST_ASSERT(strcmp(info->unit, "pa") == 0, "unit is pa");
	TEST_ASSERT_EQ_INT(1, (int)info->value_count);
}

static void test_lookup_finds_bmp_temp_id_7(void) {
	TEST_ASSERT(board_motion_lookup_sensor(BOARD_MOTION_SENSOR_BMP_TEMP) != NULL,
		"bmp_temp (7) should exist");
}

static void test_lookup_finds_humidity_id_8(void) {
	const board_motion_sensor_info_t *info =
		board_motion_lookup_sensor(BOARD_MOTION_SENSOR_HUMIDITY);
	TEST_ASSERT(info != NULL, "humidity (8) should exist");
	TEST_ASSERT(strcmp(info->unit, "milli_percent_rh") == 0,
		"humidity unit");
}

static void test_lookup_finds_sht_temp_id_9(void) {
	TEST_ASSERT(board_motion_lookup_sensor(BOARD_MOTION_SENSOR_SHT_TEMP) != NULL,
		"sht_temp (9) should exist");
}

static void test_lookup_finds_color_id_10(void) {
	const board_motion_sensor_info_t *info =
		board_motion_lookup_sensor(BOARD_MOTION_SENSOR_COLOR);
	TEST_ASSERT(info != NULL, "color (10) should exist");
	TEST_ASSERT_EQ_INT(4, (int)info->value_count);
	TEST_ASSERT(strcmp(info->unit, "counts") == 0, "color unit");
}

static void test_lookup_finds_proximity_id_11(void) {
	const board_motion_sensor_info_t *info =
		board_motion_lookup_sensor(BOARD_MOTION_SENSOR_PROXIMITY);
	TEST_ASSERT(info != NULL, "proximity (11) should exist");
	TEST_ASSERT_EQ_INT(1, (int)info->value_count);
}

static void test_lookup_finds_gesture_id_12(void) {
	const board_motion_sensor_info_t *info =
		board_motion_lookup_sensor(BOARD_MOTION_SENSOR_GESTURE);
	TEST_ASSERT(info != NULL, "gesture (12) should exist");
	TEST_ASSERT(strcmp(info->unit, "enum") == 0, "gesture unit");
}

static void test_cursor_pagination_covers_all_sensors(void) {
	uint32_t cursor = 0;
	bool eof = false;
	uint32_t count = 0;

	while (!eof) {
		uint32_t next = 0;
		bool e = false;
		const board_motion_sensor_info_t *info =
			board_motion_get_by_cursor(cursor, &next, &e);
		if (info == NULL) break;
		count++;
		cursor = next;
		eof = e;
	}

	TEST_ASSERT_EQ_INT(14, (int)count);
}

static void test_rate_clamp_color_to_apds_max(void) {
	uint32_t actual = board_motion_clamp_rate(BOARD_MOTION_SENSOR_COLOR, 99999);
	TEST_ASSERT_EQ_INT((int)BOARD_MOTION_RATE_APDS_MAX_MHZ, (int)actual);
}

static void test_rate_clamp_proximity_to_apds_max(void) {
	uint32_t actual = board_motion_clamp_rate(BOARD_MOTION_SENSOR_PROXIMITY, 99999);
	TEST_ASSERT_EQ_INT((int)BOARD_MOTION_RATE_APDS_MAX_MHZ, (int)actual);
}

static void test_rate_clamp_pressure_to_bmp_max(void) {
	uint32_t actual = board_motion_clamp_rate(BOARD_MOTION_SENSOR_PRESSURE, 99999);
	TEST_ASSERT_EQ_INT((int)BOARD_MOTION_RATE_BMP_MAX_MHZ, (int)actual);
}

static void test_rate_clamp_humidity_to_sht_max(void) {
	uint32_t actual = board_motion_clamp_rate(BOARD_MOTION_SENSOR_HUMIDITY, 99999);
	TEST_ASSERT_EQ_INT((int)BOARD_MOTION_RATE_SHT_MAX_MHZ, (int)actual);
}

static void test_is_valid_sensor_rejects_unknown_ids(void) {
	TEST_ASSERT(!board_motion_is_valid_sensor(0), "id 0 not in table");
	TEST_ASSERT(!board_motion_is_valid_sensor(99), "id 99 not in table");
	TEST_ASSERT(!board_motion_is_valid_sensor(15), "id 15 not in table");
}

static void test_stream_configure_proximity(void) {
	board_motion_stream_state_t st;
	board_motion_stream_init(&st);

	uint32_t code = 0;
	uint32_t actual = board_motion_stream_configure(&st,
		BOARD_MOTION_SENSOR_PROXIMITY, 5000, &code);

	TEST_ASSERT_EQ_INT(0, (int)code);
	TEST_ASSERT_EQ_INT(5000, (int)actual);
	TEST_ASSERT(board_motion_stream_is_active(&st, BOARD_MOTION_SENSOR_PROXIMITY),
		"proximity stream active");
}

static void test_stream_configure_color_at_max(void) {
	board_motion_stream_state_t st;
	board_motion_stream_init(&st);

	uint32_t code = 0;
	uint32_t actual = board_motion_stream_configure(&st,
		BOARD_MOTION_SENSOR_COLOR, 99999, &code);

	TEST_ASSERT_EQ_INT(0, (int)code);
	TEST_ASSERT_EQ_INT((int)BOARD_MOTION_RATE_APDS_MAX_MHZ, (int)actual);
}

static void test_stream_stop_gesture(void) {
	board_motion_stream_state_t st;
	board_motion_stream_init(&st);

	uint32_t code = 0;
	board_motion_stream_configure(&st, BOARD_MOTION_SENSOR_GESTURE, 1000, &code);
	TEST_ASSERT(board_motion_stream_is_active(&st, BOARD_MOTION_SENSOR_GESTURE),
		"gesture stream active");

	board_motion_stream_stop(&st, BOARD_MOTION_SENSOR_GESTURE);
	TEST_ASSERT(!board_motion_stream_is_active(&st, BOARD_MOTION_SENSOR_GESTURE),
		"gesture stream stopped after stop");
}

/* ====================================================================== */
/*  main                                                                  */
/* ====================================================================== */

int main(void) {
	test_framework_init();

	/* Register golden values */
	RUN_TEST(test_golden_enable_is_0x27);
	RUN_TEST(test_golden_atime_is_0xFF);
	RUN_TEST(test_golden_wtime_is_0xFF);
	RUN_TEST(test_golden_ppulse_is_0xC9);
	RUN_TEST(test_golden_control_is_0x20);
	RUN_TEST(test_golden_config2_is_0x00);
	RUN_TEST(test_golden_gpenth_is_40);
	RUN_TEST(test_golden_gexth_is_30);
	RUN_TEST(test_golden_gfifoth_is_4);
	RUN_TEST(test_id_value_is_0xAB);
	RUN_TEST(test_i2c_addr_is_0x39);

	/* ENABLE bit decode */
	RUN_TEST(test_enable_pon_bit);
	RUN_TEST(test_enable_aen_bit);
	RUN_TEST(test_enable_pen_bit);
	RUN_TEST(test_enable_gen_bit);
	RUN_TEST(test_golden_enable_decodes_to_optical);
	RUN_TEST(test_golden_enable_is_not_gesture);
	RUN_TEST(test_gesture_enable_value_decodes_to_gesture);
	RUN_TEST(test_gesture_enable_is_not_optical);

	/* Mutual exclusion */
	RUN_TEST(test_golden_enable_no_mutual_exclusion_violation);
	RUN_TEST(test_gesture_enable_no_mutual_exclusion_violation);
	RUN_TEST(test_aen_plus_gen_detected_as_violation);
	RUN_TEST(test_quiesced_state_no_violation);

	/* Mode transition FSM */
	RUN_TEST(test_mode_init_sets_off);
	RUN_TEST(test_off_to_optical_is_immediate);
	RUN_TEST(test_off_to_gesture_is_immediate);
	RUN_TEST(test_optical_to_gesture_requires_quiesce);
	RUN_TEST(test_gesture_to_optical_requires_quiesce);
	RUN_TEST(test_quiesce_complete_advances_to_target);
	RUN_TEST(test_same_mode_returns_same);
	RUN_TEST(test_concurrent_transition_rejected);

	/* Register config validation */
	RUN_TEST(test_validate_optical_config_passes_with_golden);
	RUN_TEST(test_validate_optical_config_fails_on_wrong_enable);
	RUN_TEST(test_validate_optical_config_fails_on_wrong_control);
	RUN_TEST(test_validate_optical_config_fails_on_wrong_ppulse);
	RUN_TEST(test_validate_optical_config_rejects_unknown_register);
	RUN_TEST(test_validate_optical_config_rejects_wrong_config2);
	RUN_TEST(test_validate_optical_config_rejects_null);

	/* RGBC parsing */
	RUN_TEST(test_rgbc_parse_all_zero);
	RUN_TEST(test_rgbc_parse_known_values);
	RUN_TEST(test_rgbc_parse_max_values);
	RUN_TEST(test_rgbc_parse_little_endian_order);

	/* Proximity */
	RUN_TEST(test_proximity_zero);
	RUN_TEST(test_proximity_max);
	RUN_TEST(test_proximity_midrange);

	/* Gesture FIFO decode */
	RUN_TEST(test_gesture_up_from_fifo);
	RUN_TEST(test_gesture_down_from_fifo);
	RUN_TEST(test_gesture_left_from_fifo);
	RUN_TEST(test_gesture_right_from_fifo);
	RUN_TEST(test_gesture_none_from_noise);
	RUN_TEST(test_gesture_none_from_empty_fifo);
	RUN_TEST(test_gesture_none_from_overflow_count);
	RUN_TEST(test_gesture_decoder_incremental_feed);
	RUN_TEST(test_gesture_decoder_vertical_dominant_over_horizontal);

	/* FIFO overflow */
	RUN_TEST(test_fifo_not_overflow_at_depth);
	RUN_TEST(test_fifo_overflow_above_depth);
	RUN_TEST(test_fifo_not_overflow_zero);

	/* Absent device */
	RUN_TEST(test_id_check_matches_0xAB);
	RUN_TEST(test_id_check_rejects_wrong_id);

	/* Gesture-to-proto mapping */
	RUN_TEST(test_gesture_to_proto_up);
	RUN_TEST(test_gesture_to_proto_down);
	RUN_TEST(test_gesture_to_proto_left);
	RUN_TEST(test_gesture_to_proto_right);
	RUN_TEST(test_gesture_to_proto_near);
	RUN_TEST(test_gesture_to_proto_far);
	RUN_TEST(test_gesture_to_proto_none);

	/* GCONF1 GFIFOTH encoding */
	RUN_TEST(test_gfifoth_encode_threshold_1);
	RUN_TEST(test_gfifoth_encode_threshold_4);
	RUN_TEST(test_gfifoth_encode_threshold_8);
	RUN_TEST(test_gfifoth_encode_threshold_16);

	/* Extended sensor table (IDs 6-12) */
	RUN_TEST(test_sensor_table_has_expected_count);
	RUN_TEST(test_lookup_finds_pressure_id_6);
	RUN_TEST(test_lookup_finds_bmp_temp_id_7);
	RUN_TEST(test_lookup_finds_humidity_id_8);
	RUN_TEST(test_lookup_finds_sht_temp_id_9);
	RUN_TEST(test_lookup_finds_color_id_10);
	RUN_TEST(test_lookup_finds_proximity_id_11);
	RUN_TEST(test_lookup_finds_gesture_id_12);
	RUN_TEST(test_cursor_pagination_covers_all_sensors);
	RUN_TEST(test_rate_clamp_color_to_apds_max);
	RUN_TEST(test_rate_clamp_proximity_to_apds_max);
	RUN_TEST(test_rate_clamp_pressure_to_bmp_max);
	RUN_TEST(test_rate_clamp_humidity_to_sht_max);
	RUN_TEST(test_is_valid_sensor_rejects_unknown_ids);
	RUN_TEST(test_stream_configure_proximity);
	RUN_TEST(test_stream_configure_color_at_max);
	RUN_TEST(test_stream_stop_gesture);

	return test_framework_finish();
}
