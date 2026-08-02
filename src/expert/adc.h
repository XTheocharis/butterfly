/*
 * adc.h - SAADC expert API for CLUE edge connector.
 *
 * Provides Board protocol AdcRead over the pinRegistry lease system.
 * The SAADC is 12-bit, internal 0.6V reference, gain 1/6 → 3.6V
 * full-scale. CLUE pins are 3.3V-only (VDD); software CANNOT enforce
 * the electrical maximum. The SATURATED flag indicates the measurement
 * exceeded the nominal VDD but does not protect the hardware.
 *
 * Two modes:
 *   1. Retained: acquire(token) → configure(token, ...) → read(token)
 *      → release(token). Pin stays configured between reads.
 *   2. One-shot: read_oneshot(d_pin) — acquires, samples, releases
 *      automatically (temporary-owner restoration).
 *
 * allow: SIZE_OK — thin firmware wrapper. All decision logic is in
 * expert_eval.{h,c}.
 */
#ifndef EXPERT_ADC_H
#define EXPERT_ADC_H

#include <stdint.h>
#include <stdbool.h>

#include "expert_eval.h"
#include "pinRegistry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- SAADC configuration ---- */

typedef struct {
	uint8_t  gain_denominator; /* 1..6 (6 = gain 1/6 for 3.6V FS)       */
	uint16_t acquisition_us;   /* 3, 5, 10, 15, 20, 40 (nRF SAADC acq)  */
	bool     oversample_4x;    /* average 4 samples for +1 bit          */
} adc_config_t;

/* Default: gain 1/6, 10us acquisition, no oversampling. */
#define ADC_DEFAULT_CONFIG \
	{ .gain_denominator = 6, .acquisition_us = 10, .oversample_4x = false }

/* ---- Backend (hardware abstraction) ---- */

typedef struct {
	void     (*init_channel)(uint8_t ain, uint8_t gain_denom,
	                         uint16_t acq_us, bool oversample);
	void     (*uninit_channel)(uint8_t ain);
	uint16_t (*sample)(uint8_t ain);  /* blocking single-shot read       */
	void     (*calibrate)(void);
} adc_backend_t;

/* ---- Public API ---- */

void adc_expert_init(const adc_backend_t *backend);

/* Acquire an analog pin for retained sampling. */
expert_result_t adc_expert_acquire(uint8_t d_pin, pinreg_owner_t owner,
                                   expert_token_t *out_token);

/* Configure the SAADC channel for a retained lease. */
expert_result_t adc_expert_configure(expert_token_t token,
                                     pinreg_owner_t owner,
                                     const adc_config_t *cfg);

/* Read from a retained lease. Returns measured mV (clamped if >VDD). */
expert_result_t adc_expert_read(expert_token_t token, pinreg_owner_t owner,
                                uint16_t *out_mv,
                                expert_adc_status_t *out_status);

/* One-shot: acquire, configure default, sample, release. Pin ownership
 * is restored (pinreg release triggers restore callback). */
expert_result_t adc_expert_read_oneshot(uint8_t d_pin, pinreg_owner_t owner,
                                        uint16_t *out_mv,
                                        expert_adc_status_t *out_status);

/* Trigger SAADC offset calibration. */
expert_result_t adc_expert_calibrate(void);

/* Release a retained lease. Idempotent for immediately preceding release. */
expert_result_t adc_expert_release(expert_token_t token);

#ifdef __cplusplus
}
#endif
#endif /* EXPERT_ADC_H */
