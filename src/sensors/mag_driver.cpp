/*
 * mag_driver.cpp - MagDriver wrapping LIS3MDL i2cBus transfers.
 *
 * Configures 5 sensor registers async, then bursts 6 bytes at 40 Hz.
 * Parsed samples pass through field-health check before injection.
 */
#include "sensor_drivers.h"

#ifdef BOARD_CLUE

#include <string.h>
#include <whad.h>
#include "../boardModule.h"
#include "../timebase.h"

/* 40 Hz sample period in microseconds. */
#define MAG_PERIOD_US  25000u

MagDriver::MagDriver()
	: m_bm(NULL)
	, m_state(IDLE)
	, m_configStep(0)
	, m_active(false)
	, m_present(false)
	, m_fresh(false)
	, m_nextUs(0)
{
	memset(&m_last, 0, sizeof m_last);
	memset(m_buf, 0, sizeof m_buf);
}

void MagDriver::s_completion(i2cbus_result_t r, void *user)
{
	static_cast<MagDriver *>(user)->onComplete(r);
}

void MagDriver::begin(uint64_t now_us)
{
	if (m_state != IDLE) return;
	if (!i2cbus_is_present(MAG_ADDR)) return;
	m_present    = true;
	m_state      = CONFIGURING;
	m_configStep = 0;
	m_active     = false;
	m_nextUs     = now_us;
	m_fresh      = false;
}

void MagDriver::tick(uint64_t now_us)
{
	if (!m_present) return;

	if (m_state == CONFIGURING) {
		if (m_active || now_us < m_nextUs) return;
		if (m_configStep >= MAG_CONFIG_COUNT) {
			m_state  = ACTIVE;
			m_nextUs = now_us + 1000u;
			return;
		}
		uint8_t reg   = MAG_CONFIG_SEQUENCE[m_configStep].reg;
		uint8_t value = MAG_CONFIG_SEQUENCE[m_configStep].value;
		m_buf[0] = reg;
		m_buf[1] = value;
		i2cbus_transfer_t xfer;
		memset(&xfer, 0, sizeof xfer);
		xfer.addr       = MAG_ADDR;
		xfer.write_buf  = m_buf;
		xfer.write_len  = 2;
		xfer.completion = s_completion;
		xfer.user       = this;
		if (i2cbus_enqueue(&xfer) != I2CBUS_TOKEN_INVALID) {
			m_active = true;
		}
		return;
	}

	if (m_state == ACTIVE && !m_active && now_us >= m_nextUs) {
		m_buf[0] = MAG_BURST_REG;
		i2cbus_transfer_t xfer;
		memset(&xfer, 0, sizeof xfer);
		xfer.addr       = MAG_ADDR;
		xfer.write_buf  = m_buf;
		xfer.write_len  = 1;
		xfer.read_buf   = m_buf;
		xfer.read_len   = MAG_BURST_LEN;
		xfer.completion = s_completion;
		xfer.user       = this;
		if (i2cbus_enqueue(&xfer) != I2CBUS_TOKEN_INVALID) {
			m_active = true;
		}
	}
}

void MagDriver::onComplete(i2cbus_result_t r)
{
	m_active = false;
	if (r != I2CBUS_OK) {
		m_nextUs = timebase_now_us() + 5000u;
		return;
	}

	if (m_state == CONFIGURING) {
		m_configStep++;
		m_nextUs = timebase_now_us();
		return;
	}

	if (m_state == ACTIVE) {
		mag_raw_t raw;
		mag_parse_burst(m_buf, &raw);
		mag_convert(&raw, &m_last);
		mag_transform_axes(&m_last);
		bool healthy = mag_field_healthy(&m_last,
		                                 MAG_FIELD_NORM_DEFAULT_MG,
		                                 MAG_FIELD_HEALTH_PCT);
		m_fresh  = true;
		m_nextUs = timebase_now_us() + MAG_PERIOD_US;
		if (m_bm) {
			m_bm->injectMag(&m_last, healthy, timebase_now_us());
		}
	}
}

#endif /* BOARD_CLUE */
