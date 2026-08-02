#include "test_framework.h"

#include "boardMotionEval.h"
#include "../src/motion/rotation_gesture_eval.h"

#include <string.h>

#include "../../src/boardMotionEval.c"
#include "../../src/motion/rotation_gesture_eval.c"

/* ---- Sensor table tests ---- */

static void sensor_table_has_expected_entry_count(void)
{
	uint32_t count = 0;
	const board_motion_sensor_info_t *table =
		board_motion_get_sensor_table(&count);

	TEST_ASSERT(table != NULL, "table should not be NULL");
	TEST_ASSERT_EQ_INT(BOARD_MOTION_SENSOR_COUNT, count);
}

static void sensor_table_includes_expected_ids(void)
{
	TEST_ASSERT(board_motion_lookup_sensor(BOARD_MOTION_SENSOR_ACCEL) != NULL,
		"accel should be in table");
	TEST_ASSERT(board_motion_lookup_sensor(BOARD_MOTION_SENSOR_GYRO) != NULL,
		"gyro should be in table");
	TEST_ASSERT(board_motion_lookup_sensor(BOARD_MOTION_SENSOR_MAG) != NULL,
		"mag should be in table");
	TEST_ASSERT(board_motion_lookup_sensor(BOARD_MOTION_SENSOR_QUATERNION) != NULL,
		"quaternion should be in table");
	TEST_ASSERT(board_motion_lookup_sensor(BOARD_MOTION_SENSOR_ORIENTATION) != NULL,
		"orientation should be in table");
	TEST_ASSERT(board_motion_lookup_sensor(BOARD_MOTION_SENSOR_AIR_MOUSE) != NULL,
		"air_mouse should be in table");
}

static void lookup_rejects_invalid_ids(void)
{
	TEST_ASSERT(board_motion_lookup_sensor(0) == NULL, "id 0 invalid");
	TEST_ASSERT(board_motion_lookup_sensor(99) == NULL, "id 99 invalid");
	TEST_ASSERT(board_motion_lookup_sensor(15) == NULL, "id 15 invalid");
	TEST_ASSERT(board_motion_lookup_sensor(16) == NULL, "id 16 invalid");
}

static void accel_descriptor_has_three_values_and_imu_max_rate(void)
{
	const board_motion_sensor_info_t *info =
		board_motion_lookup_sensor(BOARD_MOTION_SENSOR_ACCEL);

	TEST_ASSERT(info != NULL, "accel exists");
	TEST_ASSERT_EQ_INT(3, info->value_count);
	TEST_ASSERT_EQ_INT(BOARD_MOTION_RATE_IMU_MAX_MHZ, info->max_rate_millihz);
	TEST_ASSERT(strcmp(info->name, "acceleration") == 0, "name matches");
	TEST_ASSERT(strcmp(info->unit, "mg") == 0, "unit matches");
}

static void mag_descriptor_has_mag_max_rate(void)
{
	const board_motion_sensor_info_t *info =
		board_motion_lookup_sensor(BOARD_MOTION_SENSOR_MAG);

	TEST_ASSERT(info != NULL, "mag exists");
	TEST_ASSERT_EQ_INT(BOARD_MOTION_RATE_MAG_MAX_MHZ, info->max_rate_millihz);
	TEST_ASSERT_EQ_INT(3, info->value_count);
}

static void air_mouse_descriptor_has_four_values_and_60hz_max(void)
{
	const board_motion_sensor_info_t *info =
		board_motion_lookup_sensor(BOARD_MOTION_SENSOR_AIR_MOUSE);

	TEST_ASSERT(info != NULL, "air_mouse exists");
	TEST_ASSERT_EQ_INT(4, info->value_count);
	TEST_ASSERT_EQ_INT(BOARD_MOTION_RATE_AIR_MOUSE_MAX_MHZ, info->max_rate_millihz);
}

static void quaternion_has_four_components(void)
{
	const board_motion_sensor_info_t *info =
		board_motion_lookup_sensor(BOARD_MOTION_SENSOR_QUATERNION);

	TEST_ASSERT(info != NULL, "quaternion exists");
	TEST_ASSERT_EQ_INT(4, info->value_count);
}

