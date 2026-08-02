/*
 * gpio.h - GPIO expert API for CLUE edge connector.
 *
 * Provides Board protocol GpioConfigure / GpioRead / GpioWrite / ReleasePin
 * over the pinRegistry lease system (Todo 7). All hardware operations are
 * behind a pluggable backend so the logic compiles and tests on host.
 *
 * Lifecycle: acquire(d_pin, owner) → token → configure/read/write(token)
 *            → release(token).
 *
 * Reserved resources (USB D+/D-, SWD, reset, power rails) are always
 * refused by pinRegistry before reaching the expert layer.
 *
 * allow: SIZE_OK — thin firmware wrapper. All decision logic is in
 * expert_eval.{h,c}; this file only translates between the Board protocol
 * and nrf_gpio hardware via the backend struct.
 */
#ifndef EXPERT_GPIO_H
#define EXPERT_GPIO_H

#include <stdint.h>
#include <stdbool.h>

#include "expert_eval.h"
#include "pinRegistry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Backend (hardware abstraction) ---- */

typedef struct {
	/* Configure a pin as input with pull and sense. */
	void (*cfg_input)(uint8_t resource_id, expert_gpio_pull_t pull,
	                  expert_gpio_sense_t sense);
	/* Configure a pin as output with drive strength. */
	void (*cfg_output)(uint8_t resource_id, expert_gpio_drive_t drive);
	/* Read pin level: 0 = low, 1 = high. */
	int  (*read)(uint8_t resource_id);
	/* Write pin level: 0 = low, 1 = high. */
	void (*write)(uint8_t resource_id, int value);
	/* Restore pin to default state (called on release via pinreg restore). */
	void (*restore)(uint8_t resource_id);
} gpio_backend_t;

/* ---- Public API ---- */

/* Initialize with a backend (firmware: nrf_gpio; host tests: mock). */
void gpio_expert_init(const gpio_backend_t *backend);

/* Acquire a GPIO lease for a D-pin.
 * Returns EXPERT_OK + token on success.
 * Returns EXPERT_ERR_INVALID_PARAM for unknown D-pin.
 * pinreg errors (BUSY, RESERVED) are passed through as EXPERT_ERR_*. */
expert_result_t gpio_expert_acquire(uint8_t d_pin, pinreg_owner_t owner,
                                    expert_token_t *out_token);

/* Configure a leased pin. Validates token + config before applying. */
expert_result_t gpio_expert_configure(expert_token_t token, pinreg_owner_t owner,
                                      const expert_gpio_config_t *cfg);

/* Read a leased input pin. *out_value is 0 or 1. */
expert_result_t gpio_expert_read(expert_token_t token, pinreg_owner_t owner,
                                 int *out_value);

/* Write a leased output pin. value is 0 or 1. */
expert_result_t gpio_expert_write(expert_token_t token, pinreg_owner_t owner,
                                  int value);

/* Release a lease. Idempotent for the immediately preceding release.
 * Calls pinreg_release internally to free the underlying hardware. */
expert_result_t gpio_expert_release(expert_token_t token);

#ifdef __cplusplus
}
#endif
#endif /* EXPERT_GPIO_H */
