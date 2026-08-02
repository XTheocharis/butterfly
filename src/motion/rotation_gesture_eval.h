/*
 * rotation_gesture_eval.h - Pure-C rotation gesture FSM (host-testable).
 *
 * Detects a deliberate device-flip-and-return sequence armed by holding
 * both user buttons. On detect, fires a runtime-mode-switch event that
 * the firmware wrapper forwards to runtime_request_switch().
 *
 * Sequence (CLUE runtime rotation):
 *   IDLE → A+B held ≥1.5s → ARMED (gravity reference captured)
 *   ARMED → 4s window opens; each tick dot(gravity_ref, gravity_now):
 *     dot ≤ -0.75 sustained ≥150 ms → INVERTED
 *     dot ≥ +0.75 sustained ≥150 ms → RETURNED (only valid after INVERTED)
 *   Both buttons released → CONFIRM_PENDING (5s confirm window)
 *     A pressed   → confirm  (fire SWITCH event)
 *     B pressed   → cancel
 *     timeout     → cancel
 *
 * Thresholds are frozen. Do not retune without updating host tests.
 *
 * Compiled on BOTH host (for unit tests) and device (linked into firmware).
 */
#ifndef ROTATION_GESTURE_EVAL_H
#define ROTATION_GESTURE_EVAL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Frozen thresholds ------------------------------------------------ */

#define ROTG_ARM_CHORD_US        1500000ull  /* 1.5s A+B hold to arm */
#define ROTG_WINDOW_US           4000000ull  /* 4s armed window */
#define ROTG_DWELL_US            150000ull  /* 150ms sustained dot */
#define ROTG_CONFIRM_TIMEOUT_US  5000000ull  /* 5s confirm window */
#define ROTG_FLIP_DOT_NEG       (-0.75f)    /* flipped threshold */
#define ROTG_FLIP_DOT_POS        0.75f      /* returned threshold */

/* ---- FSM state ------------------------------------------------------- */

typedef enum {
	ROTG_STATE_IDLE            = 0,
	ROTG_STATE_ARMING          = 1,  /* A+B held, not yet armed */
	ROTG_STATE_ARMED           = 2,  /* armed, capturing flip */
	ROTG_STATE_INVERTED        = 3,  /* flip detected, awaiting return */
	ROTG_STATE_RETURNED        = 4,  /* return detected, awaiting chord release */
	ROTG_STATE_CONFIRM_PENDING = 5,  /* awaiting A=confirm / B=cancel */
} rotg_state_t;

/* ---- Events emitted by tick ------------------------------------------ */

typedef enum {
	ROTG_EVENT_NONE           = 0,
	ROTG_EVENT_ARMED          = 1,  /* reference captured, window opened */
	ROTG_EVENT_INVERTED       = 2,
	ROTG_EVENT_RETURNED       = 3,
	ROTG_EVENT_CONFIRM_REQ    = 4,  /* entered CONFIRM_PENDING — show prompt */
	ROTG_EVENT_SWITCH_CONFIRM = 5,  /* user pressed A → switch runtime */
	ROTG_EVENT_CANCELLED      = 6,  /* user cancelled / timed out */
} rotg_event_t;

/* ---- Target runtime mode to switch to -------------------------------- */
/*
 * The flip-and-return gesture toggles between the two runtimes. The
 * caller selects the target mode (typically: if currently raw, target
 * BLE; if currently BLE, target raw). The FSM does not care which.
 */

/* ---- State ----------------------------------------------------------- */

typedef struct {
	rotg_state_t state;
	uint64_t     arm_chord_start_us;   /* when A+B first pressed */
	uint64_t     armed_at_us;          /* when ARMED entered */
	uint64_t     dwell_start_us;       /* start of current dwell */
	uint64_t     confirm_start_us;     /* when CONFIRM_PENDING entered */
	float        gravity_ref[3];       /* captured at arming */
	bool         has_ref;
	bool         armed_button_a;       /* A held when entering CONFIRM_PENDING */
	bool         armed_button_b;       /* B held when entering CONFIRM_PENDING */
} rotg_state_machine_t;

void rotg_init(rotg_state_machine_t *sm);

/*
 * Advance the FSM one tick.
 *   button_a, button_b : current button states (true=pressed)
 *   gravity_now        : normalized gravity vector from current quaternion
 *                        (NULL if no fresh quaternion — FSM only times out)
 *   now_us             : monotonic timestamp
 *   out_event          : filled with the event for this tick (NONE if none)
 *
 * The FSM tracks the gravity reference at arming time. When armed, it
 * compares dot(gravity_ref, gravity_now) against the flip thresholds.
 * SWITCH_CONFIRM fires only on the A-press transition inside CONFIRM_PENDING.
 */
void rotg_tick(rotg_state_machine_t *sm,
                bool button_a, bool button_b,
                const float gravity_now[3],
                uint64_t now_us,
                rotg_event_t *out_event);

/* Convenience: peek current state for diagnostic / dashboard. */
rotg_state_t rotg_get_state(const rotg_state_machine_t *sm);

/* Force-cancel (e.g. application-level abort). Returns to IDLE. */
void rotg_cancel(rotg_state_machine_t *sm);

#ifdef __cplusplus
}
#endif
#endif /* ROTATION_GESTURE_EVAL_H */