/* ---- Cursor pagination tests ---- */

static void cursor_zero_returns_first_sensor(void)
{
	uint32_t next = 0;
	bool eof = true;
	const board_motion_sensor_info_t *info =
		board_motion_get_by_cursor(0, &next, &eof);

	TEST_ASSERT(info != NULL, "cursor 0 valid");
	TEST_ASSERT_EQ_INT(BOARD_MOTION_SENSOR_ACCEL, info->sensor_id);
	TEST_ASSERT_EQ_INT(1, next);
	TEST_ASSERT(!eof, "should not be eof at cursor 0");
}

static void cursor_last_index_sets_eof(void)
{
	uint32_t next = 0;
	bool eof = false;
	const board_motion_sensor_info_t *info =
		board_motion_get_by_cursor(BOARD_MOTION_SENSOR_COUNT - 1,
		                           &next, &eof);

	TEST_ASSERT(info != NULL, "last cursor valid");
	TEST_ASSERT(eof, "should be eof at last cursor");
}

static void cursor_beyond_table_returns_null_and_eof(void)
{
	uint32_t next = 999;
	bool eof = false;
	const board_motion_sensor_info_t *info =
		board_motion_get_by_cursor(BOARD_MOTION_SENSOR_COUNT,
		                           &next, &eof);

	TEST_ASSERT(info == NULL, "out-of-range cursor NULL");
	TEST_ASSERT(eof, "eof on out-of-range");
}

static void full_cursor_walk_visits_all_sensors(void)
{
	uint32_t visited = 0;
	uint32_t cursor = 0;
	bool eof = false;

	while (!eof) {
		const board_motion_sensor_info_t *info =
			board_motion_get_by_cursor(cursor, &cursor, &eof);
		if (info == NULL) {
			break;
		}
		visited++;
	}

	TEST_ASSERT_EQ_INT(BOARD_MOTION_SENSOR_COUNT, visited);
}

/* ---- Rate clamping tests ---- */

static void clamp_imu_rate_caps_at_104000(void)
{
	uint32_t actual = board_motion_clamp_rate(
		BOARD_MOTION_SENSOR_ACCEL, 200000);
	TEST_ASSERT_EQ_INT(BOARD_MOTION_RATE_IMU_MAX_MHZ, actual);
}

static void clamp_mag_rate_caps_at_40000(void)
{
	uint32_t actual = board_motion_clamp_rate(
		BOARD_MOTION_SENSOR_MAG, 200000);
	TEST_ASSERT_EQ_INT(BOARD_MOTION_RATE_MAG_MAX_MHZ, actual);
}

static void clamp_air_mouse_rate_caps_at_60000(void)
{
	uint32_t actual = board_motion_clamp_rate(
		BOARD_MOTION_SENSOR_AIR_MOUSE, 200000);
	TEST_ASSERT_EQ_INT(BOARD_MOTION_RATE_AIR_MOUSE_MAX_MHZ, actual);
}

static void clamp_below_minimum_raises_to_minimum(void)
{
	uint32_t actual = board_motion_clamp_rate(
		BOARD_MOTION_SENSOR_ACCEL, 1);
	TEST_ASSERT_EQ_INT(BOARD_MOTION_RATE_MIN_MHZ, actual);
}

static void clamp_zero_returns_zero(void)
{
	uint32_t actual = board_motion_clamp_rate(
		BOARD_MOTION_SENSOR_ACCEL, 0);
	TEST_ASSERT_EQ_INT(0, actual);
}

static void clamp_invalid_sensor_returns_zero(void)
{
	uint32_t actual = board_motion_clamp_rate(99, 50000);
	TEST_ASSERT_EQ_INT(0, actual);
}

static void clamp_exact_rate_passes_through(void)
{
	uint32_t actual = board_motion_clamp_rate(
		BOARD_MOTION_SENSOR_GYRO, 50000);
	TEST_ASSERT_EQ_INT(50000, actual);
}

