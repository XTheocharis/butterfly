/*
 * buzzer_eval.c - Pure-logic buzzer/output dispatch evaluation.
 *
 * No SDK deps. Included from boardModule.cpp (firmware) and
 * test_buzzer.cpp (host test).
 */
#include "buzzer_eval.h"

void buzz_fsm_init(buzz_fsm_t *st)
{
	if (st == NULL) {
		return;
	}
	st->state        = BUZZ_STATE_IDLE;
	st->freq_hz      = 0;
	st->start_ms     = 0;
	st->duration_ms  = 0;
}

bool buzz_validate_freq(uint32_t freq_hz)
{
	return (freq_hz >= BUZZ_FREQ_MIN_HZ && freq_hz <= BUZZ_FREQ_MAX_HZ);
}

bool buzz_validate_duration(uint32_t duration_ms)
{
	return (duration_ms > 0u && duration_ms <= BUZZ_DURATION_MAX_MS);
}

uint32_t buzz_freq_to_top(uint32_t freq_hz)
{
	if (!buzz_validate_freq(freq_hz)) {
		return 0u;
	}
	return (BUZZ_PWM_TICK_HZ / freq_hz) - 1u;
}

uint16_t buzz_duty_50pct(uint32_t top)
{
	if (top == 0u) {
		return 0u;
	}
	uint32_t duty = (top + 1u) / 2u;
	if (duty > top) {
		duty = top;
	}
	return (uint16_t)duty;
}

uint32_t buzz_fsm_start(buzz_fsm_t *st, uint32_t freq_hz,
                        uint32_t duration_ms, uint32_t now_ms)
{
	if (st == NULL) {
		return BUZZ_EVAL_INVALID_ARG;
	}
	if (!buzz_validate_freq(freq_hz)) {
		return BUZZ_EVAL_INVALID_ARG;
	}
	if (!buzz_validate_duration(duration_ms)) {
		return BUZZ_EVAL_INVALID_ARG;
	}

	/* Stop semantics: a new start replaces any active tone.
	 * The caller's PWM driver unconditionally restarts the playback. */
	st->state       = BUZZ_STATE_PLAYING;
	st->freq_hz     = freq_hz;
	st->start_ms    = now_ms;
	st->duration_ms = duration_ms;
	return BUZZ_EVAL_SUCCESS;
}

void buzz_fsm_stop(buzz_fsm_t *st)
{
	if (st == NULL) {
		return;
	}
	st->state       = BUZZ_STATE_IDLE;
	st->freq_hz     = 0;
	st->start_ms    = 0;
	st->duration_ms = 0;
}

bool buzz_fsm_tick(buzz_fsm_t *st, uint32_t now_ms)
{
	if (st == NULL) {
		return false;
	}
	if (st->state != BUZZ_STATE_PLAYING) {
		return false;
	}
	/* duration_ms == 0 means indefinite — never auto-completes.
	 * (buzz_fsm_start rejects 0, but tick guards anyway.) */
	if (st->duration_ms == 0u) {
		return false;
	}
	if ((now_ms - st->start_ms) >= st->duration_ms) {
		st->state       = BUZZ_STATE_IDLE;
		st->freq_hz     = 0;
		st->start_ms    = 0;
		st->duration_ms = 0;
		return true;
	}
	return false;
}

bool buzz_fsm_is_playing(const buzz_fsm_t *st)
{
	return (st != NULL && st->state == BUZZ_STATE_PLAYING);
}

/* ---- SetOutput validation ------------------------------------------- */

bool buzz_output_target_valid(uint32_t target)
{
	return (target >= BUZZ_OUTPUT_BUZZER && target <= BUZZ_OUTPUT_BACKLIGHT);
}

uint32_t buzz_eval_set_output(uint32_t target, uint32_t value,
                              uint32_t duration_ms, bool stop)
{
	/* stop=true means "silence this output" — always valid for known
	 * outputs. (Unknown target → INVALID_ARGUMENT.) */
	if (!buzz_output_target_valid(target)) {
		return BUZZ_EVAL_INVALID_ARG;
	}

	if (stop) {
		return BUZZ_EVAL_SUCCESS;
	}

	switch (target) {
	case BUZZ_OUTPUT_BUZZER:
		/* value=frequency in Hz, duration_ms required. */
		if (!buzz_validate_freq(value)) {
			return BUZZ_EVAL_INVALID_ARG;
		}
		if (!buzz_validate_duration(duration_ms)) {
			return BUZZ_EVAL_INVALID_ARG;
		}
		return BUZZ_EVAL_SUCCESS;

	case BUZZ_OUTPUT_NEOPIXEL:
		/* value=packed RGB 0x00RRGGBB. Any uint32 is a valid color. */
		(void)value;
		(void)duration_ms;
		return BUZZ_EVAL_SUCCESS;

	case BUZZ_OUTPUT_WHITE_LED:
	case BUZZ_OUTPUT_RED_LED:
		/* value=0 (off) or 1 (on). */
		if (value > 1u) {
			return BUZZ_EVAL_INVALID_ARG;
		}
		return BUZZ_EVAL_SUCCESS;

	case BUZZ_OUTPUT_BACKLIGHT:
		/* value=0 (off), >0 (on). Any non-zero turns on. */
		(void)value;
		return BUZZ_EVAL_SUCCESS;

	default:
		return BUZZ_EVAL_INVALID_ARG;
	}
}

/* ---- Status color mapping ------------------------------------------- */

buzz_rgb_t buzz_status_to_rgb(buzz_status_color_t color)
{
	buzz_rgb_t rgb = {0, 0, 0};
	switch (color) {
	case BUZZ_STATUS_OK:      rgb.r =   0; rgb.g = 255; rgb.b =   0; break;
	case BUZZ_STATUS_ERROR:   rgb.r = 255; rgb.g =   0; rgb.b =   0; break;
	case BUZZ_STATUS_WARNING: rgb.r = 255; rgb.g = 255; rgb.b =   0; break;
	case BUZZ_STATUS_INFO:    rgb.r =   0; rgb.g =   0; rgb.b = 255; break;
	case BUZZ_STATUS_ARM:     rgb.r =   0; rgb.g = 255; rgb.b = 255; break;
	case BUZZ_STATUS_CANCEL:  rgb.r = 255; rgb.g =   0; rgb.b = 255; break;
	case BUZZ_STATUS_OFF:
	default:                  rgb.r =   0; rgb.g =   0; rgb.b =   0; break;
	}
	return rgb;
}
