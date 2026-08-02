/*
 * tilt.h - Tilt-to-D-pad navigation firmware wrapper.
 *
 * Derives pitch/roll from fusion quaternion, applies hysteresis
 * (engage 25 degrees, release 17 degrees) to produce directional
 * D-pad events for consumer key mapping (Todo 21 HIDS).
 */
#ifndef MOTION_TILT_H
#define MOTION_TILT_H

#include "motion_eval.h"
#include "fusion.h"

#ifdef __cplusplus

class TiltNav {
public:
	TiltNav();

	void init();

	/* Update from fusion quaternion. Produces press/release events. */
	void update(const motion_quat_t *q, motion_tilt_event_t *event);

	motion_tilt_dir_t getCurrentDirection() const { return m_state.current; }

private:
	motion_tilt_state_t m_state;
};

#endif /* __cplusplus */
#endif /* MOTION_TILT_H */
