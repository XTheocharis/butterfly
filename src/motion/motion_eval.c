/*
 * motion_eval.c - Pure-C motion evaluation implementations.
 *
 * No SDK deps. Compiled on BOTH host (unit tests) and device (firmware).
 *
 * allow: SIZE_OK — cohesive single-subsystem file: all functions operate on
 * motion fusion/air-mouse/tilt state types. The Madgwick gradient step
 * expansion is irreducible without sacrificing numerical clarity.
 *
 * Madgwick 2010 AHRS algorithm references:
 *   "An effective orientation filter for inertial and inertial/magnetic
 *    sensor arrays" — S. Madgwick, 2010.
 */
#include "motion_eval.h"
#include <math.h>

/* ---- Helpers --------------------------------------------------------- */

static bool motion_finite_f(float v) {
	/* NaN: v != v. Inf: v - v is NaN (not 0). */
	return (v == v) && ((v - v) == 0.0f);
}

static bool motion_finite3(float a, float b, float c) {
	return motion_finite_f(a) && motion_finite_f(b) && motion_finite_f(c);
}

static float motion_normalize3(float *ax, float *ay, float *az) {
	float norm = sqrtf((*ax) * (*ax) + (*ay) * (*ay) + (*az) * (*az));
	if (norm > 0.0f) {
		float inv = 1.0f / norm;
		*ax *= inv; *ay *= inv; *az *= inv;
	}
	return norm;
}

static float motion_normalize4(float *q0, float *q1, float *q2, float *q3) {
	float norm = sqrtf((*q0) * (*q0) + (*q1) * (*q1) +
	                   (*q2) * (*q2) + (*q3) * (*q3));
	if (norm > 0.0f) {
		float inv = 1.0f / norm;
		*q0 *= inv; *q1 *= inv; *q2 *= inv; *q3 *= inv;
	}
	return norm;
}

/* ---- Fusion lifecycle ------------------------------------------------ */

void motion_fusion_init(motion_fusion_state_t *st) {
	st->q.w = 1.0f; st->q.x = 0.0f; st->q.y = 0.0f; st->q.z = 0.0f;
	st->mode = MOTION_FUSION_MODE_9DOF;
	st->mag_present = false;
	st->mag_calibrated = false;
	st->mag_healthy = false;
	st->disturbance_start_us = 0;
	st->recovery_start_us = 0;
	st->recovering = false;
	st->last_mx = 0.0f; st->last_my = 0.0f; st->last_mz = 0.0f;
	st->last_mag_us = 0;
	st->ref_bx = 0.0f; st->ref_bz = 0.0f;
	st->ref_set = false;
}

motion_fusion_mode_t motion_mag_health_eval(motion_fusion_state_t *st,
                                            bool mag_present,
                                            bool mag_calibrated,
                                            bool mag_healthy,
                                            uint64_t now_us) {
	st->mag_present = mag_present;
	st->mag_calibrated = mag_calibrated;

	if (!mag_present || !mag_calibrated) {
		st->mode = MOTION_FUSION_MODE_6DOF;
		st->recovering = false;
		st->disturbance_start_us = 0;
		st->mag_healthy = false;
		return st->mode;
	}

	if (mag_healthy) {
		st->mag_healthy = true;
		st->disturbance_start_us = 0;
		if (st->mode == MOTION_FUSION_MODE_6DOF) {
			if (!st->recovering) {
				st->recovering = true;
				st->recovery_start_us = now_us;
			} else if ((now_us - st->recovery_start_us)
			           >= MOTION_MAG_RECOVERY_DURATION_US) {
				st->mode = MOTION_FUSION_MODE_9DOF;
				st->recovering = false;
			}
		}
	} else {
		st->mag_healthy = false;
		st->recovering = false;
		if (st->disturbance_start_us == 0)
			st->disturbance_start_us = now_us;
		if (st->mode == MOTION_FUSION_MODE_9DOF &&
		    (now_us - st->disturbance_start_us)
		    >= MOTION_MAG_DISTURBANCE_TIMEOUT_US) {
			st->mode = MOTION_FUSION_MODE_6DOF;
		}
	}
	return st->mode;
}

