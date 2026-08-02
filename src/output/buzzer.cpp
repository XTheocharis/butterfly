/*
 * buzzer.cpp - Buzzer firmware wrapper (PWM1, 1 MHz tick).
 *
 * Drives the CLUE onboard speaker (P1.00) via PWM1 with EasyDMA.
 * PWM0 is reserved for the NeoPixel (led.cpp, Todo 8) — see
 * buzzer_eval.h for the constraint rationale.
 *
 * NOTE: custom_board.h's CLUE_SPEAKER_PWM_INSTANCE is 0 (incorrect —
 * that pin instance is owned by NeoPixel). We hardcode PWM1 here.
 */
#include "buzzer.h"

#ifdef BOARD_CLUE

#include "core.h"
#include "custom_board.h"
#include "nrf_gpio.h"
#include "nrfx_pwm.h"
#include "timebase.h"

/* PWM1 instance — separate from PWM0 (NeoPixel in led.cpp). */
static nrfx_pwm_t       buzzer_pwm_inst = NRFX_PWM_INSTANCE(1);
static bool             buzzer_pwm_ready = false;

/* EasyDMA sequence: single duty value, common-load, loop until stopped.
 * Bit 15 polarity set for active-HIGH compare. */
static nrf_pwm_values_common_t buzzer_seq_value;
static nrf_pwm_sequence_t      buzzer_seq;

static void buzzer_init_pwm(void)
{
	if (buzzer_pwm_ready) return;

	nrfx_pwm_config_t cfg = {};
	cfg.output_pins[0] = CLUE_SPEAKER;     /* P1.00 */
	cfg.output_pins[1] = NRFX_PWM_PIN_NOT_USED;
	cfg.output_pins[2] = NRFX_PWM_PIN_NOT_USED;
	cfg.output_pins[3] = NRFX_PWM_PIN_NOT_USED;
	cfg.irq_priority   = 6;
	/* 16 MHz / 2^4 = 1 MHz tick. */
	cfg.base_clock     = NRF_PWM_CLK_1MHz;
	cfg.count_mode     = NRF_PWM_MODE_UP;
	cfg.top_value      = 0;    /* set per-frequency at playback */
	cfg.load_mode      = NRF_PWM_LOAD_COMMON;
	cfg.step_mode      = NRF_PWM_STEP_AUTO;

	nrfx_pwm_init(&buzzer_pwm_inst, &cfg, NULL);
	buzzer_pwm_ready = true;
}

static void buzzer_apply_freq(uint32_t freq_hz)
{
	buzzer_init_pwm();

	uint32_t top  = buzz_freq_to_top(freq_hz);
	uint16_t duty = buzz_duty_50pct(top);

	/* nrf_pwm supports COUNTERTOP up to 32767 — guaranteed by our
	 * freq range (1MHz/200 = 5000, well below 32767). */
	nrf_pwm_configure(buzzer_pwm_inst.p_registers,
	                  NRF_PWM_CLK_1MHz, NRF_PWM_MODE_UP, (uint16_t)top);

	/* Active-HIGH polarity (bit 15) + duty compare. */
	buzzer_seq_value = (nrf_pwm_values_common_t)(0x8000u | duty);

	buzzer_seq.values.p_common = &buzzer_seq_value;
	buzzer_seq.length          = 1;
	buzzer_seq.repeats         = 0;
	buzzer_seq.end_delay       = 0;

	/* Loop until explicitly stopped. */
	nrfx_pwm_simple_playback(&buzzer_pwm_inst, &buzzer_seq, 0,
	                         NRFX_PWM_FLAG_SIGNAL_END_SEQ0 |
	                         NRFX_PWM_FLAG_LOOP);
}

static void buzzer_stop_pwm(void)
{
	if (!buzzer_pwm_ready) return;
	nrfx_pwm_stop(&buzzer_pwm_inst, true);
}

Buzzer::Buzzer(Core *core)
	: m_core(core)
{
	buzz_fsm_init(&m_fsm);
	m_pwmReady = false;
}

uint32_t Buzzer::m_nowMs(void)
{
	return timebase_now_ms();
}

void Buzzer::init(void)
{
	if (!m_pwmReady) {
		nrf_gpio_cfg_output(CLUE_SPEAKER);
		nrf_gpio_pin_clear(CLUE_SPEAKER);
		buzzer_init_pwm();
		m_pwmReady = true;
	}
}

uint32_t Buzzer::startTone(uint32_t freq_hz, uint32_t duration_ms)
{
	init();

	uint32_t now = m_nowMs();
	uint32_t code = buzz_fsm_start(&m_fsm, freq_hz, duration_ms, now);
	if (code != BUZZ_EVAL_SUCCESS) {
		return code;
	}

	buzzer_apply_freq(freq_hz);
	return BUZZ_EVAL_SUCCESS;
}

void Buzzer::stopTone(void)
{
	buzz_fsm_stop(&m_fsm);
	buzzer_stop_pwm();
}

bool Buzzer::tick(void)
{
	if (!buzz_fsm_is_playing(&m_fsm)) {
		return false;
	}

	uint32_t now = m_nowMs();
	bool completed = buzz_fsm_tick(&m_fsm, now);
	if (completed) {
		buzzer_stop_pwm();
	}
	return completed;
}

bool Buzzer::isPlaying(void) const
{
	return buzz_fsm_is_playing(&m_fsm);
}

#else /* !BOARD_CLUE — non-CLUE platforms: buzzer is a stub. */

Buzzer::Buzzer(Core *core)
	: m_core(core)
{
	buzz_fsm_init(&m_fsm);
	m_pwmReady = false;
}

void Buzzer::init(void) {}

uint32_t Buzzer::startTone(uint32_t, uint32_t)
{
	return BUZZ_EVAL_NOT_IMPLEMENTED;
}

void Buzzer::stopTone(void) {}

bool Buzzer::tick(void) { return false; }

bool Buzzer::isPlaying(void) const { return false; }

#endif /* BOARD_CLUE */
