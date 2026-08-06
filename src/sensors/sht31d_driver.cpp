/*
 * sht31d_driver.cpp - Sht31dDriver wrapping SHT31-D sync I2C transfers.
 *
 * Single-shot medium repeatability cycle: write 2-byte cmd -> wait
 * 6.5 ms (bus free during conversion) -> read 6 bytes (T+RH+CRC*2).
 * Default rate 1 Hz.  Cached for handleReadSensor.
 *
 * Sensors 8 (humidity milli-%RH) + 9 (temperature milli-degC).
 */
#include "sensor_drivers.h"

#ifdef BOARD_CLUE

#include <string.h>
#include "../timebase.h"
#include "../i2c_sync.h"

#define SHT31D_PERIOD_US   1000000u  /* 1 Hz default */
#define SHT31D_RETRY_US    50000u    /* back off on CRC error */
#define SHT31D_I2C_RETRY_US 5000u    /* back off on I2C NACK/error */

Sht31dDriver::Sht31dDriver()
	: m_state(IDLE)
	, m_present(false)
	, m_fresh(false)
	, m_nextUs(0)
	, m_convertDeadline(0)
{
	memset(m_resp, 0, sizeof m_resp);
	memset(&m_last, 0, sizeof m_last);
}

void Sht31dDriver::begin(uint64_t now_us)
{
	if (m_state != IDLE) return;
	if (!i2cbus_is_present(SHT31D_ADDR)) return;
	m_present = true;
	m_fresh   = false;
	m_state   = WRITING_CMD;
	m_nextUs  = now_us;
}

bool Sht31dDriver::getLatest(sht31d_sample_t *out) const
{
	if (!m_fresh || out == NULL) return false;
	*out = m_last;
	return true;
}

void Sht31dDriver::tick(uint64_t now_us)
{
	if (!m_present) return;
	if (now_us < m_nextUs) return;

	switch (m_state) {
	case IDLE:
		/* Start a new measurement cycle. */
		m_state = WRITING_CMD;
		m_nextUs = now_us;
		return;

	case WRITING_CMD: {
		/* Issue single-shot medium repeatability command (0x240B). */
		uint8_t cmd[2];
		cmd[0] = (uint8_t)(SHT31D_DEFAULT_CMD >> 8);
		cmd[1] = (uint8_t)(SHT31D_DEFAULT_CMD & 0xFF);
		i2c_sync_result_t r = i2c_sync_write(SHT31D_ADDR, cmd, 2);
		if (r == I2C_SYNC_OK) {
			/* Conversion starts now; bus is free during conversion. */
			m_state           = CONVERTING;
			m_convertDeadline = now_us + SHT31D_DEFAULT_CONV_US;
			return;
		}
		/* I2C NACK or bus error — short backoff, retry cmd write. */
		m_state  = WRITING_CMD;
		m_nextUs = now_us + SHT31D_I2C_RETRY_US;
		return;
	}

	case CONVERTING:
		/* Bus free during conversion; wait until deadline elapses. */
		if (now_us < m_convertDeadline) return;
		m_state = READING;
		m_nextUs = now_us;
		return;

	case READING: {
		/* Bare read of 6 bytes (no register prefix — sensor already
		 * primed by the command write). */
		i2c_sync_result_t r = i2c_sync_read_only(SHT31D_ADDR,
		                                         m_resp, SHT31D_RESP_LEN);
		if (r != I2C_SYNC_OK) {
			/* I2C error — short backoff, retry from cmd write. */
			m_state  = WRITING_CMD;
			m_nextUs = now_us + SHT31D_I2C_RETRY_US;
			return;
		}
		sht31d_result_t rc = sht31d_convert(m_resp, &m_last);
		if (rc == SHT31D_OK) {
			m_fresh = true;
			m_state = IDLE;
			m_nextUs = now_us + SHT31D_PERIOD_US;
		} else {
			/* CRC error or out-of-range — retry from cmd write. */
			m_state  = WRITING_CMD;
			m_nextUs = now_us + SHT31D_RETRY_US;
		}
		return;
	}
	}
}

#endif /* BOARD_CLUE */
