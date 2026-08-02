/*
 * test_motion.cpp - host tests for Madgwick fusion, air mouse, tilt,
 * and gesture primitives.
 *
 * Links motion_eval.c directly (pure-C, no SDK deps).
 *
 * Coverage:
 *   - Fusion convergence on stationary input (gravity-aligned)
 *   - Fusion yaw rotation golden vector
 *   - Variable dt / jitter handling
 *   - Quaternion normalization + finite check
 *   - Q30 conversion (valid, NaN/Inf rejection, clamping)
 *   - Euler angle conversion
 *   - Gravity vector from quaternion
 *   - 6DoF fallback when mag absent/disturbed >500ms
 *   - Hysteretic recovery (healthy >1s before restoring 9DoF)
 *   - 40/104Hz mag synchronization (last-known mag)
 *   - Air mouse: dead zone, sensitivity ramp, fractional carry, clamp
 *   - Tilt hysteresis: engage 25°, release 17°, one direction, paired release
 *   - NaN/Inf input → SENSOR_FAULT
 */
#include "test_framework.h"
#include "../../src/motion/motion_eval.c"
#include <math.h>

#define DEG2RAD(x) ((x) * 0.017453292519943295f)
#define RAD2DEG(x) ((x) * 57.295779513082323f)

/* ====================================================================== */
/*  Helper: feed stationary data and verify convergence                   */
/* ====================================================================== */

static void feed_stationary_6dof(motion_fusion_state_t *st, int steps,
                                  float dt) {
	/* accel = [0, 0, 1g], gyro = [0, 0, 0] */
	for (int i = 0; i < steps; i++) {
		motion_fusion_update(st, 0, 0, 0, 0, 0, 1.0f,
		                     0, 0, 0, false, 0, dt);
	}
}

/* ====================================================================== */
/*  Fusion: stationary convergence                                        */
/* ====================================================================== */

static void test_fusion_stationary_converges_to_identity(void) {
	motion_fusion_state_t st;
	motion_fusion_init(&st);
	st.mode = MOTION_FUSION_MODE_6DOF;

	/* Perturb initial quaternion 30 degrees around X */
	float half = DEG2RAD(15.0f);
	st.q.w = cosf(half); st.q.x = sinf(half);
	st.q.y = 0.0f;       st.q.z = 0.0f;
	motion_quat_normalize(&st.q);

	feed_stationary_6dof(&st, 1000, MOTION_IMU_DT_NOMINAL_S);

	/* Should converge close to identity */
	TEST_ASSERT(fabsf(st.q.w - 1.0f) < 0.01f, "w ~ 1.0");
	TEST_ASSERT(fabsf(st.q.x) < 0.01f, "x ~ 0");
	TEST_ASSERT(fabsf(st.q.y) < 0.01f, "y ~ 0");
	TEST_ASSERT(fabsf(st.q.z) < 0.01f, "z ~ 0");
}

/* ====================================================================== */
/*  Fusion: yaw rotation golden vector                                    */
/* ====================================================================== */

static void test_fusion_yaw_rotation_90deg(void) {
	motion_fusion_state_t st;
	motion_fusion_init(&st);
	st.mode = MOTION_FUSION_MODE_6DOF;

	/* Rotate at 90 dps around Z for ~1 second */
	float gz = DEG2RAD(90.0f);
	float dt = MOTION_IMU_DT_NOMINAL_S;
	for (int i = 0; i < 104; i++) {
		motion_fusion_update(&st, 0, 0, gz, 0, 0, 1.0f,
		                     0, 0, 0, false, 0, dt);
	}

	/* After 90 deg yaw, quaternion ~ [cos(45), 0, 0, sin(45)] */
	TEST_ASSERT(fabsf(st.q.w - cosf(DEG2RAD(45.0f))) < 0.05f,
	            "w ~ cos(45)");
	TEST_ASSERT(fabsf(st.q.z - sinf(DEG2RAD(45.0f))) < 0.05f,
	            "z ~ sin(45)");
	TEST_ASSERT(fabsf(st.q.x) < 0.05f, "x ~ 0");
	TEST_ASSERT(fabsf(st.q.y) < 0.05f, "y ~ 0");
}

/* ====================================================================== */
/*  Fusion: variable dt / jitter handling                                 */
/* ====================================================================== */