/* ---- Stream state tests ---- */

static void stream_configure_activates_slot(void)
{
	board_motion_stream_state_t st;
	board_motion_stream_init(&st);

	uint32_t code = 0;
	uint32_t actual = board_motion_stream_configure(
		&st, BOARD_MOTION_SENSOR_ACCEL, 50000, &code);

	TEST_ASSERT_EQ_INT(0, code);
	TEST_ASSERT_EQ_INT(50000, actual);
	TEST_ASSERT(board_motion_stream_is_active(&st, BOARD_MOTION_SENSOR_ACCEL),
		"accel stream active");
	TEST_ASSERT_EQ_INT(50000,
		board_motion_stream_get_rate(&st, BOARD_MOTION_SENSOR_ACCEL));
}

static void stream_configure_clamps_rate(void)
{
	board_motion_stream_state_t st;
	board_motion_stream_init(&st);

	uint32_t code = 0;
	uint32_t actual = board_motion_stream_configure(
		&st, BOARD_MOTION_SENSOR_ACCEL, 999999, &code);

	TEST_ASSERT_EQ_INT(0, code);
	TEST_ASSERT_EQ_INT(BOARD_MOTION_RATE_IMU_MAX_MHZ, actual);
}

static void stream_stop_is_idempotent(void)
{
	board_motion_stream_state_t st;
	board_motion_stream_init(&st);

	uint32_t code = 0;
	board_motion_stream_configure(&st, BOARD_MOTION_SENSOR_GYRO, 104000, &code);

	uint32_t rc1 = board_motion_stream_stop(&st, BOARD_MOTION_SENSOR_GYRO);
	TEST_ASSERT_EQ_INT(0, rc1);
	TEST_ASSERT(!board_motion_stream_is_active(&st, BOARD_MOTION_SENSOR_GYRO),
		"gyro inactive after stop");

	uint32_t rc2 = board_motion_stream_stop(&st, BOARD_MOTION_SENSOR_GYRO);
	TEST_ASSERT_EQ_INT(0, rc2);
}

static void stream_reconfigure_updates_rate(void)
{
	board_motion_stream_state_t st;
	board_motion_stream_init(&st);

	uint32_t code = 0;
	board_motion_stream_configure(&st, BOARD_MOTION_SENSOR_ACCEL, 10000, &code);
	TEST_ASSERT_EQ_INT(10000,
		board_motion_stream_get_rate(&st, BOARD_MOTION_SENSOR_ACCEL));

	uint32_t actual = board_motion_stream_configure(
		&st, BOARD_MOTION_SENSOR_ACCEL, 50000, &code);
	TEST_ASSERT_EQ_INT(50000, actual);
	TEST_ASSERT_EQ_INT(50000,
		board_motion_stream_get_rate(&st, BOARD_MOTION_SENSOR_ACCEL));
}

static void stream_rejects_invalid_sensor(void)
{
	board_motion_stream_state_t st;
	board_motion_stream_init(&st);

	uint32_t code = 99;
	uint32_t actual = board_motion_stream_configure(
		&st, 99, 50000, &code);

	TEST_ASSERT_EQ_INT(0, actual);
	TEST_ASSERT(code != 0, "should fail for invalid sensor");
}

static void stream_fills_all_slots_then_busy(void)
{
	board_motion_stream_state_t st;
	board_motion_stream_init(&st);

	uint32_t code = 0;
	uint32_t sensors[] = {
		BOARD_MOTION_SENSOR_ACCEL,
		BOARD_MOTION_SENSOR_GYRO,
		BOARD_MOTION_SENSOR_MAG,
		BOARD_MOTION_SENSOR_QUATERNION,
	};

	for (uint32_t i = 0; i < BOARD_MOTION_MAX_STREAMS; i++) {
		uint32_t actual = board_motion_stream_configure(
			&st, sensors[i], 10000, &code);
		TEST_ASSERT(actual > 0, "slot should allocate");
		TEST_ASSERT_EQ_INT(0, code);
	}

	uint32_t actual = board_motion_stream_configure(
		&st, BOARD_MOTION_SENSOR_AIR_MOUSE, 60000, &code);
	TEST_ASSERT_EQ_INT(0, actual);
	TEST_ASSERT(code != 0, "should fail when table full");
}

