/*
 * tilt.cpp - Tilt-to-D-pad navigation firmware wrapper.
 *
 * Derives pitch/roll angles from the gravity vector projected from
 * the current fusion quaternion, then feeds them to the eval hysteresis
 * FSM.
 */
#include "tilt.h"
#include <math.h>

TiltNav::TiltNav() {
	motion_tilt_init(&m_state);
}

void TiltNav::init() {
	motion_tilt_init(&m_state);
}

static float motion_atan2f_safe(float y, float x) {
	if (x == 0.0f && y == 0.0f) return 0.0f;
	return atan2f(y, x);
}

void TiltNav::update(const motion_quat_t *q, motion_tilt_event_t *event) {
	float g[3];
	if (motion_gravity_from_quat(q, g) != MOTION_RESULT_OK) {
		event->pressed = MOTION_TILT_DIR_NONE;
		event->released = MOTION_TILT_DIR_NONE;
		return;
	}
	/* Pitch = angle of gravity from vertical in the YZ plane.
	 * Roll  = angle of gravity from vertical in the XZ plane.
	 * Convert radians to degrees. */
	float pitch_rad = motion_atan2f_safe(g[1], g[2]);
	float roll_rad  = motion_atan2f_safe(-g[0], g[2]);
	float pitch_deg = pitch_rad * 57.295779513f;
	float roll_deg  = roll_rad  * 57.295779513f;
	motion_tilt_update(&m_state, pitch_deg, roll_deg, event);
}
