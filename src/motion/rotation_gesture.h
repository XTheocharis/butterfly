/*
 * rotation_gesture.h - Runtime rotation gesture firmware wrapper.
 *
 * Wraps rotation_gesture_eval (pure-C FSM) with a quaternion-driven
 * gravity computation via GesturePrimitive. Caller feeds button state
 * and current fusion quaternion each tick; wrapper computes gravity
 * and forwards to the eval FSM.
 *
 * On SWITCH_CONFIRM, the wrapper invokes the runtime switch callback
 * set by BoardModule.
 */
#ifndef ROTATION_GESTURE_H
#define ROTATION_GESTURE_H

#include "rotation_gesture_eval.h"
#include "motion_eval.h"

#ifdef __cplusplus

#include <stdint.h>

/* Signature of the runtime-switch callback fired on SWITCH_CONFIRM.
 * target_mode is supplied by the caller (typically the opposite of the
 * current runtime mode). */
typedef void (*rotg_switch_fn)(void *user);

class RotationGesture {
public:
	RotationGesture();

	void init();

	/* Set the callback fired when the user confirms a switch.
	 * Called from tick() context — must be non-blocking. */
	void setSwitchCallback(rotg_switch_fn fn, void *user);

	/* Advance one tick.
	 *   button_a, button_b : current button states
	 *   q                  : current fusion quaternion (NULL if stale)
	 *   now_us             : monotonic timestamp
	 * Returns the event emitted this tick (ROTG_EVENT_NONE most ticks). */
	rotg_event_t tick(bool button_a, bool button_b,
	                  const motion_quat_t *q, uint64_t now_us);

	rotg_state_t getState() const { return rotg_get_state(&m_sm); }

	/* Force-cancel from outside (e.g. application override). */
	void cancel() { rotg_cancel(&m_sm); }

private:
	rotg_state_machine_t m_sm;
	rotg_switch_fn       m_switchFn;
	void                *m_switchUser;
};

#endif /* __cplusplus */
#endif /* ROTATION_GESTURE_H */