void motion_fusion_set_mag_reference(motion_fusion_state_t *st,
                                     float mx, float my, float mz) {
	float q0 = st->q.w, q1 = st->q.x, q2 = st->q.y, q3 = st->q.z;
	/* Rotate mag from body to earth frame: h = R(q) * m */
	float hx = 2.0f * (mx * (0.5f - q2*q2 - q3*q3) +
	                   my * (q1*q2 - q0*q3) +
	                   mz * (q1*q3 + q0*q2));
	float hy = 2.0f * (mx * (q1*q2 + q0*q3) +
	                   my * (0.5f - q1*q1 - q3*q3) +
	                   mz * (q2*q3 - q0*q1));
	float hz = 2.0f * (mx * (q1*q3 - q0*q2) +
	                   my * (q2*q3 + q0*q1) +
	                   mz * (0.5f - q1*q1 - q2*q2));
	st->ref_bx = sqrtf(hx*hx + hy*hy);
	st->ref_bz = hz;
	st->ref_set = true;
}

/* ---- 6DoF gradient step (accelerometer only) ------------------------- */

static motion_result_t fusion_step_6dof(motion_fusion_state_t *st,
                                        float gx, float gy, float gz,
                                        float ax, float ay, float az,
                                        float dt) {
	float q0 = st->q.w, q1 = st->q.x, q2 = st->q.y, q3 = st->q.z;

	/* Normalize accelerometer */
	if (motion_normalize3(&ax, &ay, &az) == 0.0f)
		return MOTION_RESULT_SENSOR_FAULT;

	/* Objective function */
	float f0 = 2.0f*(q1*q3 - q0*q2) - ax;
	float f1 = 2.0f*(q0*q1 + q2*q3) - ay;
	float f2 = 2.0f*(0.5f - q1*q1 - q2*q2) - az;

	/* Gradient = J^T * f */
	float s0 = (-2.0f*q2)*f0 + (2.0f*q1)*f1;
	float s1 = (2.0f*q3)*f0 + (2.0f*q0)*f1 + (-4.0f*q1)*f2;
	float s2 = (-2.0f*q0)*f0 + (2.0f*q3)*f1 + (-4.0f*q2)*f2;
	float s3 = (2.0f*q1)*f0 + (2.0f*q2)*f1;

	/* Normalize step */
	float sn = sqrtf(s0*s0 + s1*s1 + s2*s2 + s3*s3);
	float inv_sn = (sn > 0.0f) ? (1.0f / sn) : 0.0f;

	/* Rate of change: q_dot = 0.5*q⊗omega - beta*step */
	float beta = MOTION_FUSION_BETA;
	float dw = 0.5f*(-(q1*gx + q2*gy + q3*gz)) - beta*s0*inv_sn;
	float dx = 0.5f*( q0*gx + q2*gz - q3*gy)  - beta*s1*inv_sn;
	float dy = 0.5f*( q0*gy - q1*gz + q3*gx)  - beta*s2*inv_sn;
	float dz = 0.5f*( q0*gz + q1*gy - q2*gx)  - beta*s3*inv_sn;

	/* Integrate */
	st->q.w = q0 + dw * dt;
	st->q.x = q1 + dx * dt;
	st->q.y = q2 + dy * dt;
	st->q.z = q3 + dz * dt;

	return motion_quat_normalize(&st->q);
}

/* ---- 9DoF gradient step (accelerometer + magnetometer) --------------- */