/* ---- Calibration ownership tests ---- */

static void calib_start_imu_succeeds(void)
{
	board_motion_calib_state_t st;
	board_motion_calib_init(&st);

	uint32_t rc = board_motion_calib_start(&st, BOARD_MOTION_SENSOR_ACCEL);
	TEST_ASSERT_EQ_INT(0, rc);
	TEST_ASSERT(board_motion_calib_is_busy(&st), "should be busy");
}

static void calib_start_gyro_also_targets_imu(void)
{
	board_motion_calib_state_t st;
	board_motion_calib_init(&st);

	uint32_t rc = board_motion_calib_start(&st, BOARD_MOTION_SENSOR_GYRO);
	TEST_ASSERT_EQ_INT(0, rc);
	TEST_ASSERT(board_motion_calib_is_busy(&st), "gyro starts IMU calib");
}

static void calib_start_mag_succeeds(void)
{
	board_motion_calib_state_t st;
	board_motion_calib_init(&st);

	uint32_t rc = board_motion_calib_start(&st, BOARD_MOTION_SENSOR_MAG);
	TEST_ASSERT_EQ_INT(0, rc);
	TEST_ASSERT(board_motion_calib_is_busy(&st), "should be busy");
}

static void calib_concurrent_rejected_with_busy(void)
{
	board_motion_calib_state_t st;
	board_motion_calib_init(&st);

	uint32_t rc1 = board_motion_calib_start(&st, BOARD_MOTION_SENSOR_ACCEL);
	TEST_ASSERT_EQ_INT(0, rc1);

	uint32_t rc2 = board_motion_calib_start(&st, BOARD_MOTION_SENSOR_MAG);
	TEST_ASSERT(rc2 != 0, "second calib should be BUSY");
}

static void calib_complete_clears_owner(void)
{
	board_motion_calib_state_t st;
	board_motion_calib_init(&st);

	board_motion_calib_start(&st, BOARD_MOTION_SENSOR_ACCEL);
	TEST_ASSERT(board_motion_calib_is_busy(&st), "busy after start");

	board_motion_calib_complete(&st);
	TEST_ASSERT(!board_motion_calib_is_busy(&st), "idle after complete");

	uint32_t rc = board_motion_calib_start(&st, BOARD_MOTION_SENSOR_MAG);
	TEST_ASSERT_EQ_INT(0, rc);
}

static void calib_rejects_invalid_sensor(void)
{
	board_motion_calib_state_t st;
	board_motion_calib_init(&st);

	uint32_t rc = board_motion_calib_start(&st, 99);
	TEST_ASSERT(rc != 0, "should reject id 99");

	rc = board_motion_calib_start(&st, BOARD_MOTION_SENSOR_QUATERNION);
	TEST_ASSERT(rc != 0, "should reject quaternion");

	rc = board_motion_calib_start(&st, BOARD_MOTION_SENSOR_AIR_MOUSE);
	TEST_ASSERT(rc != 0, "should reject air_mouse");
}

static void calib_idle_when_not_started(void)
{
	board_motion_calib_state_t st;
	board_motion_calib_init(&st);
	TEST_ASSERT(!board_motion_calib_is_busy(&st), "idle at init");
}

/* ---- Validity helper ---- */

static void is_valid_sensor_for_known_and_unknown(void)
{
	TEST_ASSERT(board_motion_is_valid_sensor(BOARD_MOTION_SENSOR_ACCEL),
		"1 valid");
	TEST_ASSERT(board_motion_is_valid_sensor(BOARD_MOTION_SENSOR_AIR_MOUSE),
		"14 valid");
	TEST_ASSERT(!board_motion_is_valid_sensor(0), "0 invalid");
	TEST_ASSERT(!board_motion_is_valid_sensor(15), "15 invalid");
}

