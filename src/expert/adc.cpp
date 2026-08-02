/*
 * adc.cpp - SAADC expert API firmware implementation.
 *
 * Wraps expert_eval.{h,c} with pinRegistry leases and nrfx_saadc hardware.
 * All decision logic is in expert_eval.c; this file translates between
 * the Board protocol, pinRegistry, and nrfx_saadc.
 *
 * One-shot read acquires a pin, configures SAADC with default settings,
 * samples once, converts to millivolts, then releases the pin (temporary-
 * owner restoration via pinreg restore callback).
 *
 * allow: SIZE_OK — indivisible firmware wrapper. The SAADC backend, token
 * mapping, and API surface share the same state variables. Splitting
 * would scatter the backend helpers from their sole callers.
 *
 * Compiled only under BOARD_CLUE.
 */
#ifdef BOARD_CLUE

#include "adc.h"
#include "custom_board.h"

#include "nrf.h"
#include "nrf_gpio.h"
#include "nrfx_saadc.h"
#include "app_util_platform.h"

extern "C" {
#include "expert_eval.h"
}

/* ---- nrfx_saadc backend ---- */

static bool g_saadc_initialized;

static void ensure_saadc_init(void)
{
	if (g_saadc_initialized) return;
	nrfx_saadc_config_t cfg = {
		.resolution         = NRF_SAADC_RESOLUTION_12BIT,
		.oversample         = NRF_SAADC_OVERSAMPLE_DISABLED,
		.interrupt_priority = 6,
		.low_power_mode     = false,
	};
	nrfx_saadc_init(&cfg, NULL);
	g_saadc_initialized = true;
}

/* Denom → gain enum: 1→GAIN1, 2→1/2, 3→1/3, 4→1/4, 5→1/5, 6→1/6.
 * nRF enum values are sequential starting from NRF_SAADC_GAIN1. */
static nrf_saadc_gain_t gain_from_denom(uint8_t denom)
{
	if (denom >= 1 && denom <= 6)
		return (nrf_saadc_gain_t)(NRF_SAADC_GAIN1 + (denom - 1));
	return NRF_SAADC_GAIN1_6;
}

/* Acquisition time us → enum: 3,5,10,15,20,40 map to ACQTIME_0..5. */
static nrf_saadc_acqtime_t acq_from_us(uint16_t us)
{
	switch (us) {
	case 3:  return NRF_SAADC_ACQTIME_3US;
	case 5:  return NRF_SAADC_ACQTIME_5US;
	case 15: return NRF_SAADC_ACQTIME_15US;
	case 20: return NRF_SAADC_ACQTIME_20US;
	case 40: return NRF_SAADC_ACQTIME_40US;
	default: return NRF_SAADC_ACQTIME_10US;
	}
}

static void hw_init_channel(uint8_t ain, uint8_t gain_denom,
                            uint16_t acq_us, bool oversample)
{
	ensure_saadc_init();

	if (oversample) {
		nrf_saadc_oversample_set(NRF_SAADC_OVERSAMPLE_4X);
	}

	nrf_saadc_channel_config_t cc = {
		.resistor_p = NRF_SAADC_RESISTOR_DISABLED,
		.resistor_n = NRF_SAADC_RESISTOR_DISABLED,
		.gain       = gain_from_denom(gain_denom),
		.reference  = NRF_SAADC_REFERENCE_INTERNAL, /* 0.6V */
		.acq_time   = acq_from_us(acq_us),
		.mode       = NRF_SAADC_MODE_SINGLE_ENDED,
		.burst      = oversample ? NRF_SAADC_BURST_ENABLED
		                          : NRF_SAADC_BURST_DISABLED,
		.pin_p      = (nrf_saadc_input_t)(NRF_SAADC_INPUT_AIN0 + ain),
		.pin_n      = NRF_SAADC_INPUT_DISABLED,
	};
	nrfx_saadc_channel_init(ain, &cc);
}

static void hw_uninit_channel(uint8_t ain)
{
	nrfx_saadc_channel_uninit(ain);
}

