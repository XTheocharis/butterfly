/*
 * bmp280_driver.cpp - Bmp280Driver wrapping BMP280 sync I2C transfers.
 *
 * Sync FSM: read 24-byte calibration -> write ctrl + config -> poll
 * data at ~13 Hz -> compensate.  Cached for handleReadSensor.
 *
 * Sensors 6 (pressure Pa) + 7 (temperature milli-degC).
 */
#include "sensor_drivers.h"

#ifdef BOARD_CLUE

#include <string.h>
#include "../timebase.h"
#include "../i2c_sync.h"

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
	, m_configStep(0)
	, m_nextUs(0)
{
	memset(m_calibRaw, 0, sizeof m_calibRaw);
	memset(m_dataBuf, 0, sizeof m_dataBuf);
	memset(&m_calib, 0, sizeof m_calib);
	memset(&m_last, 0, sizeof m_last);
	m_tFine = 0;
}

void Bmp280Driver::begin(uint64_t now_us)
{
	if (m_state != IDLE) return;
	if (!i2cbus_is_present(BMP280_ADDR)) return;
	m_present    = true;
	m_state      = READING_CALIB;
	m_nextUs     = now_us;
	m_fresh      = false;
	m_configStep = 0;
}

bool Bmp280Driver::getLatest(bmp280_sample_t *out) const
{
	if (!m_fresh || out == NULL) return false;
	*out = m_last;
	return true;
}

/* Sync FSM: each tick runs one blocking TWIM1 transfer (~1ms at 400kHz).
 * Backoff 5ms on any error; retry from the current state on next tick. */
void Bmp280Driver::tick(uint64_t now_us)
{
	if (!m_present || now_us < m_nextUs) return;

	switch (m_state) {
	case READING_CALIB: {
		i2c_sync_result_t r = i2c_sync_read_reg(BMP280_ADDR, BMP280_REG_CALIB,
		                                        m_calibRaw, BMP280_CALIB_LEN);
		if (r != I2C_SYNC_OK) {
			m_nextUs = now_us + 5000u;
			return;
		}
		bmp280_parse_calib(m_calibRaw, &m_calib);
		m_state  = WRITING_CTRL;
		m_nextUs = now_us;
		return;
	}

	case WRITING_CTRL: {
		m_dataBuf[0] = BMP280_INIT_SEQ[m_configStep][0];
		m_dataBuf[1] = BMP280_INIT_SEQ[m_configStep][1];
		i2c_sync_result_t r = i2c_sync_write(BMP280_ADDR, m_dataBuf, 2);
		if (r != I2C_SYNC_OK) {
			m_nextUs = now_us + 5000u;
			return;
		}
		m_configStep++;
		if (m_configStep >= 2) {
			/* Both ctrl writes done — enter active sampling.
			 * One-time forced conversion delay before first data read. */
			m_state  = READING_DATA;
			m_nextUs = now_us + BMP280_CONV_FORCED_DEFAULT_US;
		} else {
			m_nextUs = now_us;
		}
		return;
	}

	case READING_DATA: {
		i2c_sync_result_t r = i2c_sync_read_reg(BMP280_ADDR, BMP280_REG_DATA,
		                                        m_dataBuf, BMP280_BURST_LEN);
		if (r != I2C_SYNC_OK) {
			m_nextUs = now_us + 5000u;
			return;
		}
		bmp280_compensate(m_dataBuf, &m_calib, &m_tFine, &m_last);
		m_fresh  = true;
		m_nextUs = now_us + BMP280_PERIOD_US;
		return;
	}

	case IDLE:
	default:
		return;
	}
}

#endif /* BOARD_CLUE */