static void test_fusion_handles_jittered_dt(void) {
	motion_fusion_state_t st;
	motion_fusion_init(&st);
	st.mode = MOTION_FUSION_MODE_6DOF;

	/* Feed with varying dt: 7ms, 10ms, 12ms, 9ms, 11ms */
	float dts[] = {0.007f, 0.010f, 0.012f, 0.009f, 0.011f};
	float gz = DEG2RAD(90.0f);
	for (int cycle = 0; cycle < 200; cycle++) {
		float dt = dts[cycle % 5];
		motion_result_t r = motion_fusion_update(&st, 0, 0, gz, 0, 0, 1.0f,
		                                          0, 0, 0, false, 0, dt);
		TEST_ASSERT(r == MOTION_RESULT_OK, "jitter dt should not fault");
	}
	/* Quaternion should still be finite and normalized */
	TEST_ASSERT(motion_quat_is_finite(&st.q), "finite after jitter");
	float n = sqrtf(st.q.w*st.q.w + st.q.x*st.q.x +
	                st.q.y*st.q.y + st.q.z*st.q.z);
	TEST_ASSERT(fabsf(n - 1.0f) < 0.001f, "still unit quaternion");
}

static void test_fusion_rejects_invalid_dt(void) {
	motion_fusion_state_t st;
	motion_fusion_init(&st);
	st.mode = MOTION_FUSION_MODE_6DOF;

	motion_quat_t saved = st.q;

	/* dt = 0 should fault */
	motion_result_t r = motion_fusion_update(&st, 0, 0, 0, 0, 0, 1.0f,
	                     0, 0, 0, false, 0, 0.0f);
	TEST_ASSERT(r == MOTION_RESULT_SENSOR_FAULT, "dt=0 should fault");

	/* dt > 1.0 should fault */
	r = motion_fusion_update(&st, 0, 0, 0, 0, 0, 1.0f,
	    0, 0, 0, false, 0, 2.0f);
	TEST_ASSERT(r == MOTION_RESULT_SENSOR_FAULT, "dt>1s should fault");

	/* State should be unchanged after fault */
	TEST_ASSERT_EQ_INT((int)(saved.w * 1000), (int)(st.q.w * 1000));
}

/* ====================================================================== */
/*  Quaternion utilities                                                  */
/* ====================================================================== */

static void test_quat_normalize_identity(void) {
	motion_quat_t q = {3.0f, 0.0f, 0.0f, 0.0f};
	motion_result_t r = motion_quat_normalize(&q);
	TEST_ASSERT(r == MOTION_RESULT_OK, "normalize non-finite magnitude");
	TEST_ASSERT(fabsf(q.w - 1.0f) < 0.001f, "normalized to unit");
}

static void test_quat_normalize_zero_fault(void) {
	motion_quat_t q = {0.0f, 0.0f, 0.0f, 0.0f};
	motion_result_t r = motion_quat_normalize(&q);
	TEST_ASSERT(r == MOTION_RESULT_SENSOR_FAULT, "zero magnitude fault");
}

static void test_quat_is_finite_valid(void) {
	motion_quat_t q = {1.0f, 0.0f, 0.0f, 0.0f};
	TEST_ASSERT(motion_quat_is_finite(&q), "identity is finite");
}

static void test_quat_is_finite_nan(void) {
	motion_quat_t q = {NAN, 0.0f, 0.0f, 0.0f};
	TEST_ASSERT(!motion_quat_is_finite(&q), "NaN w not finite");
}

static void test_quat_is_finite_inf(void) {
	motion_quat_t q = {1.0f, INFINITY, 0.0f, 0.0f};
	TEST_ASSERT(!motion_quat_is_finite(&q), "Inf x not finite");
}

/* ====================================================================== */
/*  Q30 conversion                                                        */
/* ====================================================================== */

static void test_q30_identity(void) {
	motion_quat_t q = {1.0f, 0.0f, 0.0f, 0.0f};
	int32_t out[4];
	motion_result_t r = motion_quat_to_q30(&q, out);
	TEST_ASSERT(r == MOTION_RESULT_OK, "identity Q30 OK");
	/* w=1.0 → 1.0 * 2^30 = 1073741824 */
	TEST_ASSERT_EQ_INT(1073741824, out[0]);
	TEST_ASSERT_EQ_INT(0, out[1]);
	TEST_ASSERT_EQ_INT(0, out[2]);
	TEST_ASSERT_EQ_INT(0, out[3]);
}

static void test_q30_negative_half(void) {
	motion_quat_t q = {0.0f, -0.5f, 0.0f, 0.0f};
	int32_t out[4];
	motion_result_t r = motion_quat_to_q30(&q, out);
	TEST_ASSERT(r == MOTION_RESULT_OK, "Q30 OK");
	TEST_ASSERT_EQ_INT(0, out[0]);
	/* -0.5 * 2^30 = -536870912 */
	TEST_ASSERT_EQ_INT(-536870912, out[1]);
}

