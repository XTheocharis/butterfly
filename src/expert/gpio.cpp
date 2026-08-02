/*
 * gpio.cpp - GPIO expert API firmware implementation.
 *
 * Wraps expert_eval.{h,c} with pinRegistry leases and nrf_gpio hardware.
 * All decision logic is in expert_eval.c; this file translates between
 * the Board protocol, pinRegistry, and nrf_gpio.
 *
 * Token mapping: expert_eval assigns its own opaque session-scoped token
 * (for owner/session validation), while pinRegistry assigns a separate
 * generation-tagged hardware token. This file maintains a parallel array
 * mapping expert_token to pinreg_token so release can free both.
 *
 * Compiled only under BOARD_CLUE.
 */
#ifdef BOARD_CLUE

#include "gpio.h"
#include "custom_board.h"

#include "nrf.h"
#include "nrf_gpio.h"

extern "C" {
#include "expert_eval.c"
}

/* ---- nrf_gpio backend ---- */

static void hw_cfg_input(uint8_t resource_id, expert_gpio_pull_t pull,
                         expert_gpio_sense_t sense)
{
	nrf_gpio_pin_pull_t nrf_pull = NRF_GPIO_PIN_NOPULL;
	switch (pull) {
	case EXPERT_GPIO_PULL_PULLDOWN: nrf_pull = NRF_GPIO_PIN_PULLDOWN; break;
	case EXPERT_GPIO_PULL_PULLUP:   nrf_pull = NRF_GPIO_PIN_PULLUP;   break;
	default: break;
	}

	nrf_gpio_cfg_input(resource_id, nrf_pull);

	if (sense == EXPERT_GPIO_SENSE_HIGH)
		nrf_gpio_cfg_sense_set(resource_id, NRF_GPIO_PIN_SENSE_HIGH);
	else if (sense == EXPERT_GPIO_SENSE_LOW)
		nrf_gpio_cfg_sense_set(resource_id, NRF_GPIO_PIN_SENSE_LOW);
}

static void hw_cfg_output(uint8_t resource_id, expert_gpio_drive_t drive)
{
	/* nRF drive enum: 0=S0S1, 1=H0S1, 2=S0H1, 3=H0H1, 4=D0S1, 5=D0H1,
	 * 6=S0D1, 7=H0D1 — matches expert_gpio_drive_t exactly. */
	nrf_gpio_cfg(resource_id,
	             NRF_GPIO_PIN_DIR_OUTPUT,
	             NRF_GPIO_PIN_INPUT_DISCONNECT,
	             NRF_GPIO_PIN_NOPULL,
	             (nrf_gpio_pin_drive_t)drive,
	             NRF_GPIO_PIN_NOSENSE);
}

static int hw_read(uint8_t resource_id)
{
	return (int)nrf_gpio_pin_read(resource_id);
}

static void hw_write(uint8_t resource_id, int value)
{
	if (value) nrf_gpio_pin_set(resource_id);
	else       nrf_gpio_pin_clear(resource_id);
}

static void hw_restore(uint8_t resource_id)
{
	nrf_gpio_cfg_default(resource_id);
}

static const gpio_backend_t HW_BACKEND = {
	.cfg_input  = hw_cfg_input,
	.cfg_output = hw_cfg_output,
	.read       = hw_read,
	.write      = hw_write,
	.restore    = hw_restore,
};

/* ---- pinreg restore bridge ---- */

static void pinreg_restore_bridge(pinreg_group_t group, uint8_t pin)
{
	(void)group;
	if (HW_BACKEND.restore) HW_BACKEND.restore(pin);
}

/* ---- Expert-to-pinreg token mapping ---- */

typedef struct {
	expert_token_t  expert_tok;
	pinreg_token_t  pinreg_tok;
	bool            used;
	bool            hw_released;
} token_map_t;

static token_map_t g_map[EXPERT_MAX_LEASES];

static token_map_t *map_find(expert_token_t et)
{
	for (uint8_t i = 0; i < EXPERT_MAX_LEASES; i++)
		if (g_map[i].used && g_map[i].expert_tok == et)
			return &g_map[i];
	return NULL;
}

static token_map_t *map_alloc(void)
{
	for (uint8_t i = 0; i < EXPERT_MAX_LEASES; i++)
		if (!g_map[i].used)
			return &g_map[i];
	return NULL;
}

/* ---- State ---- */

static const gpio_backend_t *g_backend;
static bool g_initialized;

void gpio_expert_init(const gpio_backend_t *backend)
{
	g_backend = backend ? backend : &HW_BACKEND;
	g_initialized = true;
	expert_eval_init();
	for (uint8_t i = 0; i < EXPERT_MAX_LEASES; i++) {
		g_map[i].used = false;
		g_map[i].hw_released = false;
	}
}

