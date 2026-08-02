/*
 * i2c.cpp - External I2C expert API firmware implementation.
 *
 * Wraps io_eval.{h,c} with the i2cBus TWIM1 manager (Todo 16).
 * All decision logic is in io_eval.c; this file translates between
 * the Board protocol and i2cBus hardware.
 *
 * Compiled only under BOARD_CLUE.
 */
#ifdef BOARD_CLUE

#include <cstring>

#include "i2c.h"
#include "i2cBus.h"
#include "custom_board.h"

extern "C" {
#include "io_eval.h"
}

/* ---- i2cBus blocking transfer adapter ---- */

/* Spin until i2cBus finishes the current transfer (idle/recovery/error). */
static void i2cbus_drain(void)
{
	uint32_t spin = 0;
	while (i2cbus_get_state() == I2CBUS_STATE_BUSY ||
	       i2cbus_get_state() == I2CBUS_STATE_RECOVERY) {
		i2cbus_tick();
		if (++spin > 100000u) break; /* safety limit */
	}
}

static io_bus_result_t i2cbus_blocking_transfer(
	uint8_t address,
	const uint8_t *write_buf, size_t write_len,
	uint8_t *read_buf, size_t read_len,
	bool repeated_start)
{
	/* For external expert I2C, we bypass i2cBus's serialized queue
	 * and perform a direct TWIM transfer. The i2cBus manager handles
	 * onboard sensor access; expert external I2C uses the same TWIM1
	 * hardware but without the sensor-specific probe/validation flow.
	 *
	 * When force=true and the address is onboard, the sensor bus group
	 * has already been quiesced by expert_force before this function
	 * is called. The TWIM1 hardware is available for direct use.
	 *
	 * This adapter uses i2cBus's backend to perform the raw transfer.
	 * The blocking semantics are provided by polling i2cbus_tick(). */

	/* Write-only or read-only: single transfer, repeated_start is moot. */
	if (write_len == 0 || read_len == 0) {
		i2cbus_transfer_t xfer;
		memset(&xfer, 0, sizeof(xfer));
		xfer.addr       = address;
		xfer.write_buf  = write_buf;
		xfer.write_len  = write_len;
		xfer.read_buf   = read_buf;
		xfer.read_len   = read_len;
		xfer.completion = NULL;
		xfer.user       = NULL;

		i2cbus_token_t tok = i2cbus_enqueue(&xfer);
		if (tok == I2CBUS_TOKEN_INVALID)
			return IO_EVAL_BUS_ERROR;

		i2cbus_drain();
		i2cbus_state_t final_state = i2cbus_get_state();
		if (final_state == I2CBUS_STATE_IDLE)
			return IO_EVAL_BUS_OK;
		if (final_state == I2CBUS_STATE_RECOVERY)
			return IO_EVAL_BUS_ERROR;
		return IO_EVAL_BUS_NACK;
	}

	/* Both phases present. repeated_start=true keeps the original
	 * single-transfer behaviour (no STOP between write and read).
	 * repeated_start=false splits into two separate transfers so the
	 * bus issues STOP after the write phase — required by slaves that
	 * cannot follow a repeated-start register read. */
	if (repeated_start) {
		i2cbus_transfer_t xfer;
		memset(&xfer, 0, sizeof(xfer));
		xfer.addr       = address;
		xfer.write_buf  = write_buf;
		xfer.write_len  = write_len;
		xfer.read_buf   = read_buf;
		xfer.read_len   = read_len;
		xfer.completion = NULL;
		xfer.user       = NULL;

		i2cbus_token_t tok = i2cbus_enqueue(&xfer);
		if (tok == I2CBUS_TOKEN_INVALID)
			return IO_EVAL_BUS_ERROR;

		i2cbus_drain();
	} else {
		/* Phase 1: write only. */
		i2cbus_transfer_t wx;
		memset(&wx, 0, sizeof(wx));
		wx.addr       = address;
		wx.write_buf  = write_buf;
		wx.write_len  = write_len;
		wx.read_buf   = NULL;
		wx.read_len   = 0;
		wx.completion = NULL;
		wx.user       = NULL;

		i2cbus_token_t wtok = i2cbus_enqueue(&wx);
		if (wtok == I2CBUS_TOKEN_INVALID)
			return IO_EVAL_BUS_ERROR;
		i2cbus_drain();
		i2cbus_state_t w_state = i2cbus_get_state();
		if (w_state != I2CBUS_STATE_IDLE)
			return (w_state == I2CBUS_STATE_RECOVERY)
				? IO_EVAL_BUS_ERROR : IO_EVAL_BUS_NACK;

		/* Phase 2: read only (fresh START+addr+r). */
		i2cbus_transfer_t rx;
		memset(&rx, 0, sizeof(rx));
		rx.addr       = address;
		rx.write_buf  = NULL;
		rx.write_len  = 0;
		rx.read_buf   = read_buf;
		rx.read_len   = read_len;
		rx.completion = NULL;
		rx.user       = NULL;

		i2cbus_token_t rtok = i2cbus_enqueue(&rx);
		if (rtok == I2CBUS_TOKEN_INVALID)
			return IO_EVAL_BUS_ERROR;
		i2cbus_drain();
	}

	i2cbus_state_t final_state = i2cbus_get_state();
	if (final_state == I2CBUS_STATE_IDLE)
		return IO_EVAL_BUS_OK;
	if (final_state == I2CBUS_STATE_RECOVERY)
		return IO_EVAL_BUS_ERROR;
	return IO_EVAL_BUS_NACK;
}

