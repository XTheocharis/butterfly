/*
 * bmp280_driver.cpp - Bmp280Driver wrapping BMP280 i2cBus transfers.
 *
 * Async FSM: read 24-byte calibration -> write ctrl + config -> poll
 * data at ~13 Hz -> compensate.  Cached for handleReadSensor.
 *
 * Sensors 6 (pressure Pa) + 7 (temperature milli-degC).
 */
#include "sensor_drivers.h"

#ifdef BOARD_CLUE

#include <string.h>
#include "../timebase.h"

/* Normal-mode period (~13 Hz from BMP280_CONTINUOUS_RATE_HZ). */
#define BMP280_PERIOD_US  77000u

/* Two ctrl writes during init: config then ctrl_meas. */
static const uint8_t BMP280_INIT_SEQ[2][2] = {
	{ BMP280_REG_CONFIG,    BMP280_CONFIG_NORMAL_DEFAULT    },
	{ BMP280_REG_CTRL_MEAS, BMP280_CTRL_MEAS_NORMAL_DEFAULT },
};

Bmp280Driver::Bmp280Driver()
	: m_state(IDLE)
	, m_present(false)
	, m_fresh(false)
	, m_active(false)
	, m_configStep(0)
	, m_nextUs(0)
{
	memset(m_calibRaw, 0, sizeof m_calibRaw);
	memset(m_dataBuf, 0, sizeof m_dataBuf);
	memset(&m_calib, 0, sizeof m_calib);
	memset(&m_last, 0, sizeof m_last);
	m_tFine = 0;
}

void Bmp280Driver::s_completion(i2cbus_result_t r, void *user)
{
	static_cast<Bmp280Driver *>(user)->onComplete(r);
}

void Bmp280Driver::begin(uint64_t now_us)
{
	if (m_state != IDLE) return;
	if (!i2cbus_is_present(BMP280_ADDR)) return;
	m_present    = true;
	m_state      = READING_CALIB;
	m_nextUs     = now_us;
	m_fresh      = false;
	m_active     = false;
	m_configStep = 0;
}

bool Bmp280Driver::getLatest(bmp280_sample_t *out) const
{
	if (!m_fresh || out == NULL) return false;
	*out = m_last;
	return true;
}

/* m_active guards against re-enqueue while a transfer is outstanding:
 * i2cBus only allows one active transfer at a time, so a second enqueue
 * would be rejected anyway — m_active avoids the wasted call. */
void Bmp280Driver::tick(uint64_t now_us)
{
	if (!m_present || m_active || now_us < m_nextUs) return;

	switch (m_state) {
	case READING_CALIB:
		m_calibRaw[0] = BMP280_REG_CALIB;
		{
			i2cbus_transfer_t xfer;
			memset(&xfer, 0, sizeof xfer);
			xfer.addr       = BMP280_ADDR;
			xfer.write_buf  = m_calibRaw;
			xfer.write_len  = 1;
			xfer.read_buf   = m_calibRaw;
			xfer.read_len   = BMP280_CALIB_LEN;
			xfer.completion = s_completion;
			xfer.user       = this;
			if (i2cbus_enqueue(&xfer) != I2CBUS_TOKEN_INVALID) {
				m_active = true;
			}
		}
		return;

	case WRITING_CTRL:
		if (m_configStep >= 2) {
			/* Both ctrl writes done — enter active sampling. */
			m_state  = READING_DATA;
			m_nextUs = now_us + BMP280_CONV_FORCED_DEFAULT_US;
			return;
		}
		m_dataBuf[0] = BMP280_INIT_SEQ[m_configStep][0];
		m_dataBuf[1] = BMP280_INIT_SEQ[m_configStep][1];
		{
			i2cbus_transfer_t xfer;
			memset(&xfer, 0, sizeof xfer);
			xfer.addr       = BMP280_ADDR;
			xfer.write_buf  = m_dataBuf;
			xfer.write_len  = 2;
			xfer.completion = s_completion;
			xfer.user       = this;
			if (i2cbus_enqueue(&xfer) != I2CBUS_TOKEN_INVALID) {
				m_active = true;
			}
		}
		return;

	case READING_DATA:
		m_dataBuf[0] = BMP280_REG_DATA;
		{
			i2cbus_transfer_t xfer;
			memset(&xfer, 0, sizeof xfer);
			xfer.addr       = BMP280_ADDR;
			xfer.write_buf  = m_dataBuf;
			xfer.write_len  = 1;
			xfer.read_buf   = m_dataBuf;
			xfer.read_len   = BMP280_BURST_LEN;
			xfer.completion = s_completion;
			xfer.user       = this;
			if (i2cbus_enqueue(&xfer) != I2CBUS_TOKEN_INVALID) {
				m_active = true;
			}
		}
		return;

	case IDLE:
	default:
		return;
	}
}

void Bmp280Driver::onComplete(i2cbus_result_t r)
{
	m_active = false;
	uint64_t now = timebase_now_us();

	if (r != I2CBUS_OK) {
		/* Back off briefly; retry from current state on next tick. */
		m_nextUs = now + 5000u;
		return;
	}

	switch (m_state) {
	case READING_CALIB:
		bmp280_parse_calib(m_calibRaw, &m_calib);
		m_state  = WRITING_CTRL;
		m_nextUs = now;
		return;

	case WRITING_CTRL:
		m_configStep++;
		m_nextUs = now;
		return;

	case READING_DATA:
		bmp280_compensate(m_dataBuf, &m_calib, &m_tFine, &m_last);
		m_fresh  = true;
		m_nextUs = now + BMP280_PERIOD_US;
		return;

	default:
		m_nextUs = now;
		return;
	}
}

#endif /* BOARD_CLUE */