static void test_q30_nan_rejected(void) {
	motion_quat_t q = {NAN, 0.0f, 0.0f, 0.0f};
	int32_t out[4];
	motion_result_t r = motion_quat_to_q30(&q, out);
	TEST_ASSERT(r == MOTION_RESULT_SENSOR_FAULT, "NaN Q30 fault");
}

static void test_q30_inf_clamped(void) {
	motion_quat_t q = {INFINITY, 0.0f, 0.0f, 0.0f};
	int32_t out[4];
	motion_result_t r = motion_quat_to_q30(&q, out);
	TEST_ASSERT(r == MOTION_RESULT_SENSOR_FAULT, "Inf Q30 fault");
}

static void test_q30_round_trip(void) {
	/* cos(30°), sin(30°), 0, 0 → 60° rotation */
	motion_quat_t q = {cosf(DEG2RAD(30.0f)), sinf(DEG2RAD(30.0f)),
	                   0.0f, 0.0f};
	int32_t out[4];
	motion_result_t r = motion_quat_to_q30(&q, out);
	TEST_ASSERT(r == MOTION_RESULT_OK, "Q30 round trip OK");
	/* Convert back: value = stored / 2^30 */
	float w_back = (float)out[0] / 1073741824.0f;
	float x_back = (float)out[1] / 1073741824.0f;
	TEST_ASSERT(fabsf(w_back - q.w) < 0.001f, "w round trips");
	TEST_ASSERT(fabsf(x_back - q.x) < 0.001f, "x round trips");
}

/* ====================================================================== */
/*  Euler angle conversion                                                */
/* ====================================================================== */

static void test_euler_identity(void) {
	motion_quat_t q = {1.0f, 0.0f, 0.0f, 0.0f};
	int32_t yaw, pitch, roll;
	motion_result_t r = motion_quat_to_euler_millideg(&q, &yaw, &pitch, &roll);
	TEST_ASSERT(r == MOTION_RESULT_OK, "euler OK");
	TEST_ASSERT_EQ_INT(0, yaw);
	TEST_ASSERT_EQ_INT(0, pitch);
	TEST_ASSERT_EQ_INT(0, roll);
}

static void test_euler_yaw_90(void) {
	motion_quat_t q = {cosf(DEG2RAD(45.0f)), 0.0f, 0.0f,
	                   sinf(DEG2RAD(45.0f))};
	int32_t yaw, pitch, roll;
	motion_quat_to_euler_millideg(&q, &yaw, &pitch, &roll);
	TEST_ASSERT(fabsf((float)yaw - 90000.0f) < 200.0f, "yaw ~90 deg");
	TEST_ASSERT(fabsf((float)pitch) < 200.0f, "pitch ~0");
	TEST_ASSERT(fabsf((float)roll) < 200.0f, "roll ~0");
}

static void test_euler_pitch_45(void) {
	motion_quat_t q = {cosf(DEG2RAD(22.5f)), 0.0f,
	                   sinf(DEG2RAD(22.5f)), 0.0f};
	int32_t yaw, pitch, roll;
	motion_quat_to_euler_millideg(&q, &yaw, &pitch, &roll);
	TEST_ASSERT(fabsf((float)pitch - 45000.0f) < 200.0f, "pitch ~45 deg");
	TEST_ASSERT(fabsf((float)yaw) < 200.0f, "yaw ~0");
}

/* ====================================================================== */
/*  Gravity from quaternion                                               */
/* ====================================================================== */

static void test_gravity_identity(void) {
	motion_quat_t q = {1.0f, 0.0f, 0.0f, 0.0f};
	float g[3];
	motion_gravity_from_quat(&q, g);
	TEST_ASSERT(fabsf(g[0]) < 0.001f, "gx ~ 0");
	TEST_ASSERT(fabsf(g[1]) < 0.001f, "gy ~ 0");
	TEST_ASSERT(fabsf(g[2] - 1.0f) < 0.001f, "gz ~ 1");
}

static void test_gravity_180_flip(void) {
	/* 180° rotation around X: gravity should flip Y and Z */
	motion_quat_t q = {0.0f, 1.0f, 0.0f, 0.0f};
	float g[3];
	motion_gravity_from_quat(&q, g);
	TEST_ASSERT(fabsf(g[2] + 1.0f) < 0.001f, "gz ~ -1 (flipped)");
}

static void test_gravity_dot(void) {
	float a[3] = {0.0f, 0.0f, 1.0f};
	float b[3] = {0.0f, 0.0f, 1.0f};
	TEST_ASSERT(fabsf(motion_vec3_dot(a, b) - 1.0f) < 0.001f,
	            "same direction dot=1");
	float c[3] = {1.0f, 0.0f, 0.0f};
	TEST_ASSERT(fabsf(motion_vec3_dot(a, c)) < 0.001f,
	            "perpendicular dot=0");
}