expert_result_t gpio_expert_acquire(uint8_t d_pin, pinreg_owner_t owner,
                                    expert_token_t *out_token)
{
	if (!g_initialized || !out_token)
		return EXPERT_ERR_INVALID_PARAM;

	*out_token = EXPERT_TOKEN_INVALID;

	const expert_alias_t *alias = expert_eval_alias_by_d(d_pin);
	if (!alias) return EXPERT_ERR_INVALID_PARAM;

	pinreg_token_t pinreg_tok = PINREG_TOKEN_INVALID;
	pinreg_result_t pr = pinreg_acquire_pin(alias->resource_id, owner,
	                                       pinreg_restore_bridge,
	                                       &pinreg_tok);
	if (pr != PINREG_OK) {
		switch (pr) {
		case PINREG_RESERVED:  return EXPERT_ERR_INVALID_PARAM;
		case PINREG_BUSY:      return EXPERT_ERR_TABLE_FULL;
		default:               return EXPERT_ERR_INVALID_PARAM;
		}
	}

	expert_token_t et = EXPERT_TOKEN_INVALID;
	expert_result_t er = expert_eval_lease_issue(alias->resource_id,
	                                             (uint8_t)owner, false, &et);
	if (er != EXPERT_OK) {
		pinreg_release(pinreg_tok);
		return er;
	}

	token_map_t *m = map_alloc();
	if (!m) {
		(void)expert_eval_lease_release(et, (uint8_t)owner);
		pinreg_release(pinreg_tok);
		return EXPERT_ERR_TABLE_FULL;
	}

	m->expert_tok  = et;
	m->pinreg_tok  = pinreg_tok;
	m->used        = true;
	m->hw_released = false;

	*out_token = et;
	return EXPERT_OK;
}

expert_result_t gpio_expert_configure(expert_token_t token, pinreg_owner_t owner,
                                      const expert_gpio_config_t *cfg)
{
	if (!g_initialized || !cfg)
		return EXPERT_ERR_INVALID_PARAM;

	if (!expert_eval_gpio_config_valid(cfg))
		return EXPERT_ERR_INVALID_PARAM;

	expert_result_t er = expert_eval_lease_validate(token, (uint8_t)owner);
	if (er != EXPERT_OK) return er;

	uint8_t resource = 0;
	if (!expert_eval_lease_query(token, &resource, NULL))
		return EXPERT_ERR_INVALID_TOKEN;

	if (cfg->dir == EXPERT_GPIO_DIR_INPUT) {
		if (g_backend->cfg_input)
			g_backend->cfg_input(resource, cfg->pull, cfg->sense);
	} else {
		if (g_backend->cfg_output)
			g_backend->cfg_output(resource, cfg->drive);
	}
	return EXPERT_OK;
}

expert_result_t gpio_expert_read(expert_token_t token, pinreg_owner_t owner,
                                 int *out_value)
{
	if (!g_initialized || !out_value)
		return EXPERT_ERR_INVALID_PARAM;

	expert_result_t er = expert_eval_lease_validate(token, (uint8_t)owner);
	if (er != EXPERT_OK) return er;

	uint8_t resource = 0;
	if (!expert_eval_lease_query(token, &resource, NULL))
		return EXPERT_ERR_INVALID_TOKEN;

	if (!g_backend->read)
		return EXPERT_ERR_INVALID_PARAM;

	*out_value = g_backend->read(resource);
	return EXPERT_OK;
}

expert_result_t gpio_expert_write(expert_token_t token, pinreg_owner_t owner,
                                  int value)
{
	if (!g_initialized)
		return EXPERT_ERR_INVALID_PARAM;

	expert_result_t er = expert_eval_lease_validate(token, (uint8_t)owner);
	if (er != EXPERT_OK) return er;

	uint8_t resource = 0;
	if (!expert_eval_lease_query(token, &resource, NULL))
		return EXPERT_ERR_INVALID_TOKEN;

	if (!g_backend->write)
		return EXPERT_ERR_INVALID_PARAM;

	g_backend->write(resource, value);
	return EXPERT_OK;
}

expert_result_t gpio_expert_release(expert_token_t token)
{
	if (!g_initialized)
		return EXPERT_ERR_INVALID_PARAM;

	token_map_t *m = map_find(token);

	/* Resolve the owner recorded at issue time for the eval owner check.
	 * Idempotent repeats (token already released) fall through to the
	 * last-released cache inside release() before any owner compare. */
	uint8_t owner = 0;
	(void)expert_eval_lease_query(token, NULL, &owner);
	expert_result_t er = expert_eval_lease_release(token, owner);
	if (er != EXPERT_OK) return er;

	/* Release hardware only on the FIRST release (not idempotent repeat). */
	if (m && !m->hw_released) {
		pinreg_release(m->pinreg_tok);
		m->hw_released = true;
		m->used = false;
	}
	return EXPERT_OK;
}

#endif /* BOARD_CLUE */
