/*
 * sht31d_driver.cpp - Sht31dDriver wrapping SHT31-D i2cBus transfers.
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

#define SHT31D_PERIOD_US   1000000u  /* 1 Hz default */
#define SHT31D_RETRY_US    50000u    /* back off on NACK */

Sht31dDriver::Sht31dDriver()
	: m_state(IDLE)
	, m_present(false)
	, m_fresh(false)
	, m_active(false)
	, m_nextUs(0)
	, m_convertDeadline(0)
{
	memset(m_resp, 0, sizeof m_resp);
	memset(&m_last, 0, sizeof m_last);
}

void Sht31dDriver::s_completion(i2cbus_result_t r, void *user)
{
	static_cast<Sht31dDriver *>(user)->onComplete(r);
}

void Sht31dDriver::begin(uint64_t now_us)
{
	if (m_state != IDLE) return;
	if (!i2cbus_is_present(SHT31D_ADDR)) return;
	m_present = true;
	m_fresh   = false;
	m_active  = false;
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
	if (!m_present || m_active) return;
	if (now_us < m_nextUs) return;

	switch (m_state) {
	case IDLE:
		/* Start a new measurement cycle. */
		m_state = WRITING_CMD;
		m_nextUs = now_us;
		return;

	case WRITING_CMD: {
		/* Issue single-shot medium repeatability command (0x240B). */
		m_resp[0] = (uint8_t)(SHT31D_DEFAULT_CMD >> 8);
		m_resp[1] = (uint8_t)(SHT31D_DEFAULT_CMD & 0xFF);
		i2cbus_transfer_t xfer;
		memset(&xfer, 0, sizeof xfer);
		xfer.addr       = SHT31D_ADDR;
		xfer.write_buf  = m_resp;
		xfer.write_len  = 2;
		xfer.completion = s_completion;
		xfer.user       = this;
		if (i2cbus_enqueue(&xfer) != I2CBUS_TOKEN_INVALID) {
			m_active = true;
		}
		return;
	}

	case CONVERTING:
		/* Bus free during conversion; wait until deadline elapses. */
		if (now_us < m_convertDeadline) return;
		m_state = READING;
		m_nextUs = now_us;
		return;

	case READING: {
		i2cbus_transfer_t xfer;
		memset(&xfer, 0, sizeof xfer);
		xfer.addr       = SHT31D_ADDR;
		xfer.read_buf   = m_resp;
		xfer.read_len   = SHT31D_RESP_LEN;
		xfer.completion = s_completion;
		xfer.user       = this;
		if (i2cbus_enqueue(&xfer) != I2CBUS_TOKEN_INVALID) {
			m_active = true;
		}
		return;
	}
	}
}

void Sht31dDriver::onComplete(i2cbus_result_t r)
{
	m_active = false;
	uint64_t now = timebase_now_us();

	if (r != I2CBUS_OK) {
		/* NACK or bus error — back off, retry from cmd write. */
		m_state  = WRITING_CMD;
		m_nextUs = now + SHT31D_RETRY_US;
		return;
	}

	switch (m_state) {
	case WRITING_CMD:
		/* Conversion starts now; bus is free during conversion. */
		m_state           = CONVERTING;
		m_convertDeadline = now + SHT31D_DEFAULT_CONV_US;
		return;

	case READING: {
		sht31d_result_t rc = sht31d_convert(m_resp, &m_last);
		if (rc == SHT31D_OK) {
			m_fresh = true;
			m_state = IDLE;
			m_nextUs = now + SHT31D_PERIOD_US;
		} else {
			/* CRC error or out-of-range — retry promptly. */
			m_state = WRITING_CMD;
			m_nextUs = now + SHT31D_RETRY_US;
		}
		return;
	}

	default:
		m_state  = IDLE;
		m_nextUs = now + SHT31D_RETRY_US;
		return;
	}
}

#endif /* BOARD_CLUE */