/* ====================================================================== */
/*  6DoF fallback and recovery                                            */
/* ====================================================================== */

static void test_mag_absent_forces_6dof(void) {
	motion_fusion_state_t st;
	motion_fusion_init(&st);
	motion_mag_health_eval(&st, false, false, false, 0);
	TEST_ASSERT(st.mode == MOTION_FUSION_MODE_6DOF, "no mag → 6DoF");
}

static void test_mag_disturbed_switches_to_6dof(void) {
	motion_fusion_state_t st;
	motion_fusion_init(&st);
	/* Start healthy 9DoF */
	motion_mag_health_eval(&st, true, true, true, 1000);
	TEST_ASSERT(st.mode == MOTION_FUSION_MODE_9DOF, "start 9DoF");

	/* Disturbance starts; first unhealthy reading at t=100000 */
	motion_mag_health_eval(&st, true, true, false, 100000);
	TEST_ASSERT(st.mode == MOTION_FUSION_MODE_9DOF,
	            "first unhealthy stays 9DoF");

	/* 400ms later (< 500ms threshold from disturbance start) → stays 9DoF */
	motion_mag_health_eval(&st, true, true, false, 500000);
	TEST_ASSERT(st.mode == MOTION_FUSION_MODE_9DOF,
	            "disturb <500ms stays 9DoF");

	/* Disturbance exceeds 500ms from start → switch to 6DoF */
	motion_mag_health_eval(&st, true, true, false, 601000);
	TEST_ASSERT(st.mode == MOTION_FUSION_MODE_6DOF,
	            "disturb >500ms → 6DoF");
}

static void test_mag_recovery_hysteretic(void) {
	motion_fusion_state_t st;
	motion_fusion_init(&st);
	/* Get into 6DoF via disturbance */
	motion_mag_health_eval(&st, true, true, true, 1000);
	motion_mag_health_eval(&st, true, true, false, 100000);
	motion_mag_health_eval(&st, true, true, false, 601000);
	TEST_ASSERT(st.mode == MOTION_FUSION_MODE_6DOF, "in 6DoF");

	/* Healthy at t=700000 → recovery starts */
	motion_mag_health_eval(&st, true, true, true, 700000);
	TEST_ASSERT(st.recovering, "recovery in progress");
	TEST_ASSERT(st.mode == MOTION_FUSION_MODE_6DOF,
	            "recovery start stays 6DoF");

	/* Continue healthy for 800ms total (< 1s recovery) → stays 6DoF */
	motion_mag_health_eval(&st, true, true, true, 1500000);
	TEST_ASSERT(st.mode == MOTION_FUSION_MODE_6DOF,
	            "recovery <1s stays 6DoF");

	/* Continue healthy past 1s → switch to 9DoF */
	motion_mag_health_eval(&st, true, true, true, 1701000);
	TEST_ASSERT(st.mode == MOTION_FUSION_MODE_9DOF,
	            "recovery >1s → 9DoF");
	TEST_ASSERT(!st.recovering, "recovery complete");
}

static void test_mag_recovery_interrupted(void) {
	motion_fusion_state_t st;
	motion_fusion_init(&st);
	motion_mag_health_eval(&st, true, true, true, 1000);
	motion_mag_health_eval(&st, true, true, false, 100000);
	motion_mag_health_eval(&st, true, true, false, 601000);
	TEST_ASSERT(st.mode == MOTION_FUSION_MODE_6DOF, "in 6DoF");

	/* Start recovery */
	motion_mag_health_eval(&st, true, true, true, 700000);
	TEST_ASSERT(st.recovering, "recovery started");

	/* Interruption resets recovery */
	motion_mag_health_eval(&st, true, true, false, 701000);
	TEST_ASSERT(!st.recovering, "interruption resets recovery");

	/* Healthy again — timer restarts from now */
	motion_mag_health_eval(&st, true, true, true, 800000);
	TEST_ASSERT(st.recovering, "recovery restarts");
	motion_mag_health_eval(&st, true, true, true, 1801000);
	TEST_ASSERT(st.mode == MOTION_FUSION_MODE_9DOF,
	            "1s from restart → 9DoF");
}

/* ====================================================================== */
/*  40/104Hz mag synchronization                                          */
/* ====================================================================== */