/* ====================================================================== */
/*  Rotation gesture FSM                                                  */
/* ====================================================================== */

/* Helper: emit one tick and return the event. */
static rotg_event_t rotg_step(rotg_state_machine_t *sm,
                              bool a, bool b,
                              float gx, float gy, float gz,
                              uint64_t now_us)
{
	float g[3] = {gx, gy, gz};
	rotg_event_t evt = ROTG_EVENT_NONE;
	rotg_tick(sm, a, b, g, now_us, &evt);
	return evt;
}

static void rotg_starts_in_idle(void)
{
	rotg_state_machine_t sm;
	rotg_init(&sm);
	TEST_ASSERT_EQ_INT((int)ROTG_STATE_IDLE, (int)rotg_get_state(&sm));
}

static void rotg_release_in_idle_stays_idle(void)
{
	rotg_state_machine_t sm;
	rotg_init(&sm);
	rotg_event_t e = rotg_step(&sm, false, false, 0.0f, 0.0f, 1.0f, 1000);
	TEST_ASSERT_EQ_INT((int)ROTG_EVENT_NONE, (int)e);
	TEST_ASSERT_EQ_INT((int)ROTG_STATE_IDLE, (int)rotg_get_state(&sm));
}

static void rotg_short_chord_does_not_arm(void)
{
	rotg_state_machine_t sm;
	rotg_init(&sm);

	/* Hold chord 1.0s (under the 1.5s threshold). */
	uint64_t t = 0;
	rotg_event_t e = ROTG_EVENT_NONE;
	for (uint32_t i = 0; i < 100; i++) {
		t += 10000; /* 10ms per tick */
		e = rotg_step(&sm, true, true, 0.0f, 0.0f, 1.0f, t);
	}
	TEST_ASSERT_EQ_INT((int)ROTG_EVENT_NONE, (int)e);
	TEST_ASSERT_EQ_INT((int)ROTG_STATE_ARMING, (int)rotg_get_state(&sm));

	/* Release before threshold — back to IDLE. */
	e = rotg_step(&sm, false, false, 0.0f, 0.0f, 1.0f, t + 10000);
	TEST_ASSERT_EQ_INT((int)ROTG_EVENT_NONE, (int)e);
	TEST_ASSERT_EQ_INT((int)ROTG_STATE_IDLE, (int)rotg_get_state(&sm));
}

static void rotg_full_chord_arms_and_captures_reference(void)
{
	rotg_state_machine_t sm;
	rotg_init(&sm);

	uint64_t t = 0;
	rotg_event_t e = ROTG_EVENT_NONE;
	/* Hold chord past 1.5s threshold with +Z up. */
	for (uint32_t i = 0; i < 200 && e == ROTG_EVENT_NONE; i++) {
		t += 10000;
		e = rotg_step(&sm, true, true, 0.0f, 0.0f, 1.0f, t);
	}
	TEST_ASSERT_EQ_INT((int)ROTG_EVENT_ARMED, (int)e);
	TEST_ASSERT_EQ_INT((int)ROTG_STATE_ARMED, (int)rotg_get_state(&sm));
	TEST_ASSERT(sm.has_ref, "reference captured");
	/* Reference should be +Z (gravity_now passed in). */
	TEST_ASSERT(sm.gravity_ref[2] > 0.99f, "ref is +Z");
}

