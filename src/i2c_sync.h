/*
 * i2c_sync.h - Standalone synchronous TWIM1 transfer API (BOARD_CLUE only).
 *
 * Provides blocking I2C transfers using raw NRF_TWIM1 register access
 * (Adafruit Wire pattern: TASKS_RESUME before every transfer, polling on
 * EVENTS, no SHORTS, no ISR). Intended for sensor drivers that need
 * deterministic single-transfer latency without the async i2cbus pipeline.
 *
 * Precondition: TWIM1 must already be enabled (i2c_twim_backend_get() or
 * equivalent nrfx_twim_init+enable call). These functions do not initialize
 * the peripheral — they only drive its registers.
 */
#ifndef I2C_SYNC_H
#define I2C_SYNC_H

#ifdef BOARD_CLUE

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
	I2C_SYNC_OK = 0,       /* transfer completed successfully */
	I2C_SYNC_NACK,         /* address or data NACK from slave */
	I2C_SYNC_BUS_ERROR,    /* overrun or other bus error */
} i2c_sync_result_t;

/*
 * Write `len` bytes to `addr`. Blocks until the transfer completes or errors.
 * No register prefix is added — caller supplies the raw payload.
 */
i2c_sync_result_t i2c_sync_write(uint8_t addr, const uint8_t *data, size_t len);

/*
 * Write `reg` as a 1-byte prefix, then read `len` bytes into `buf`.
 * The write and read phases are separated by TWIM SUSPEND (clock-low-hold),
 * not a STOP — the slave retains its register pointer across the repeated-start.
 */
i2c_sync_result_t i2c_sync_read_reg(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len);

/*
 * Bare read of `len` bytes from `addr` with NO preceding write.
 * Required by SHT31-D: the host writes a 2-byte command, waits for the
 * conversion, then calls this function to read the response.
 */
i2c_sync_result_t i2c_sync_read_only(uint8_t addr, uint8_t *buf, size_t len);

/*
 * Convenience: write a single (register, value) pair to `addr`.
 * Equivalent to i2c_sync_write(addr, {reg, value}, 2).
 */
i2c_sync_result_t i2c_sync_write_reg_byte(uint8_t addr, uint8_t reg, uint8_t value);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_CLUE */
#endif /* I2C_SYNC_H */