static void test_mag_sync_last_known_40hz(void) {
	motion_fusion_state_t st;
	motion_fusion_init(&st);
	st.mode = MOTION_FUSION_MODE_9DOF;
	st.ref_set = true;
	st.mag_healthy = true;

	/* Feed mag sample at t=0 */
	st.last_mx = 0.3f; st.last_my = 0.0f; st.last_mz = 0.9f;
	st.last_mag_us = 0;

	/* Feed 3 IMU samples at 104Hz (≈9.6ms apart) without new mag */
	float dt = MOTION_IMU_DT_NOMINAL_S;
	for (int i = 0; i < 3; i++) {
		motion_result_t r = motion_fusion_update(&st, 0, 0, 0, 0, 0, 1.0f,
		                     st.last_mx, st.last_my, st.last_mz,
		                     true, (i+1) * 9615, dt);
		TEST_ASSERT(r == MOTION_RESULT_OK, "9DoF with last-known mag OK");
	}
	/* Quaternion should still be finite */
	TEST_ASSERT(motion_quat_is_finite(&st.q), "finite after mag sync");
}

/* ====================================================================== */
/*  NaN / Inf input rejection                                             */
/* ====================================================================== */

static void test_fusion_nan_gyro_rejected(void) {
	motion_fusion_state_t st;
	motion_fusion_init(&st);
	st.mode = MOTION_FUSION_MODE_6DOF;
	motion_quat_t saved = st.q;

	motion_result_t r = motion_fusion_update(&st,
	    NAN, 0, 0, 0, 0, 1.0f, 0, 0, 0, false, 0,
	    MOTION_IMU_DT_NOMINAL_S);
	TEST_ASSERT(r == MOTION_RESULT_SENSOR_FAULT, "NaN gyro fault");
	TEST_ASSERT_EQ_INT((int)(saved.w * 1000), (int)(st.q.w * 1000));
}

static void test_fusion_inf_accel_rejected(void) {
	motion_fusion_state_t st;
	motion_fusion_init(&st);
	st.mode = MOTION_FUSION_MODE_6DOF;

	motion_result_t r = motion_fusion_update(&st,
	    0, 0, 0, INFINITY, 0, 0, 0, 0, 0, false, 0,
	    MOTION_IMU_DT_NOMINAL_S);
	TEST_ASSERT(r == MOTION_RESULT_SENSOR_FAULT, "Inf accel fault");
}

static void test_fusion_nan_mag_rejected_in_9dof(void) {
	motion_fusion_state_t st;
	motion_fusion_init(&st);
	st.mode = MOTION_FUSION_MODE_9DOF;
	st.ref_set = true;

	motion_result_t r = motion_fusion_update(&st,
	    0, 0, 0, 0, 0, 1.0f,
	    NAN, 0, 0, true, 0, MOTION_IMU_DT_NOMINAL_S);
	TEST_ASSERT(r == MOTION_RESULT_SENSOR_FAULT, "NaN mag fault in 9DoF");
}

static void test_fusion_zero_accel_fault(void) {
	motion_fusion_state_t st;
	motion_fusion_init(&st);
	st.mode = MOTION_FUSION_MODE_6DOF;

	/* All-zero accel → normalize3 returns 0 → SENSOR_FAULT */
	motion_result_t r = motion_fusion_update(&st,
	    0, 0, 0, 0, 0, 0, 0, 0, 0, false, 0,
	    MOTION_IMU_DT_NOMINAL_S);
	TEST_ASSERT(r == MOTION_RESULT_SENSOR_FAULT, "zero accel fault");
}

/* ====================================================================== */
/*  Air mouse: dead zone                                                  */
/* ====================================================================== */

static void test_air_mouse_deadzone_below_threshold(void) {
	motion_air_mouse_state_t s;
	motion_air_mouse_init(&s);
	motion_air_mouse_enable(&s);

	/* 0.5 dps is below 0.8 dps dead zone */
	motion_air_mouse_update(&s, 0.5f, 0.0f, 1.0f / 60.0f);
	TEST_ASSERT_EQ_INT(0, s.dx);
	TEST_ASSERT_EQ_INT(0, s.dy);
}

static void test_air_mouse_deadzone_at_boundary(void) {
	motion_air_mouse_state_t s;
	motion_air_mouse_init(&s);
	motion_air_mouse_enable(&s);

	/* Exactly 0.8 dps → just at dead zone boundary, should be zero */
	motion_air_mouse_update(&s, 0.8f, 0.0f, 1.0f / 60.0f);
	TEST_ASSERT_EQ_INT(0, s.dx);
}

static void test_air_mouse_above_deadzone_produces_movement(void) {
	motion_air_mouse_state_t s;
	motion_air_mouse_init(&s);
	motion_air_mouse_enable(&s);

	/* 50 dps for 1/60 s = 0.833 deg → 0.833 * 8 = 6.67 px */
	motion_air_mouse_update(&s, 50.0f, 0.0f, 1.0f / 60.0f);
	TEST_ASSERT(s.dx > 0, "should produce positive dx");
	TEST_ASSERT(s.dx >= 5 && s.dx <= 8, "dx ~6-7 pixels");
}