static uint16_t hw_sample(uint8_t ain)
{
	nrf_saadc_value_t value = 0;
	nrfx_saadc_sample_convert(ain, &value);
	if (value < 0) value = 0;
	if (value > (nrf_saadc_value_t)EXPERT_ADC_MAX_RAW) value = EXPERT_ADC_MAX_RAW;
	return (uint16_t)value;
}

static void hw_calibrate(void)
{
	ensure_saadc_init();
	nrfx_saadc_calibrate_offset();
	while (!nrfx_saadc_is_busy()) { }
	while (nrfx_saadc_is_busy()) { }
}

static const adc_backend_t HW_BACKEND = {
	.init_channel    = hw_init_channel,
	.uninit_channel  = hw_uninit_channel,
	.sample          = hw_sample,
	.calibrate       = hw_calibrate,
};

/* ---- pinreg restore bridge ---- */

static void adc_pinreg_restore(pinreg_group_t group, uint8_t pin)
{
	(void)group;
	(void)pin;
	/* Pin is released by pinreg; SAADC channel is uninitialized separately
	 * via the backend. No additional GPIO restore needed — SAADC releases
	 * the analog pin mux when the channel is uninit'd. */
}

/* ---- Expert-to-pinreg token mapping ---- */

typedef struct {
	expert_token_t  expert_tok;
	pinreg_token_t  pinreg_tok;
	uint8_t         ain;
	bool            used;
	bool            hw_released;
} adc_map_t;

static adc_map_t g_adcMap[EXPERT_MAX_LEASES];

static adc_map_t *adc_map_find(expert_token_t et)
{
	for (uint8_t i = 0; i < EXPERT_MAX_LEASES; i++)
		if (g_adcMap[i].used && g_adcMap[i].expert_tok == et)
			return &g_adcMap[i];
	return NULL;
}

static adc_map_t *adc_map_alloc(void)
{
	for (uint8_t i = 0; i < EXPERT_MAX_LEASES; i++)
		if (!g_adcMap[i].used)
			return &g_adcMap[i];
	return NULL;
}

/* ---- State ---- */

static const adc_backend_t *g_adcBackend;
static bool g_adcReady;

void adc_expert_init(const adc_backend_t *backend)
{
	g_adcBackend = backend ? backend : &HW_BACKEND;
	g_adcReady   = true;
	expert_eval_init();
	for (uint8_t i = 0; i < EXPERT_MAX_LEASES; i++) {
		g_adcMap[i].used = false;
		g_adcMap[i].hw_released = false;
	}
}

expert_result_t adc_expert_acquire(uint8_t d_pin, pinreg_owner_t owner,
                                   expert_token_t *out_token)
{
	if (!g_adcReady || !out_token)
		return EXPERT_ERR_INVALID_PARAM;

	*out_token = EXPERT_TOKEN_INVALID;

	const expert_alias_t *alias = expert_eval_alias_by_d(d_pin);
	if (!alias) return EXPERT_ERR_INVALID_PARAM;

	pinreg_token_t pinreg_tok = PINREG_TOKEN_INVALID;
	pinreg_result_t pr = pinreg_acquire_pin(alias->resource_id, owner,
	                                       adc_pinreg_restore,
	                                       &pinreg_tok);
	if (pr != PINREG_OK) {
		switch (pr) {
		case PINREG_RESERVED:  return EXPERT_ERR_INVALID_PARAM;
		case PINREG_BUSY:      return EXPERT_ERR_TABLE_FULL;
		default:               return EXPERT_ERR_INVALID_PARAM;
		}
	}

	expert_token_t et = EXPERT_TOKEN_INVALID;
	/* require_analog = true: SAADC only works on AIN-capable pins. */
	expert_result_t er = expert_eval_lease_issue(alias->resource_id,
	                                             (uint8_t)owner, true, &et);
	if (er != EXPERT_OK) {
		pinreg_release(pinreg_tok);
		return er;
	}

	adc_map_t *m = adc_map_alloc();
	if (!m) {
		/* Roll back the expert lease we just issued. owner is the
		 * same one passed to lease_issue and is in scope here. */
		(void)expert_eval_lease_release(et, (uint8_t)owner);
		pinreg_release(pinreg_tok);
		return EXPERT_ERR_TABLE_FULL;
	}

	m->expert_tok  = et;
	m->pinreg_tok  = pinreg_tok;
	m->ain         = alias->ain;
	m->used        = true;
	m->hw_released = false;

	*out_token = et;
	return EXPERT_OK;
}

