/*
 * buzzer_eval.h - Pure-logic buzzer/output dispatch evaluation.
 *
 * No SDK deps — safe for host tests. Included from boardModule.cpp
 * (firmware) and test_buzzer.cpp (host test).
 *
 * Provides:
 *   - Frequency validation (200-4000Hz) and PWM1 top calculation
 *   - Nonblocking duration FSM (idle → playing → complete)
 *   - Stop/replace semantics
 *   - Status color mapping (generic → NeoPixel RGB)
 *   - SetOutput request validation per OutputTarget
 *
 * Buzzer hardware: PWM1 (NOT PWM0 — PWM0 owns the NeoPixel).
 *   base clock 16 MHz, prescaler 2^4 → 1 MHz tick.
 *   COUNTERTOP = (1000000 / freq) − 1 gives 50% duty cycle
 *   (the nRF PWM up-mode drives the pin symmetrically around TOP/2).
 */
#ifndef OUTPUT_BUZZER_EVAL_H
#define OUTPUT_BUZZER_EVAL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Local BoardResultCode copies (avoid board.pb.h in host tests) ---
 * Mirror board_BoardResultCode_* exactly; see board.pb.h. */
#define BUZZ_EVAL_SUCCESS          0u
#define BUZZ_EVAL_WRONG_MODE       1u
#define BUZZ_EVAL_INVALID_ARG      2u
#define BUZZ_EVAL_PERMISSION       3u
#define BUZZ_EVAL_WRONG_STATE      4u
#define BUZZ_EVAL_NOT_ADOPTED      5u
#define BUZZ_EVAL_BUSY             6u
#define BUZZ_EVAL_NOT_IMPLEMENTED  14u

/* ---- Buzzer hardware constants --------------------------------------- */
/* PWM1 base clock is 16 MHz; prescaler /16 → 1 MHz tick.
 * 50% duty cycle: CC[0] = (top+1) / 2 — caller writes top to COUNTERTOP. */
#define BUZZ_PWM_BASE_CLK_HZ       16000000u
#define BUZZ_PWM_PRESCALER_DIV     16u
#define BUZZ_PWM_TICK_HZ           (BUZZ_PWM_BASE_CLK_HZ / BUZZ_PWM_PRESCALER_DIV)

#define BUZZ_FREQ_MIN_HZ           200u
#define BUZZ_FREQ_MAX_HZ           4000u
#define BUZZ_DURATION_MAX_MS       65535u

/* ---- OutputTarget (matches board.pb.h, 1-based) --------------------- */
#define BUZZ_OUTPUT_UNKNOWN        0u
#define BUZZ_OUTPUT_BUZZER         1u
#define BUZZ_OUTPUT_NEOPIXEL       2u
#define BUZZ_OUTPUT_WHITE_LED      3u
#define BUZZ_OUTPUT_RED_LED        4u
#define BUZZ_OUTPUT_BACKLIGHT      5u

/* ---- Status colors (runtime-gesture feedback) ----------------------- */
typedef enum {
	BUZZ_STATUS_OFF = 0,
	BUZZ_STATUS_OK,        /* green    — confirm/armed   */
	BUZZ_STATUS_ERROR,     /* red      — fault           */
	BUZZ_STATUS_WARNING,   /* yellow   — debounce/pending*/
	BUZZ_STATUS_INFO,      /* blue     — discoverable    */
	BUZZ_STATUS_ARM,       /* cyan     — runtime switch  */
	BUZZ_STATUS_CANCEL     /* magenta  — switch canceled */
} buzz_status_color_t;

typedef struct {
	uint8_t r;
	uint8_t g;
	uint8_t b;
} buzz_rgb_t;

/* ---- Buzzer FSM state ----------------------------------------------- */
typedef enum {
	BUZZ_STATE_IDLE = 0,
	BUZZ_STATE_PLAYING
} buzz_state_t;

typedef struct {
	buzz_state_t state;
	uint32_t    freq_hz;       /* active frequency */
	uint32_t    start_ms;      /* monotonic ms at start */
	uint32_t    duration_ms;   /* 0 = indefinite until stop */
} buzz_fsm_t;

/* ---- API ------------------------------------------------------------- */

/* Initialize FSM to idle. */
void buzz_fsm_init(buzz_fsm_t *st);

/* Validate buzzer frequency. Returns true iff 200 <= freq <= 4000. */
bool buzz_validate_freq(uint32_t freq_hz);

/* Validate buzzer duration. 0 is invalid; >65535 invalid. */
bool buzz_validate_duration(uint32_t duration_ms);

/* Compute PWM1 COUNTERTOP for a given frequency.
 * Returns 0 for invalid frequencies (caller must validate first).
 * top = (BUZZ_PWM_TICK_HZ / freq) - 1.
 */
uint32_t buzz_freq_to_top(uint32_t freq_hz);

/* Compute CC[0] duty value for 50% duty at given top.
 * Returns (top+1)/2 — clamps to top for odd-tick periods. */
uint16_t buzz_duty_50pct(uint32_t top);

/* Start a tone. Returns BoardResultCode:
 *   INVALID_ARGUMENT  bad frequency/duration
 *   SUCCESS           started (or replaced active tone)
 * Caller passes the monotonic ms timestamp.
 */
uint32_t buzz_fsm_start(buzz_fsm_t *st, uint32_t freq_hz,
                        uint32_t duration_ms, uint32_t now_ms);

/* Stop the active tone. Returns to IDLE. Idempotent. */
void buzz_fsm_stop(buzz_fsm_t *st);

/* Poll for completion. If state is PLAYING and duration has elapsed
 * (duration_ms != 0 and now-start >= duration), returns true and
 * transitions to IDLE. duration_ms == 0 means indefinite — never
 * auto-completes. */
bool buzz_fsm_tick(buzz_fsm_t *st, uint32_t now_ms);

/* Returns true iff the buzzer is actively playing. */
bool buzz_fsm_is_playing(const buzz_fsm_t *st);

/* ---- SetOutput validation ------------------------------------------- */

/* Map a board_OutputTarget enum to our internal constants.
 * Returns true iff target is a known non-UNKNOWN target. */
bool buzz_output_target_valid(uint32_t target);

/* Validate a SetOutput request for the given target.
 *
 *  BUZZER:     value=freq_hz (200-4000), duration_ms (1-65535, 0=bad)
 *  NEOPIXEL:   value=packed RGB (0x00RRGGBB), duration_ms ignored
 *  WHITE_LED:  value=0(off)/1(on), duration_ms ignored
 *  RED_LED:    value=0(off)/1(on), duration_ms ignored
 *  BACKLIGHT:  value=0(off)/>0(on), duration_ms ignored
 *
 * Returns BUZZ_EVAL_* BoardResultCode.
 */
uint32_t buzz_eval_set_output(uint32_t target, uint32_t value,
                              uint32_t duration_ms, bool stop);

/* ---- Status color mapping ------------------------------------------- */

/* Map a status color to RGB. Status OFF → (0,0,0). */
buzz_rgb_t buzz_status_to_rgb(buzz_status_color_t color);

#ifdef __cplusplus
}
#endif

#endif /* OUTPUT_BUZZER_EVAL_H */