/* ====================================================================== */
/*  Air mouse: sensitivity ramp                                           */
/* ====================================================================== */

static void test_air_mouse_sensitivity_base(void) {
	/* Below ramp start (120 dps) → base sensitivity */
	TEST_ASSERT(fabsf(motion_air_mouse_sensitivity(0.0f) - 8.0f) < 0.01f,
	            "0 dps → 8 px/deg");
	TEST_ASSERT(fabsf(motion_air_mouse_sensitivity(100.0f) - 8.0f) < 0.01f,
	            "100 dps → 8 px/deg");
	TEST_ASSERT(fabsf(motion_air_mouse_sensitivity(120.0f) - 8.0f) < 0.01f,
	            "120 dps → 8 px/deg");
}

static void test_air_mouse_sensitivity_high(void) {
	/* At/above ramp end (240 dps) → high sensitivity */
	TEST_ASSERT(fabsf(motion_air_mouse_sensitivity(240.0f) - 14.0f) < 0.01f,
	            "240 dps → 14 px/deg");
	TEST_ASSERT(fabsf(motion_air_mouse_sensitivity(500.0f) - 14.0f) < 0.01f,
	            "500 dps → 14 px/deg");
}

static void test_air_mouse_sensitivity_midpoint(void) {
	/* At 180 dps (midpoint of 120-240) → smoothstep ~0.5 → ~11 px/deg */
	float sens = motion_air_mouse_sensitivity(180.0f);
	TEST_ASSERT(sens > 9.0f && sens < 13.0f,
	            "midpoint ramp between 8 and 14");
}

static void test_air_mouse_sensitivity_monotonic(void) {
	/* Sensitivity must be monotonically non-decreasing */
	float prev = motion_air_mouse_sensitivity(0.0f);
	for (float d = 10.0f; d <= 300.0f; d += 10.0f) {
		float curr = motion_air_mouse_sensitivity(d);
		TEST_ASSERT(curr >= prev - 0.001f, "monotonic non-decreasing");
		prev = curr;
	}
}

/* ====================================================================== */
/*  Air mouse: fractional carry                                           */
/* ====================================================================== */

static void test_air_mouse_fractional_carry(void) {
	motion_air_mouse_state_t s;
	motion_air_mouse_init(&s);
	motion_air_mouse_enable(&s);

	/* 5 dps * (1/60s) * 8 px/deg = 0.667 px/update.
	 * After 1 update: carry=0.667, dx=0
	 * After 2 updates: carry=1.333, dx=1 (or 2 with truncation) */
	float dt = 1.0f / 60.0f;
	motion_air_mouse_update(&s, 5.0f, 0.0f, dt);
	TEST_ASSERT_EQ_INT(0, s.dx);  /* 0.667 px → 0 */

	motion_air_mouse_update(&s, 5.0f, 0.0f, dt);
	/* carry was 0.667, now 0.667+0.667=1.333 → dx=1 */
	TEST_ASSERT_EQ_INT(1, s.dx);
}

static void test_air_mouse_recenter_on_enable(void) {
	motion_air_mouse_state_t s;
	motion_air_mouse_init(&s);
	motion_air_mouse_enable(&s);

	/* Generate some movement */
	motion_air_mouse_update(&s, 100.0f, 50.0f, 1.0f / 60.0f);
	TEST_ASSERT(s.dx != 0 || s.dy != 0, "some movement");

	/* Re-enable should recenter */
	motion_air_mouse_enable(&s);
	TEST_ASSERT_EQ_INT(0, s.dx);
	TEST_ASSERT_EQ_INT(0, s.dy);
}

static void test_air_mouse_disabled_no_movement(void) {
	motion_air_mouse_state_t s;
	motion_air_mouse_init(&s);
	/* Not enabled */
	motion_air_mouse_update(&s, 100.0f, 100.0f, 1.0f / 60.0f);
	TEST_ASSERT_EQ_INT(0, s.dx);
	TEST_ASSERT_EQ_INT(0, s.dy);
}

static void test_air_mouse_clamp_int16(void) {
	motion_air_mouse_state_t s;
	motion_air_mouse_init(&s);
	motion_air_mouse_enable(&s);

	/* Very high rate for long dt → should clamp, not overflow */
	motion_air_mouse_update(&s, 100000.0f, 100000.0f, 1.0f);
	TEST_ASSERT(s.dx <= 32767, "dx clamped to int16 max");
	TEST_ASSERT(s.dy <= 32767, "dy clamped to int16 max");
}