static void rotg_complete_flip_returns_and_confirms(void)
{
	rotg_state_machine_t sm;
	rotg_init(&sm);

	uint64_t t = 0;
	rotg_event_t e = ROTG_EVENT_NONE;
	/* Arm. */
	for (uint32_t i = 0; i < 200 && e == ROTG_EVENT_NONE; i++) {
		t += 10000;
		e = rotg_step(&sm, true, true, 0.0f, 0.0f, 1.0f, t);
	}
	TEST_ASSERT_EQ_INT((int)ROTG_EVENT_ARMED, (int)e);

	/* Flip device (gravity → -Z, dot ≤ -0.75) sustained 150ms (15 ticks). */
	e = ROTG_EVENT_NONE;
	for (uint32_t i = 0; i < 30 && e == ROTG_EVENT_NONE; i++) {
		t += 10000;
		e = rotg_step(&sm, true, true, 0.0f, 0.0f, -1.0f, t);
	}
	TEST_ASSERT_EQ_INT((int)ROTG_EVENT_INVERTED, (int)e);
	TEST_ASSERT_EQ_INT((int)ROTG_STATE_INVERTED, (int)rotg_get_state(&sm));

	/* Hold inverted briefly. */
	for (uint32_t i = 0; i < 5; i++) {
		t += 10000;
		e = rotg_step(&sm, true, true, 0.0f, 0.0f, -1.0f, t);
		TEST_ASSERT_EQ_INT((int)ROTG_EVENT_NONE, (int)e);
	}

	/* Return (gravity → +Z, dot ≥ +0.75) sustained 150ms. */
	e = ROTG_EVENT_NONE;
	for (uint32_t i = 0; i < 30 && e == ROTG_EVENT_NONE; i++) {
		t += 10000;
		e = rotg_step(&sm, true, true, 0.0f, 0.0f, 1.0f, t);
	}

	/* Now release chord — should enter CONFIRM_PENDING. */
	e = ROTG_EVENT_NONE;
	t += 10000;
	e = rotg_step(&sm, false, false, 0.0f, 0.0f, 1.0f, t);
	TEST_ASSERT_EQ_INT((int)ROTG_EVENT_CONFIRM_REQ, (int)e);
	TEST_ASSERT_EQ_INT((int)ROTG_STATE_CONFIRM_PENDING, (int)rotg_get_state(&sm));

	/* Press A → confirm. */
	e = ROTG_EVENT_NONE;
	t += 10000;
	e = rotg_step(&sm, true, false, 0.0f, 0.0f, 1.0f, t);
	TEST_ASSERT_EQ_INT((int)ROTG_EVENT_SWITCH_CONFIRM, (int)e);
	TEST_ASSERT_EQ_INT((int)ROTG_STATE_IDLE, (int)rotg_get_state(&sm));
}

static void rotg_b_in_confirm_cancels(void)
{
	rotg_state_machine_t sm;
	rotg_init(&sm);

	uint64_t t = 0;
	rotg_event_t e = ROTG_EVENT_NONE;
	for (uint32_t i = 0; i < 200 && e == ROTG_EVENT_NONE; i++) {
		t += 10000;
		e = rotg_step(&sm, true, true, 0.0f, 0.0f, 1.0f, t);
	}
	e = ROTG_EVENT_NONE;
	for (uint32_t i = 0; i < 30 && e == ROTG_EVENT_NONE; i++) {
		t += 10000;
		e = rotg_step(&sm, true, true, 0.0f, 0.0f, -1.0f, t);
	}
	e = ROTG_EVENT_NONE;
	for (uint32_t i = 0; i < 30 && e == ROTG_EVENT_NONE; i++) {
		t += 10000;
		e = rotg_step(&sm, true, true, 0.0f, 0.0f, 1.0f, t);
	}
	t += 10000;
	e = rotg_step(&sm, false, false, 0.0f, 0.0f, 1.0f, t);
	TEST_ASSERT_EQ_INT((int)ROTG_EVENT_CONFIRM_REQ, (int)e);

	/* Press B alone → cancel. */
	e = ROTG_EVENT_NONE;
	t += 10000;
	e = rotg_step(&sm, false, true, 0.0f, 0.0f, 1.0f, t);
	TEST_ASSERT_EQ_INT((int)ROTG_EVENT_CANCELLED, (int)e);
	TEST_ASSERT_EQ_INT((int)ROTG_STATE_IDLE, (int)rotg_get_state(&sm));
}

