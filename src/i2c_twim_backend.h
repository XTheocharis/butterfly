/*
 * i2c_twim_backend.h - production TWIM1 backend for i2cBus.
 *
 * Wires nrfx_twim (TWIM instance 1, SDA=P0.24 / SCL=P0.25 / 400 kHz) to
 * the pluggable i2cbus_backend_t interface so the pure-logic i2cBus
 * manager can drive the five CLUE onboard sensors in production.
 *
 * The TWIM1 ISR fires nrfx_twim's event handler, which translates the
 * nrfx event type to an i2cbus_result_t and forwards it to
 * i2cbus_report_xfer_complete() (thread/ISR-safe by design).
 *
 * Recovery primitives (release_sda / toggle_scl / read_sda / gen_stop /
 * reinit_twim) implement bit-banged SCL clocking via open-drain GPIO so
 * a stuck slave can release SDA, then reinit the TWIM peripheral.
 *
 * Firmware-only: the entire file is inside #ifdef BOARD_CLUE.
 */
#ifndef I2C_TWIM_BACKEND_H
#define I2C_TWIM_BACKEND_H

#include "i2cBus.h"

#ifdef BOARD_CLUE
#ifdef __cplusplus
extern "C" {
#endif

/* Initialize TWIM1 hardware + return an i2cbus_backend_t wired to it.
 * Caller passes the resulting struct to i2cbus_init().
 * Returns NULL if nrfx_twim_init fails (then caller skips i2cBus). */
const i2cbus_backend_t *i2c_twim_backend_get(void);
void i2c_twim_poll(void);
int i2c_twim_backend_is_ready(void);

#ifdef __cplusplus
}
#endif
#endif /* BOARD_CLUE */
#endif /* I2C_TWIM_BACKEND_H */