static void test_air_mouse_nan_rejected(void) {
	motion_air_mouse_state_t s;
	motion_air_mouse_init(&s);
	motion_air_mouse_enable(&s);

	motion_result_t r = motion_air_mouse_update(&s, NAN, 0.0f, 0.016f);
	TEST_ASSERT(r == MOTION_RESULT_SENSOR_FAULT, "NaN rate fault");
}

/* ====================================================================== */
/*  Tilt: engage / release hysteresis                                     */
/* ====================================================================== */

static void test_tilt_init_none(void) {
	motion_tilt_state_t s;
	motion_tilt_init(&s);
	TEST_ASSERT(s.current == MOTION_TILT_DIR_NONE, "init = NONE");
}

static void test_tilt_engage_up_at_25(void) {
	motion_tilt_state_t s;
	motion_tilt_init(&s);
	motion_tilt_event_t ev;

	motion_tilt_update(&s, 24.0f, 0.0f, &ev);
	TEST_ASSERT(ev.pressed == MOTION_TILT_DIR_NONE, "24° no engage");

	motion_tilt_update(&s, 25.0f, 0.0f, &ev);
	TEST_ASSERT(ev.pressed == MOTION_TILT_DIR_UP, "25° engage UP");
	TEST_ASSERT(ev.released == MOTION_TILT_DIR_NONE, "no release");
}

static void test_tilt_hysteresis_stays_engaged(void) {
	motion_tilt_state_t s;
	motion_tilt_init(&s);
	motion_tilt_event_t ev;

	motion_tilt_update(&s, 30.0f, 0.0f, &ev);  /* engage UP */
	motion_tilt_update(&s, 20.0f, 0.0f, &ev);  /* above release 17° */
	TEST_ASSERT(ev.pressed == MOTION_TILT_DIR_NONE, "no re-press");
	TEST_ASSERT(ev.released == MOTION_TILT_DIR_NONE, "no release at 20°");
	TEST_ASSERT(s.current == MOTION_TILT_DIR_UP, "stays UP at 20°");
}

static void test_tilt_release_at_17(void) {
	motion_tilt_state_t s;
	motion_tilt_init(&s);
	motion_tilt_event_t ev;

	motion_tilt_update(&s, 30.0f, 0.0f, &ev);  /* engage UP */
	motion_tilt_update(&s, 16.0f, 0.0f, &ev);  /* below release 17° */
	TEST_ASSERT(ev.released == MOTION_TILT_DIR_UP, "release UP at 16°");
	TEST_ASSERT(s.current == MOTION_TILT_DIR_NONE, "NONE after release");
}

static void test_tilt_engage_all_directions(void) {
	motion_tilt_state_t s;
	motion_tilt_init(&s);
	motion_tilt_event_t ev;

	motion_tilt_update(&s, -25.0f, 0.0f, &ev);
	TEST_ASSERT(ev.pressed == MOTION_TILT_DIR_DOWN, "engage DOWN");

	motion_tilt_update(&s, 0.0f, 0.0f, &ev);  /* release */
	motion_tilt_update(&s, 0.0f, 25.0f, &ev);
	TEST_ASSERT(ev.pressed == MOTION_TILT_DIR_RIGHT, "engage RIGHT");

	motion_tilt_update(&s, 0.0f, 0.0f, &ev);  /* release */
	motion_tilt_update(&s, 0.0f, -25.0f, &ev);
	TEST_ASSERT(ev.pressed == MOTION_TILT_DIR_LEFT, "engage LEFT");
}

static void test_tilt_one_direction_at_a_time(void) {
	motion_tilt_state_t s;
	motion_tilt_init(&s);
	motion_tilt_event_t ev;

	/* Engage UP */
	motion_tilt_update(&s, 30.0f, 0.0f, &ev);
	TEST_ASSERT(s.current == MOTION_TILT_DIR_UP, "UP engaged");

	/* Roll also exceeds threshold but UP is already engaged */
	motion_tilt_update(&s, 30.0f, 35.0f, &ev);
	TEST_ASSERT(s.current == MOTION_TILT_DIR_UP, "stays UP not RIGHT");
}

static void test_tilt_release_then_engage_new(void) {
	motion_tilt_state_t s;
	motion_tilt_init(&s);
	motion_tilt_event_t ev;

	/* Engage UP, then release, then engage RIGHT */
	motion_tilt_update(&s, 30.0f, 0.0f, &ev);  /* UP */
	TEST_ASSERT(ev.pressed == MOTION_TILT_DIR_UP, "press UP");

	motion_tilt_update(&s, 10.0f, 0.0f, &ev);  /* release */
	TEST_ASSERT(ev.released == MOTION_TILT_DIR_UP, "release UP");

	motion_tilt_update(&s, 0.0f, 30.0f, &ev);  /* RIGHT */
	TEST_ASSERT(ev.pressed == MOTION_TILT_DIR_RIGHT, "press RIGHT");
}