static void rotg_confirm_window_timeout_cancels(void)
{
	rotg_state_machine_t sm;
	rotg_init(&sm);

	uint64_t t = 0;
	rotg_event_t e = ROTG_EVENT_NONE;
	for (uint32_t i = 0; i < 200 && e == ROTG_EVENT_NONE; i++) {
		t += 10000;
		e = rotg_step(&sm, true, true, 0.0f, 0.0f, 1.0f, t);
	}
	e = ROTG_EVENT_NONE;
	for (uint32_t i = 0; i < 30 && e == ROTG_EVENT_NONE; i++) {
		t += 10000;
		e = rotg_step(&sm, true, true, 0.0f, 0.0f, -1.0f, t);
	}
	e = ROTG_EVENT_NONE;
	for (uint32_t i = 0; i < 30 && e == ROTG_EVENT_NONE; i++) {
		t += 10000;
		e = rotg_step(&sm, true, true, 0.0f, 0.0f, 1.0f, t);
	}
	t += 10000;
	e = rotg_step(&sm, false, false, 0.0f, 0.0f, 1.0f, t);
	TEST_ASSERT_EQ_INT((int)ROTG_EVENT_CONFIRM_REQ, (int)e);

	/* Wait past 5s confirm timeout without pressing. */
	e = ROTG_EVENT_NONE;
	for (uint32_t i = 0; i < 600 && e == ROTG_EVENT_NONE; i++) {
		t += 10000;
		e = rotg_step(&sm, false, false, 0.0f, 0.0f, 1.0f, t);
	}
	TEST_ASSERT_EQ_INT((int)ROTG_EVENT_CANCELLED, (int)e);
	TEST_ASSERT_EQ_INT((int)ROTG_STATE_IDLE, (int)rotg_get_state(&sm));
}

static void rotg_armed_window_expires_silently(void)
{
	rotg_state_machine_t sm;
	rotg_init(&sm);

	uint64_t t = 0;
	rotg_event_t e = ROTG_EVENT_NONE;
	for (uint32_t i = 0; i < 200 && e == ROTG_EVENT_NONE; i++) {
		t += 10000;
		e = rotg_step(&sm, true, true, 0.0f, 0.0f, 1.0f, t);
	}
	TEST_ASSERT_EQ_INT((int)ROTG_EVENT_ARMED, (int)e);
	TEST_ASSERT_EQ_INT((int)ROTG_STATE_ARMED, (int)rotg_get_state(&sm));

	/* Hold the chord past the 4s armed window. The FSM should exit
	 * ARMED (and may immediately re-enter ARMING because the chord
	 * is still held). We detect expiry by watching for a transition
	 * OUT of ARMED. */
	bool saw_idle = false;
	rotg_state_t prev = ROTG_STATE_ARMED;
	for (uint32_t i = 0; i < 500; i++) {
		t += 10000;
		(void)rotg_step(&sm, true, true, 0.0f, 0.0f, 1.0f, t);
		rotg_state_t now = rotg_get_state(&sm);
		if (prev == ROTG_STATE_ARMED && now != ROTG_STATE_ARMED) {
			saw_idle = true;
		}
		prev = now;
	}
	TEST_ASSERT(saw_idle, "FSM should exit ARMED when window expires");

	/* Releasing the chord returns to IDLE. */
	t += 10000;
	(void)rotg_step(&sm, false, false, 0.0f, 0.0f, 1.0f, t);
	TEST_ASSERT_EQ_INT((int)ROTG_STATE_IDLE, (int)rotg_get_state(&sm));
}

static void rotg_partial_flip_does_not_invert(void)
{
	rotg_state_machine_t sm;
	rotg_init(&sm);

	uint64_t t = 0;
	rotg_event_t e = ROTG_EVENT_NONE;
	for (uint32_t i = 0; i < 200 && e == ROTG_EVENT_NONE; i++) {
		t += 10000;
		e = rotg_step(&sm, true, true, 0.0f, 0.0f, 1.0f, t);
	}
	TEST_ASSERT_EQ_INT((int)ROTG_EVENT_ARMED, (int)e);

	/* Tilt to horizontal — dot = 0, neither flip nor return threshold. */
	for (uint32_t i = 0; i < 30; i++) {
		t += 10000;
		e = rotg_step(&sm, true, true, 1.0f, 0.0f, 0.0f, t);
		TEST_ASSERT_EQ_INT((int)ROTG_EVENT_NONE, (int)e);
	}
	TEST_ASSERT_EQ_INT((int)ROTG_STATE_ARMED, (int)rotg_get_state(&sm));
}

