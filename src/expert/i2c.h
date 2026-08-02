/*
 * i2c.h - External I2C expert API for CLUE edge connector.
 *
 * Provides Board protocol I2cTransfer over the TWIM1 bus manager
 * (Todo 16's i2cBus). External addresses (0x08-0x77, excluding the
 * 5 onboard sensor addresses) are directly accessible. Onboard
 * addresses require force=true and quiescence of the entire TWIM1
 * sensor-bus group via expert_force.
 *
 * 7-bit address space, explicit write/read/repeated-start forms.
 * Bounded buffers <=256 bytes per direction. 10ms per bus operation
 * timeout (enforced by i2cBus). Precise transferred/error counts
 * on partial completion.
 *
 * allow: SIZE_OK — thin firmware wrapper. All decision logic is in
 * io_eval.{h,c}; this file translates between the Board protocol
 * and i2cBus TWIM1 hardware via the backend struct.
 *
 * Compiled only under BOARD_CLUE.
 */
#ifndef EXPERT_I2C_H
#define EXPERT_I2C_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "io_eval.h"
#include "expert_force.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Transfer descriptor ---- */

typedef struct {
	uint8_t             address;       /* 7-bit slave address         */
	const uint8_t      *write_buf;     /* TX (NULL if write_len=0)    */
	size_t              write_len;
	uint8_t            *read_buf;      /* RX (NULL if read_len=0)     */
	size_t              read_len;
	bool                repeated_start; /* no STOP between W and R  */
	bool                force;         /* quiesce onboard sensors?   */
} i2c_expert_transfer_t;

/* ---- Transfer result ---- */

typedef struct {
	io_bus_result_t  bus_result;
	uint32_t         bytes_written;
	uint32_t         bytes_read;
} i2c_expert_result_t;

/* ---- Result codes (map to BoardResultCode) ---- */

typedef enum {
	I2C_EXPERT_OK              = 0,  /* SUCCESS              */
	I2C_EXPERT_ERR_INVALID_ARG = 2,  /* INVALID_ARGUMENT     */
	I2C_EXPERT_ERR_PERMISSION  = 3,  /* PERMISSION_DENIED    */
	I2C_EXPERT_ERR_BUSY        = 6,  /* BUSY (onboard no force) */
	I2C_EXPERT_ERR_NACK        = 7,  /* custom: NACK         */
	I2C_EXPERT_ERR_TIMEOUT     = 8,  /* custom: TIMEOUT      */
	I2C_EXPERT_ERR_NOT_IMPL    = 14, /* NOT_IMPLEMENTED      */
} i2c_expert_code_t;

/* ---- Backend (hardware abstraction) ----
 *
 * Wraps the i2cBus TWIM1 manager. The backend performs a blocking
 * write-then-read transfer. For repeated_start=true, no STOP is
 * generated between write and read phases.
 */
typedef struct {
	/* Blocking write-then-read. Returns bus result.
	 * address is 7-bit. write_buf/read_buf are RAM.
	 * If write_len=0: read-only (START + addr+r + read + STOP).
	 * If read_len=0: write-only (START + addr+w + write + STOP).
	 * If repeated_start: no STOP between write and read. */
	io_bus_result_t (*transfer)(uint8_t address,
	                            const uint8_t *write_buf, size_t write_len,
	                            uint8_t *read_buf, size_t read_len,
	                            bool repeated_start);
} i2c_backend_t;

/* ---- Public API ---- */

/* Initialize with a backend. Firmware: i2cBus TWIM1; host: mock. */
void i2c_expert_init(const i2c_backend_t *backend);

/* Execute an I2C transfer.
 * Validates address, buffer sizes, and force requirement.
 * If onboard address and !force → I2C_EXPERT_ERR_BUSY.
 * If onboard address and force → quiesce sensor bus via expert_force.
 * Returns result code + partial transfer counts. */
i2c_expert_code_t i2c_expert_transfer(const i2c_expert_transfer_t *xfer,
                                      i2c_expert_result_t *out_result);

#ifdef __cplusplus
}
#endif
#endif /* EXPERT_I2C_H */