expert_result_t adc_expert_configure(expert_token_t token,
                                     pinreg_owner_t owner,
                                     const adc_config_t *cfg)
{
	if (!g_adcReady || !cfg)
		return EXPERT_ERR_INVALID_PARAM;

	expert_result_t er = expert_eval_lease_validate(token, (uint8_t)owner);
	if (er != EXPERT_OK) return er;

	adc_map_t *m = adc_map_find(token);
	if (!m) return EXPERT_ERR_INVALID_TOKEN;

	if (g_adcBackend->init_channel)
		g_adcBackend->init_channel(m->ain, cfg->gain_denominator,
		                           cfg->acquisition_us, cfg->oversample_4x);
	return EXPERT_OK;
}

expert_result_t adc_expert_read(expert_token_t token, pinreg_owner_t owner,
                                uint16_t *out_mv,
                                expert_adc_status_t *out_status)
{
	if (!g_adcReady || !out_mv)
		return EXPERT_ERR_INVALID_PARAM;

	expert_result_t er = expert_eval_lease_validate(token, (uint8_t)owner);
	if (er != EXPERT_OK) return er;

	adc_map_t *m = adc_map_find(token);
	if (!m) return EXPERT_ERR_INVALID_TOKEN;

	if (!g_adcBackend->sample)
		return EXPERT_ERR_INVALID_PARAM;

	uint16_t raw = g_adcBackend->sample(m->ain);
	expert_adc_status_t st = expert_eval_adc_raw_to_mv(raw, out_mv);
	if (out_status) *out_status = st;
	return EXPERT_OK;
}

expert_result_t adc_expert_read_oneshot(uint8_t d_pin, pinreg_owner_t owner,
                                        uint16_t *out_mv,
                                        expert_adc_status_t *out_status)
{
	if (!g_adcReady || !out_mv)
		return EXPERT_ERR_INVALID_PARAM;

	expert_token_t tok = EXPERT_TOKEN_INVALID;
	expert_result_t er = adc_expert_acquire(d_pin, owner, &tok);
	if (er != EXPERT_OK) return er;

	adc_config_t cfg = ADC_DEFAULT_CONFIG;
	er = adc_expert_configure(tok, owner, &cfg);
	if (er != EXPERT_OK) {
		adc_expert_release(tok);
		return er;
	}

	er = adc_expert_read(tok, owner, out_mv, out_status);
	adc_expert_release(tok); /* temporary-owner restoration */
	return er;
}

expert_result_t adc_expert_calibrate(void)
{
	if (!g_adcReady || !g_adcBackend->calibrate)
		return EXPERT_ERR_INVALID_PARAM;

	g_adcBackend->calibrate();
	return EXPERT_OK;
}

expert_result_t adc_expert_release(expert_token_t token)
{
	if (!g_adcReady)
		return EXPERT_ERR_INVALID_PARAM;

	adc_map_t *m = adc_map_find(token);

	/* Look up the owner recorded at issue time so the expert eval's
	 * owner check sees a matching owner for a legitimate release. For
	 * an already-released (idempotent repeat) token, query returns
	 * false and owner stays 0 — release() then resolves via its
	 * last-released cache before any owner comparison runs. */
	uint8_t owner = 0;
	(void)expert_eval_lease_query(token, NULL, &owner);
	expert_result_t er = expert_eval_lease_release(token, owner);
	if (er != EXPERT_OK) return er;

	if (m && !m->hw_released) {
		if (g_adcBackend->uninit_channel)
			g_adcBackend->uninit_channel(m->ain);
		pinreg_release(m->pinreg_tok);
		m->hw_released = true;
		m->used = false;
	}
	return EXPERT_OK;
}

#endif /* BOARD_CLUE */