static void rotg_cancel_returns_to_idle(void)
{
	rotg_state_machine_t sm;
	rotg_init(&sm);

	uint64_t t = 0;
	for (uint32_t i = 0; i < 200; i++) {
		t += 10000;
		(void)rotg_step(&sm, true, true, 0.0f, 0.0f, 1.0f, t);
	}
	TEST_ASSERT_EQ_INT((int)ROTG_STATE_ARMED, (int)rotg_get_state(&sm));

	rotg_cancel(&sm);
	TEST_ASSERT_EQ_INT((int)ROTG_STATE_IDLE, (int)rotg_get_state(&sm));
	TEST_ASSERT(!sm.has_ref, "cancel clears reference");
}

/* ---- Test runner ---- */

int main(void)
{
	test_framework_init();

	RUN_TEST(sensor_table_has_expected_entry_count);
	RUN_TEST(sensor_table_includes_expected_ids);
	RUN_TEST(lookup_rejects_invalid_ids);
	RUN_TEST(accel_descriptor_has_three_values_and_imu_max_rate);
	RUN_TEST(mag_descriptor_has_mag_max_rate);
	RUN_TEST(air_mouse_descriptor_has_four_values_and_60hz_max);
	RUN_TEST(quaternion_has_four_components);

	RUN_TEST(cursor_zero_returns_first_sensor);
	RUN_TEST(cursor_last_index_sets_eof);
	RUN_TEST(cursor_beyond_table_returns_null_and_eof);
	RUN_TEST(full_cursor_walk_visits_all_sensors);

	RUN_TEST(clamp_imu_rate_caps_at_104000);
	RUN_TEST(clamp_mag_rate_caps_at_40000);
	RUN_TEST(clamp_air_mouse_rate_caps_at_60000);
	RUN_TEST(clamp_below_minimum_raises_to_minimum);
	RUN_TEST(clamp_zero_returns_zero);
	RUN_TEST(clamp_invalid_sensor_returns_zero);
	RUN_TEST(clamp_exact_rate_passes_through);

	RUN_TEST(stream_configure_activates_slot);
	RUN_TEST(stream_configure_clamps_rate);
	RUN_TEST(stream_stop_is_idempotent);
	RUN_TEST(stream_reconfigure_updates_rate);
	RUN_TEST(stream_rejects_invalid_sensor);
	RUN_TEST(stream_fills_all_slots_then_busy);

	RUN_TEST(calib_start_imu_succeeds);
	RUN_TEST(calib_start_gyro_also_targets_imu);
	RUN_TEST(calib_start_mag_succeeds);
	RUN_TEST(calib_concurrent_rejected_with_busy);
	RUN_TEST(calib_complete_clears_owner);
	RUN_TEST(calib_rejects_invalid_sensor);
	RUN_TEST(calib_idle_when_not_started);

	RUN_TEST(is_valid_sensor_for_known_and_unknown);

	/* Rotation gesture FSM */
	RUN_TEST(rotg_starts_in_idle);
	RUN_TEST(rotg_release_in_idle_stays_idle);
	RUN_TEST(rotg_short_chord_does_not_arm);
	RUN_TEST(rotg_full_chord_arms_and_captures_reference);
	RUN_TEST(rotg_complete_flip_returns_and_confirms);
	RUN_TEST(rotg_b_in_confirm_cancels);
	RUN_TEST(rotg_confirm_window_timeout_cancels);
	RUN_TEST(rotg_armed_window_expires_silently);
	RUN_TEST(rotg_partial_flip_does_not_invert);
	RUN_TEST(rotg_cancel_returns_to_idle);

	return test_framework_finish();
}
