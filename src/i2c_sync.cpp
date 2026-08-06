/*
 * i2c_sync.cpp - Standalone synchronous TWIM1 transfer implementation.
 *
 * Extracts the proven raw-register TWIM1 polling pattern from
 * i2c_twim_backend.cpp:twim_start_xfer (lines 29-81) into four standalone
 * blocking functions. No nrfx API, no ISR, no async pipeline — just direct
 * NRF_TWIM1 register access following the Adafruit Wire convention:
 *
 *   1. TASKS_RESUME before every transfer (releases clock-low-hold).
 *   2. SHORTS = 0 (manual STARTTX/STARTRX/SUSPEND/STOP phase management).
 *   3. Poll EVENTS_LASTTX / EVENTS_LASTRX / EVENTS_ERROR with bounded loops.
 *   4. Always issue TASKS_STOP + clear ERRORSRC to leave the bus released.
 *
 * allow: SIZE_OK — single responsibility (sync TWIM1 register driver).
 * Splitting would scatter hardware register knowledge across files.
 */
#include "i2c_sync.h"

#ifdef BOARD_CLUE

#include "nrf.h"  /* NRF_TWIM1, NRF_TWIM_Type, TWIM_*_Msk macros */

/* Polling iteration bounds (match i2c_twim_backend.cpp proven values). */
#define SYNC_POLL_XFER  50000   /* LASTTX / LASTRX / ERROR */
#define SYNC_POLL_STOP  5000    /* SUSPENDED / STOPPED */

/* ---- finalize_stop: shared cleanup + result mapping ------------------- */

static i2c_sync_result_t finalize_stop(uint32_t errsrc)
{
	NRF_TWIM1->TASKS_STOP = 1;
	for (int i = 0; i < SYNC_POLL_STOP && !NRF_TWIM1->EVENTS_STOPPED; i++) { __NOP(); }
	NRF_TWIM1->EVENTS_STOPPED = 0;
	/* Writing ERRORSRC bits back clears the latched sticky bits (nRF52 ref man). */
	NRF_TWIM1->ERRORSRC = errsrc;
	NRF_TWIM1->EVENTS_ERROR = 0;
	if (errsrc == 0) return I2C_SYNC_OK;
	if (errsrc & (TWIM_ERRORSRC_DNACK_Msk | TWIM_ERRORSRC_ANACK_Msk))
		return I2C_SYNC_NACK;
	return I2C_SYNC_BUS_ERROR;
}

/* ---- i2c_sync_write --------------------------------------------------- */

i2c_sync_result_t i2c_sync_write(uint8_t addr, const uint8_t *data, size_t len)
{
	if (len == 0) return finalize_stop(0);

	NRF_TWIM1->ADDRESS = addr;
	NRF_TWIM1->TASKS_RESUME = 1;

	NRF_TWIM1->TXD.PTR     = (uint32_t)data;
	NRF_TWIM1->TXD.MAXCNT  = (uint16_t)len;
	NRF_TWIM1->EVENTS_TXSTARTED = 0;
	NRF_TWIM1->EVENTS_LASTTX    = 0;
	NRF_TWIM1->EVENTS_ERROR      = 0;
	NRF_TWIM1->SHORTS = 0;
	NRF_TWIM1->TASKS_STARTTX = 1;

	for (int i = 0; i < SYNC_POLL_XFER &&
	     !NRF_TWIM1->EVENTS_LASTTX && !NRF_TWIM1->EVENTS_ERROR; i++)
	{ __NOP(); }

	uint32_t errsrc = NRF_TWIM1->ERRORSRC;
	NRF_TWIM1->EVENTS_LASTTX = 0;

	/* SUSPEND clock-low-hold before STOP (matches proven backend sequence). */
	NRF_TWIM1->TASKS_SUSPEND = 1;
	for (int i = 0; i < SYNC_POLL_STOP && !NRF_TWIM1->EVENTS_SUSPENDED; i++) { __NOP(); }
	NRF_TWIM1->EVENTS_SUSPENDED = 0;

	return finalize_stop(errsrc);
}

/* ---- i2c_sync_read_reg (write 1-byte reg, SUSPEND, then read) -------- */