static motion_result_t fusion_step_9dof(motion_fusion_state_t *st,
                                        float gx, float gy, float gz,
                                        float ax, float ay, float az,
                                        float mx, float my, float mz,
                                        float dt) {
	float q0 = st->q.w, q1 = st->q.x, q2 = st->q.y, q3 = st->q.z;

	/* Normalize accelerometer */
	if (motion_normalize3(&ax, &ay, &az) == 0.0f)
		return MOTION_RESULT_SENSOR_FAULT;

	/* Normalize magnetometer */
	if (motion_normalize3(&mx, &my, &mz) == 0.0f)
		return MOTION_RESULT_SENSOR_FAULT;

	float bx = st->ref_bx;
	float bz = st->ref_bz;

	/* Objective function (accel + mag) */
	float f0 = 2.0f*(q1*q3 - q0*q2) - ax;
	float f1 = 2.0f*(q0*q1 + q2*q3) - ay;
	float f2 = 2.0f*(0.5f - q1*q1 - q2*q2) - az;
	float f3 = 2.0f*bx*(0.5f - q2*q2 - q3*q3) + 2.0f*bz*(q1*q3 - q0*q2) - mx;
	float f4 = 2.0f*bx*(q1*q2 - q0*q3) + 2.0f*bz*(q0*q1 + q2*q3) - my;
	float f5 = 2.0f*bx*(q0*q2 + q1*q3) + 2.0f*bz*(0.5f - q1*q1 - q2*q2) - mz;

	/* Gradient = J^T * f (expanded from published 6x4 Jacobian) */
	float s0 = (-2.0f*q2)*f0 + (2.0f*q1)*f1
	         + (-2.0f*bz*q2)*f3 + (-2.0f*bx*q3 + 2.0f*bz*q1)*f4
	         + (2.0f*bx*q2)*f5;
	float s1 = (2.0f*q3)*f0 + (2.0f*q0)*f1 + (-4.0f*q1)*f2
	         + (-2.0f*bz*q3)*f3 + (2.0f*bx*q2 + 2.0f*bz*q0)*f4
	         + (2.0f*bx*q3 - 4.0f*bz*q1)*f5;
	float s2 = (-2.0f*q0)*f0 + (2.0f*q3)*f1 + (-4.0f*q2)*f2
	         + (2.0f*bz*q0 - 4.0f*bx*q2)*f3
	             + (2.0f*bx*q1 + 2.0f*bz*q3)*f4
	             + (2.0f*bx*q0 - 4.0f*bz*q2)*f5;
	float s3 = (2.0f*q1)*f0 + (2.0f*q2)*f1
	         + (2.0f*bz*q1 - 4.0f*bx*q3)*f3
	             + (-2.0f*bx*q0 + 2.0f*bz*q2)*f4
	             + (2.0f*bx*q1)*f5;

	/* Normalize step */
	float sn = sqrtf(s0*s0 + s1*s1 + s2*s2 + s3*s3);
	float inv_sn = (sn > 0.0f) ? (1.0f / sn) : 0.0f;

	/* Rate of change */
	float beta = MOTION_FUSION_BETA;
	float dw = 0.5f*(-(q1*gx + q2*gy + q3*gz)) - beta*s0*inv_sn;
	float dx = 0.5f*( q0*gx + q2*gz - q3*gy)  - beta*s1*inv_sn;
	float dy = 0.5f*( q0*gy - q1*gz + q3*gx)  - beta*s2*inv_sn;
	float dz = 0.5f*( q0*gz + q1*gy - q2*gx)  - beta*s3*inv_sn;

	/* Integrate */
	st->q.w = q0 + dw * dt;
	st->q.x = q1 + dx * dt;
	st->q.y = q2 + dy * dt;
	st->q.z = q3 + dz * dt;

	return motion_quat_normalize(&st->q);
}

/* ---- Combined update ------------------------------------------------- */

motion_result_t motion_fusion_update(motion_fusion_state_t *st,
                                     float gx, float gy, float gz,
                                     float ax, float ay, float az,
                                     float mx, float my, float mz,
                                     bool mag_valid,
                                     uint64_t now_us,
                                     float dt) {
	(void)now_us;  /* timestamp consumed by motion_mag_health_eval */
	if (!motion_finite3(gx, gy, gz) || !motion_finite3(ax, ay, az) ||
	    !motion_finite_f(dt) || dt <= 0.0f || dt > 1.0f)
		return MOTION_RESULT_SENSOR_FAULT;

	if (st->mode == MOTION_FUSION_MODE_9DOF && mag_valid && st->ref_set) {
		if (!motion_finite3(mx, my, mz))
			return MOTION_RESULT_SENSOR_FAULT;
		return fusion_step_9dof(st, gx, gy, gz, ax, ay, az,
		                        mx, my, mz, dt);
	}
	return fusion_step_6dof(st, gx, gy, gz, ax, ay, az, dt);
}

/* ---- Quaternion utilities -------------------------------------------- */

bool motion_quat_is_finite(const motion_quat_t *q) {
	return motion_finite3(q->w, q->x, q->y) && motion_finite_f(q->z);
}

motion_result_t motion_quat_normalize(motion_quat_t *q) {
	float norm = motion_normalize4(&q->w, &q->x, &q->y, &q->z);
	if (norm == 0.0f || !motion_quat_is_finite(q))
		return MOTION_RESULT_SENSOR_FAULT;
	return MOTION_RESULT_OK;
}

