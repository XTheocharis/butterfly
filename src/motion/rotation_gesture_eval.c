/*
 * rotation_gesture_eval.c - Pure-C rotation gesture FSM implementation.
 *
 * No SDK deps. Linked on host (test) and device (firmware).
 *
 * Implementation notes:
 *   - dot product sign conventions:
 *       +1.0 = gravity_now points the same way as reference
 *       -1.0 = gravity_now points opposite (device flipped)
 *   - Dwell timer resets whenever the dot product leaves the qualifying
 *     band. The flip must be held steadily inverted for the full dwell.
 *   - The FSM only enters CONFIRM_PENDING on a full A+B release. A or B
 *     held alone during ARMED continues to qualify for INVERTED/RETURNED
 *     detection — the gesture is performed while holding the chord.
 */
#include "rotation_gesture_eval.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Dot product helper --------------------------------------------- */

static float rotg_dot3(const float a[3], const float b[3]) {
	if (a == NULL || b == NULL) return 0.0f;
	return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* ---- Lifecycle ------------------------------------------------------- */

void rotg_init(rotg_state_machine_t *sm) {
	if (sm == NULL) return;
	memset(sm, 0, sizeof(*sm));
	sm->state = ROTG_STATE_IDLE;
	sm->has_ref = false;
}

void rotg_cancel(rotg_state_machine_t *sm) {
	if (sm == NULL) return;
	sm->state = ROTG_STATE_IDLE;
	sm->has_ref = false;
	sm->arm_chord_start_us = 0;
	sm->armed_at_us = 0;
	sm->dwell_start_us = 0;
	sm->confirm_start_us = 0;
}

rotg_state_t rotg_get_state(const rotg_state_machine_t *sm) {
	if (sm == NULL) return ROTG_STATE_IDLE;
	return sm->state;
}

/* ---- FSM ------------------------------------------------------------ */

void rotg_tick(rotg_state_machine_t *sm,
                bool button_a, bool button_b,
                const float gravity_now[3],
                uint64_t now_us,
                rotg_event_t *out_event)
{
	if (sm == NULL || out_event == NULL) return;
	*out_event = ROTG_EVENT_NONE;

	bool chord = button_a && button_b;

	switch (sm->state) {

	case ROTG_STATE_IDLE: {
		if (chord) {
			sm->state = ROTG_STATE_ARMING;
			sm->arm_chord_start_us = now_us;
		}
		break;
	}

	case ROTG_STATE_ARMING: {
		if (!chord) {
			/* Released early — return to IDLE without firing. */
			sm->state = ROTG_STATE_IDLE;
			break;
		}
		if ((now_us - sm->arm_chord_start_us) >= ROTG_ARM_CHORD_US) {
			/* Capture reference. If gravity_now is unavailable this
			 * tick, delay arming by leaving the state as ARMING —
			 * the chord is still held so we'll retry next tick. */
			if (gravity_now != NULL) {
				sm->gravity_ref[0] = gravity_now[0];
				sm->gravity_ref[1] = gravity_now[1];
				sm->gravity_ref[2] = gravity_now[2];
				sm->has_ref = true;
				sm->state = ROTG_STATE_ARMED;
				sm->armed_at_us = now_us;
				sm->dwell_start_us = now_us;
				*out_event = ROTG_EVENT_ARMED;
			}
		}
		break;
	}

	case ROTG_STATE_ARMED: {
		/* Window expiry — return to IDLE silently. */
		if ((now_us - sm->armed_at_us) >= ROTG_WINDOW_US) {
			sm->state = ROTG_STATE_IDLE;
			sm->has_ref = false;
			break;
		}

		/* If both buttons released, leave ARMED without firing.
		 * (The flip must be completed while the chord is held.) */
		if (!chord) {
			sm->state = ROTG_STATE_IDLE;
			sm->has_ref = false;
			break;
		}

		/* No fresh gravity this tick — keep dwell reset at the last
		 * good timestamp by skipping the comparison. */
		if (gravity_now == NULL) {
			sm->dwell_start_us = now_us;
			break;
		}

		float dot = rotg_dot3(gravity_now, sm->gravity_ref);
		if (dot <= ROTG_FLIP_DOT_NEG) {
			if ((now_us - sm->dwell_start_us) >= ROTG_DWELL_US) {
				sm->state = ROTG_STATE_INVERTED;
				sm->dwell_start_us = now_us;
				*out_event = ROTG_EVENT_INVERTED;
			}
		} else {
			/* Not inverted — dwell timer resets. */
			sm->dwell_start_us = now_us;
		}
		break;
	}

	case ROTG_STATE_INVERTED: {
		/* Wait for return (dot ≥ +0.75) sustained for dwell. If the
		 * armed window expires first, abort. */
		if ((now_us - sm->armed_at_us) >= ROTG_WINDOW_US) {
			sm->state = ROTG_STATE_IDLE;
			sm->has_ref = false;
			break;
		}
		if (!chord) {
			/* Released early — abandon. */
			sm->state = ROTG_STATE_IDLE;
			sm->has_ref = false;
			break;
		}
		if (gravity_now == NULL) {
			sm->dwell_start_us = now_us;
			break;
		}
		float dot = rotg_dot3(gravity_now, sm->gravity_ref);
		if (dot >= ROTG_FLIP_DOT_POS) {
			if ((now_us - sm->dwell_start_us) >= ROTG_DWELL_US) {
				*out_event = ROTG_EVENT_RETURNED;
				/* Transition to RETURNED — wait for chord release
				 * without further dot checks. */
				sm->state = ROTG_STATE_RETURNED;
			}
		} else {
			sm->dwell_start_us = now_us;
		}
		break;
	}

	case ROTG_STATE_RETURNED: {
		/* Return detected; wait for full chord release. Window still
		 * applies — if the user keeps holding past the armed window,
		 * cancel silently. */
		if ((now_us - sm->armed_at_us) >= ROTG_WINDOW_US) {
			sm->state = ROTG_STATE_IDLE;
			sm->has_ref = false;
			break;
		}
		if (!chord) {
			sm->state = ROTG_STATE_CONFIRM_PENDING;
			sm->confirm_start_us = now_us;
			*out_event = ROTG_EVENT_CONFIRM_REQ;
		}
		break;
	}

	case ROTG_STATE_CONFIRM_PENDING: {
		/* Timeout → cancel. */
		if ((now_us - sm->confirm_start_us) >= ROTG_CONFIRM_TIMEOUT_US) {
			sm->state = ROTG_STATE_IDLE;
			sm->has_ref = false;
			*out_event = ROTG_EVENT_CANCELLED;
			break;
		}
		/* A press → confirm. B press → cancel. */
		if (button_a && !button_b) {
			sm->state = ROTG_STATE_IDLE;
			sm->has_ref = false;
			*out_event = ROTG_EVENT_SWITCH_CONFIRM;
			break;
		}
		if (button_b && !button_a) {
			sm->state = ROTG_STATE_IDLE;
			sm->has_ref = false;
			*out_event = ROTG_EVENT_CANCELLED;
			break;
		}
		break;
	}

	default:
		sm->state = ROTG_STATE_IDLE;
		sm->has_ref = false;
		break;
	}
}

#ifdef __cplusplus
}
#endif