i2c_sync_result_t i2c_sync_read_reg(uint8_t addr, uint8_t reg, uint8_t *buf, size_t len)
{
	if (len == 0) return finalize_stop(0);

	uint8_t reg_byte = reg;  /* local — must outlive the TXD pointer deref */

	NRF_TWIM1->ADDRESS = addr;
	NRF_TWIM1->TASKS_RESUME = 1;

	/* Write phase: send register address byte. */
	NRF_TWIM1->TXD.PTR     = (uint32_t)&reg_byte;
	NRF_TWIM1->TXD.MAXCNT  = 1;
	NRF_TWIM1->EVENTS_TXSTARTED = 0;
	NRF_TWIM1->EVENTS_LASTTX    = 0;
	NRF_TWIM1->EVENTS_ERROR      = 0;
	NRF_TWIM1->SHORTS = 0;
	NRF_TWIM1->TASKS_STARTTX = 1;

	for (int i = 0; i < SYNC_POLL_XFER &&
	     !NRF_TWIM1->EVENTS_LASTTX && !NRF_TWIM1->EVENTS_ERROR; i++)
	{ __NOP(); }

	uint32_t errsrc = NRF_TWIM1->ERRORSRC;
	if (errsrc != 0) {
		NRF_TWIM1->EVENTS_LASTTX = 0;
		return finalize_stop(errsrc);
	}
	NRF_TWIM1->EVENTS_LASTTX = 0;

	/* SUSPEND holds SCL low so the slave retains its register pointer. */
	NRF_TWIM1->TASKS_SUSPEND = 1;
	for (int i = 0; i < SYNC_POLL_STOP && !NRF_TWIM1->EVENTS_SUSPENDED; i++) { __NOP(); }
	NRF_TWIM1->EVENTS_SUSPENDED = 0;

	/* Read phase: RESUME releases the clock, STARTRX begins the read. */
	NRF_TWIM1->RXD.PTR     = (uint32_t)buf;
	NRF_TWIM1->RXD.MAXCNT  = (uint16_t)len;
	NRF_TWIM1->EVENTS_RXSTARTED = 0;
	NRF_TWIM1->EVENTS_LASTRX    = 0;
	NRF_TWIM1->EVENTS_ERROR      = 0;
	NRF_TWIM1->SHORTS = 0;
	NRF_TWIM1->TASKS_RESUME = 1;
	NRF_TWIM1->TASKS_STARTRX = 1;

	for (int i = 0; i < SYNC_POLL_XFER &&
	     !NRF_TWIM1->EVENTS_LASTRX && !NRF_TWIM1->EVENTS_ERROR; i++)
	{ __NOP(); }

	errsrc = NRF_TWIM1->ERRORSRC;
	NRF_TWIM1->EVENTS_LASTRX = 0;

	return finalize_stop(errsrc);
}

/* ---- i2c_sync_read_only (bare read, no preceding write) --------------- */

i2c_sync_result_t i2c_sync_read_only(uint8_t addr, uint8_t *buf, size_t len)
{
	if (len == 0) return finalize_stop(0);

	NRF_TWIM1->ADDRESS = addr;
	NRF_TWIM1->TASKS_RESUME = 1;

	NRF_TWIM1->RXD.PTR     = (uint32_t)buf;
	NRF_TWIM1->RXD.MAXCNT  = (uint16_t)len;
	NRF_TWIM1->EVENTS_RXSTARTED = 0;
	NRF_TWIM1->EVENTS_LASTRX    = 0;
	NRF_TWIM1->EVENTS_ERROR      = 0;
	NRF_TWIM1->SHORTS = 0;
	NRF_TWIM1->TASKS_STARTRX = 1;

	for (int i = 0; i < SYNC_POLL_XFER &&
	     !NRF_TWIM1->EVENTS_LASTRX && !NRF_TWIM1->EVENTS_ERROR; i++)
	{ __NOP(); }

	uint32_t errsrc = NRF_TWIM1->ERRORSRC;
	NRF_TWIM1->EVENTS_LASTRX = 0;

	return finalize_stop(errsrc);
}

/* ---- i2c_sync_write_reg_byte (convenience wrapper) ------------------- */

i2c_sync_result_t i2c_sync_write_reg_byte(uint8_t addr, uint8_t reg, uint8_t value)
{
	uint8_t payload[2] = { reg, value };
	return i2c_sync_write(addr, payload, 2);
}

#endif /* BOARD_CLUE */