motion_result_t motion_quat_to_q30(const motion_quat_t *q, int32_t out[4]) {
	if (!motion_quat_is_finite(q))
		return MOTION_RESULT_SENSOR_FAULT;
	float vals[4] = { q->w, q->x, q->y, q->z };
	for (int i = 0; i < 4; i++) {
		double scaled = (double)vals[i] * 1073741824.0;  /* 2^30 */
		if (scaled >= 2147483647.0)
			out[i] = INT32_MAX;
		else if (scaled <= -2147483648.0)
			out[i] = INT32_MIN;
		else
			out[i] = (int32_t)scaled;
	}
	return MOTION_RESULT_OK;
}

motion_result_t motion_quat_to_euler_millideg(const motion_quat_t *q,
                                              int32_t *yaw_mdeg,
                                              int32_t *pitch_mdeg,
                                              int32_t *roll_mdeg) {
	if (!motion_quat_is_finite(q))
		return MOTION_RESULT_SENSOR_FAULT;
	float q0 = q->w, q1 = q->x, q2 = q->y, q3 = q->z;
	float roll  = atan2f(2.0f*(q0*q1 + q2*q3), 1.0f - 2.0f*(q1*q1 + q2*q2));
	float sp    = 2.0f*(q0*q2 - q3*q1);
	if (sp > 1.0f) sp = 1.0f;
	if (sp < -1.0f) sp = -1.0f;
	float pitch = asinf(sp);
	float yaw   = atan2f(2.0f*(q0*q3 + q1*q2), 1.0f - 2.0f*(q2*q2 + q3*q3));
	*roll_mdeg  = (int32_t)(roll  * 57295.77951308232);  /* rad → millideg */
	*pitch_mdeg = (int32_t)(pitch * 57295.77951308232);
	*yaw_mdeg   = (int32_t)(yaw   * 57295.77951308232);
	return MOTION_RESULT_OK;
}

motion_result_t motion_gravity_from_quat(const motion_quat_t *q, float g_out[3]) {
	if (!motion_quat_is_finite(q))
		return MOTION_RESULT_SENSOR_FAULT;
	float q0 = q->w, q1 = q->x, q2 = q->y, q3 = q->z;
	g_out[0] = 2.0f*(q1*q3 - q0*q2);
	g_out[1] = 2.0f*(q0*q1 + q2*q3);
	g_out[2] = 1.0f - 2.0f*q1*q1 - 2.0f*q2*q2;
	return MOTION_RESULT_OK;
}

float motion_vec3_dot(const float a[3], const float b[3]) {
	return a[0]*b[0] + a[1]*b[1] + a[2]*b[2];
}

/* ---- Air mouse ------------------------------------------------------- */

void motion_air_mouse_init(motion_air_mouse_state_t *s) {
	s->carry_h = 0.0f; s->carry_v = 0.0f;
	s->dx = 0; s->dy = 0;
	s->enabled = false;
}

void motion_air_mouse_enable(motion_air_mouse_state_t *s) {
	s->carry_h = 0.0f; s->carry_v = 0.0f;
	s->dx = 0; s->dy = 0;
	s->enabled = true;
}

void motion_air_mouse_disable(motion_air_mouse_state_t *s) {
	s->enabled = false;
	s->dx = 0; s->dy = 0;
	s->carry_h = 0.0f; s->carry_v = 0.0f;
}

float motion_air_mouse_sensitivity(float rate_magnitude_dps) {
	if (rate_magnitude_dps <= MOTION_AIR_MOUSE_RAMP_START)
		return MOTION_AIR_MOUSE_SENS_BASE;
	if (rate_magnitude_dps >= MOTION_AIR_MOUSE_RAMP_END)
		return MOTION_AIR_MOUSE_SENS_HIGH;
	float t = (rate_magnitude_dps - MOTION_AIR_MOUSE_RAMP_START) /
	          (MOTION_AIR_MOUSE_RAMP_END - MOTION_AIR_MOUSE_RAMP_START);
	t = t * t * (3.0f - 2.0f * t);  /* smoothstep */
	return MOTION_AIR_MOUSE_SENS_BASE +
	       t * (MOTION_AIR_MOUSE_SENS_HIGH - MOTION_AIR_MOUSE_SENS_BASE);
}

