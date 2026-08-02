/*
 * buzzer.h - Buzzer driver firmware wrapper.
 *
 * PWM1 (NOT PWM0 — PWM0 is owned by NeoPixel via Todo 8's led.cpp).
 * 1 MHz tick, 50% duty, 200-4000Hz range. Nonblocking playback with
 * monotonic duration.
 *
 * NOTE: custom_board.h's CLUE_SPEAKER_PWM_INSTANCE is defined as 0
 * (incorrect — PWM0 is taken by NeoPixel). We hardcode PWM1 here.
 * The eval layer in buzzer_eval.{h,c} handles validation/state; this
 * file wraps it with nrfx_pwm calls.
 */
#ifndef OUTPUT_BUZZER_H
#define OUTPUT_BUZZER_H

#include "buzzer_eval.h"

#ifdef __cplusplus

/* Forward decl — avoids pulling core.h into this header. */
class Core;

class Buzzer {
public:
	explicit Buzzer(Core *core = nullptr);

	/* Initialize PWM1 + speaker pin (P1.00). Idempotent. */
	void init(void);

	/* Start a tone. freq_hz 200-4000, duration_ms 1-65535.
	 * Returns BoardResultCode (BUZZ_EVAL_*).
	 * If a tone is already playing, the new one replaces it. */
	uint32_t startTone(uint32_t freq_hz, uint32_t duration_ms);

	/* Stop any active tone immediately. Idempotent. */
	void stopTone(void);

	/* Called from Core::loop() — completes tones whose duration has
	 * elapsed. Returns true iff a tone just completed. */
	bool tick(void);

	/* True iff the buzzer is actively producing sound. */
	bool isPlaying(void) const;

private:
	Core      *m_core;
	buzz_fsm_t m_fsm;
	bool       m_pwmReady;
	uint32_t   m_nowMs(void);
};

#endif /* __cplusplus */
#endif /* OUTPUT_BUZZER_H */