static void test_tilt_always_pairs_releases(void) {
	motion_tilt_state_t s;
	motion_tilt_init(&s);
	motion_tilt_event_t ev;

	/* Every press must be followed by a release before a new press */
	motion_tilt_update(&s, 30.0f, 0.0f, &ev);
	TEST_ASSERT(ev.pressed != MOTION_TILT_DIR_NONE, "press happened");
	TEST_ASSERT(ev.released == MOTION_TILT_DIR_NONE, "no release yet");

	motion_tilt_update(&s, 0.0f, 0.0f, &ev);
	TEST_ASSERT(ev.released != MOTION_TILT_DIR_NONE, "release happened");
	TEST_ASSERT(ev.pressed == MOTION_TILT_DIR_NONE, "no press on release");
}

/* ====================================================================== */
/*  main                                                                  */
/* ====================================================================== */

int main(void) {
	test_framework_init();

	/* Fusion convergence */
	RUN_TEST(test_fusion_stationary_converges_to_identity);
	RUN_TEST(test_fusion_yaw_rotation_90deg);
	RUN_TEST(test_fusion_handles_jittered_dt);
	RUN_TEST(test_fusion_rejects_invalid_dt);

	/* Quaternion utilities */
	RUN_TEST(test_quat_normalize_identity);
	RUN_TEST(test_quat_normalize_zero_fault);
	RUN_TEST(test_quat_is_finite_valid);
	RUN_TEST(test_quat_is_finite_nan);
	RUN_TEST(test_quat_is_finite_inf);

	/* Q30 conversion */
	RUN_TEST(test_q30_identity);
	RUN_TEST(test_q30_negative_half);
	RUN_TEST(test_q30_nan_rejected);
	RUN_TEST(test_q30_inf_clamped);
	RUN_TEST(test_q30_round_trip);

	/* Euler conversion */
	RUN_TEST(test_euler_identity);
	RUN_TEST(test_euler_yaw_90);
	RUN_TEST(test_euler_pitch_45);

	/* Gravity */
	RUN_TEST(test_gravity_identity);
	RUN_TEST(test_gravity_180_flip);
	RUN_TEST(test_gravity_dot);

	/* 6DoF fallback / recovery */
	RUN_TEST(test_mag_absent_forces_6dof);
	RUN_TEST(test_mag_disturbed_switches_to_6dof);
	RUN_TEST(test_mag_recovery_hysteretic);
	RUN_TEST(test_mag_recovery_interrupted);

	/* Mag synchronization */
	RUN_TEST(test_mag_sync_last_known_40hz);

	/* NaN rejection */
	RUN_TEST(test_fusion_nan_gyro_rejected);
	RUN_TEST(test_fusion_inf_accel_rejected);
	RUN_TEST(test_fusion_nan_mag_rejected_in_9dof);
	RUN_TEST(test_fusion_zero_accel_fault);

	/* Air mouse: dead zone */
	RUN_TEST(test_air_mouse_deadzone_below_threshold);
	RUN_TEST(test_air_mouse_deadzone_at_boundary);
	RUN_TEST(test_air_mouse_above_deadzone_produces_movement);

	/* Air mouse: sensitivity ramp */
	RUN_TEST(test_air_mouse_sensitivity_base);
	RUN_TEST(test_air_mouse_sensitivity_high);
	RUN_TEST(test_air_mouse_sensitivity_midpoint);
	RUN_TEST(test_air_mouse_sensitivity_monotonic);

	/* Air mouse: carry + misc */
	RUN_TEST(test_air_mouse_fractional_carry);
	RUN_TEST(test_air_mouse_recenter_on_enable);
	RUN_TEST(test_air_mouse_disabled_no_movement);
	RUN_TEST(test_air_mouse_clamp_int16);
	RUN_TEST(test_air_mouse_nan_rejected);

	/* Tilt hysteresis */
	RUN_TEST(test_tilt_init_none);
	RUN_TEST(test_tilt_engage_up_at_25);
	RUN_TEST(test_tilt_hysteresis_stays_engaged);
	RUN_TEST(test_tilt_release_at_17);
	RUN_TEST(test_tilt_engage_all_directions);
	RUN_TEST(test_tilt_one_direction_at_a_time);
	RUN_TEST(test_tilt_release_then_engage_new);
	RUN_TEST(test_tilt_always_pairs_releases);

	return test_framework_finish();
}