motion_result_t motion_air_mouse_update(motion_air_mouse_state_t *s,
                                        float rate_h_dps,
                                        float rate_v_dps,
                                        float dt) {
	if (!motion_finite_f(rate_h_dps) || !motion_finite_f(rate_v_dps) ||
	    !motion_finite_f(dt) || dt <= 0.0f)
		return MOTION_RESULT_SENSOR_FAULT;
	if (!s->enabled) {
		s->dx = 0; s->dy = 0;
		return MOTION_RESULT_OK;
	}

	float rh = rate_h_dps, rv = rate_v_dps;
	/* Dead zone */
	if (rh > -MOTION_AIR_MOUSE_DEADZONE_DPS && rh < MOTION_AIR_MOUSE_DEADZONE_DPS)
		rh = 0.0f;
	if (rv > -MOTION_AIR_MOUSE_DEADZONE_DPS && rv < MOTION_AIR_MOUSE_DEADZONE_DPS)
		rv = 0.0f;

	float mag = sqrtf(rh*rh + rv*rv);
	float sens = motion_air_mouse_sensitivity(mag);

	/* Integrate rotation → degrees → pixels */
	s->carry_h += rh * dt * sens;
	s->carry_v += rv * dt * sens;

	/* Extract integer delta, preserve fractional carry */
	float dx_f = truncf(s->carry_h);
	float dy_f = truncf(s->carry_v);
	s->carry_h -= dx_f;
	s->carry_v -= dy_f;

	/* Clamp to int16 */
	if (dx_f > 32767.0f) dx_f = 32767.0f;
	if (dx_f < -32768.0f) dx_f = -32768.0f;
	if (dy_f > 32767.0f) dy_f = 32767.0f;
	if (dy_f < -32768.0f) dy_f = -32768.0f;

	s->dx = (int16_t)dx_f;
	s->dy = (int16_t)dy_f;
	return MOTION_RESULT_OK;
}

/* ---- Tilt-to-D-pad --------------------------------------------------- */

void motion_tilt_init(motion_tilt_state_t *s) {
	s->current = MOTION_TILT_DIR_NONE;
}

static motion_tilt_dir_t tilt_classify(float pitch_deg, float roll_deg) {
	/* Priority: pitch over roll. Only one direction at a time. */
	float ap = (pitch_deg < 0.0f) ? -pitch_deg : pitch_deg;
	float ar = (roll_deg  < 0.0f) ? -roll_deg  : roll_deg;
	if (ap >= ar) {
		if (pitch_deg >= MOTION_TILT_ENGAGE_DEG) return MOTION_TILT_DIR_UP;
		if (pitch_deg <= -MOTION_TILT_ENGAGE_DEG) return MOTION_TILT_DIR_DOWN;
	} else {
		if (roll_deg >= MOTION_TILT_ENGAGE_DEG) return MOTION_TILT_DIR_RIGHT;
		if (roll_deg <= -MOTION_TILT_ENGAGE_DEG) return MOTION_TILT_DIR_LEFT;
	}
	return MOTION_TILT_DIR_NONE;
}

static bool tilt_should_release(motion_tilt_dir_t dir,
                                float pitch_deg, float roll_deg) {
	float ap = (pitch_deg < 0.0f) ? -pitch_deg : pitch_deg;
	float ar = (roll_deg  < 0.0f) ? -roll_deg  : roll_deg;
	switch (dir) {
	case MOTION_TILT_DIR_UP:    return ap < MOTION_TILT_RELEASE_DEG;
	case MOTION_TILT_DIR_DOWN:  return ap < MOTION_TILT_RELEASE_DEG;
	case MOTION_TILT_DIR_LEFT:  return ar < MOTION_TILT_RELEASE_DEG;
	case MOTION_TILT_DIR_RIGHT: return ar < MOTION_TILT_RELEASE_DEG;
	default: return true;
	}
}

void motion_tilt_update(motion_tilt_state_t *s,
                        float pitch_deg, float roll_deg,
                        motion_tilt_event_t *event) {
	event->pressed = MOTION_TILT_DIR_NONE;
	event->released = MOTION_TILT_DIR_NONE;

	if (s->current != MOTION_TILT_DIR_NONE) {
		if (tilt_should_release(s->current, pitch_deg, roll_deg)) {
			event->released = s->current;
			s->current = MOTION_TILT_DIR_NONE;
		}
	}
	if (s->current == MOTION_TILT_DIR_NONE) {
		motion_tilt_dir_t d = tilt_classify(pitch_deg, roll_deg);
		if (d != MOTION_TILT_DIR_NONE) {
			s->current = d;
			event->pressed = d;
		}
	}
}