static const i2c_backend_t HW_BACKEND = {
	.transfer = i2cbus_blocking_transfer,
};

/* ---- State ---- */

static const i2c_backend_t *g_backend;
static bool g_initialized;

void i2c_expert_init(const i2c_backend_t *backend)
{
	g_backend = backend ? backend : &HW_BACKEND;
	g_initialized = true;
}

i2c_expert_code_t i2c_expert_transfer(const i2c_expert_transfer_t *xfer,
                                      i2c_expert_result_t *out_result)
{
	if (!g_initialized || !xfer)
		return I2C_EXPERT_ERR_INVALID_ARG;

	if (out_result) {
		out_result->bus_result    = IO_EVAL_BUS_OK;
		out_result->bytes_written = 0;
		out_result->bytes_read    = 0;
	}

	/* Validate address (7-bit). */
	if (!io_eval_i2c_address_valid(xfer->address))
		return I2C_EXPERT_ERR_INVALID_ARG;

	/* Validate buffer sizes. */
	if (!io_eval_buffer_size_valid(xfer->write_len) ||
	    !io_eval_buffer_size_valid(xfer->read_len))
		return I2C_EXPERT_ERR_INVALID_ARG;

	/* Validate RAM pointers for EasyDMA. */
	if (xfer->write_len > 0 && xfer->write_buf &&
	    !io_eval_is_ram_pointer(xfer->write_buf))
		return I2C_EXPERT_ERR_INVALID_ARG;
	if (xfer->read_len > 0 && xfer->read_buf &&
	    !io_eval_is_ram_pointer(xfer->read_buf))
		return I2C_EXPERT_ERR_INVALID_ARG;

	/* Onboard address requires force. */
	bool onboard = io_eval_is_onboard_i2c(xfer->address);

	if (onboard && !xfer->force)
		return I2C_EXPERT_ERR_BUSY;

	if (onboard && xfer->force) {
		/* Quiesce the entire sensor bus group. */
		expert_force_result_t fr = expert_force_request(
			EXPERT_FORCE_SVC_SENSOR_BUS);

		if (fr == EXPERT_FORCE_ERR_NOT_CANCELLABLE)
			return I2C_EXPERT_ERR_BUSY;

		if (fr == EXPERT_FORCE_ERR_ALREADY_DISPLACED) {
			/* Already quiesced by a previous force — OK to proceed. */
		} else if (fr != EXPERT_FORCE_OK) {
			return I2C_EXPERT_ERR_BUSY;
		} else {
			/* Quiesce accepted. In a full implementation, the quiesce
			 * callback from the sensor subsystem marks completion.
			 * For now, complete immediately (synchronous quiesce). */
			expert_force_quiesce_complete(EXPERT_FORCE_SVC_SENSOR_BUS);
		}
	}

	/* Execute transfer. */
	if (!g_backend->transfer)
		return I2C_EXPERT_ERR_NOT_IMPL;

	io_bus_result_t bus = g_backend->transfer(
		xfer->address,
		xfer->write_buf, xfer->write_len,
		xfer->read_buf, xfer->read_len,
		xfer->repeated_start);

	/* Compute partial transfer result. */
	io_transfer_result_t tr = {};
	io_eval_i2c_compute_result(bus,
		xfer->write_len, xfer->read_len,
		xfer->write_len, /* nack_after_write: assume all write sent */
		&tr);

	if (out_result) {
		out_result->bus_result    = tr.bus_result;
		out_result->bytes_written = tr.bytes_written;
		out_result->bytes_read    = tr.bytes_read;
	}

	/* If we quiesced the sensor bus, restore it. */
	if (onboard && xfer->force &&
	    expert_force_is_displaced(EXPERT_FORCE_SVC_SENSOR_BUS)) {
		expert_force_release(EXPERT_FORCE_SVC_SENSOR_BUS);
		/* Restore: in a full implementation, this triggers sensor
		 * reinitialization. Synchronous completion here. */
		expert_force_restore_complete(EXPERT_FORCE_SVC_SENSOR_BUS, true);
	}

	/* Map bus result to expert code. */
	switch (bus) {
	case IO_EVAL_BUS_OK:      return I2C_EXPERT_OK;
	case IO_EVAL_BUS_NACK:    return I2C_EXPERT_ERR_NACK;
	case IO_EVAL_BUS_TIMEOUT: return I2C_EXPERT_ERR_TIMEOUT;
	default:                  return I2C_EXPERT_ERR_BUSY;
	}
}

#endif /* BOARD_CLUE */
