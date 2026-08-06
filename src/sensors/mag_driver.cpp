/*
 * mag_driver.cpp - MagDriver wrapping LIS3MDL synchronous I2C transfers.
 *
 * Configures 5 sensor registers via blocking i2c_sync_write calls, then
 * bursts 6 bytes at 40 Hz via i2c_sync_read_reg. Parsed samples pass
 * through the field-health check before injection into BoardModule.
 *
 * Sync transfers replace the prior async i2cbus_enqueue/onComplete path;
 * the IDLE → CONFIGURING → ACTIVE state machine shape is preserved.
 */
#include "sensor_drivers.h"

#ifdef BOARD_CLUE

#include <string.h>
#include <whad.h>
#include "../boardModule.h"
#include "../timebase.h"
#include "../i2c_sync.h"

/* 40 Hz sample period in microseconds. */
#define MAG_PERIOD_US   25000u

/* Backoff after a NACK or bus error (retries next tick). */
#define MAG_BACKOFF_US  5000u

MagDriver::MagDriver()
	: m_bm(NULL)
	, m_state(IDLE)
	, m_configStep(0)
	, m_present(false)
	, m_fresh(false)
	, m_nextUs(0)
{
	memset(&m_last, 0, sizeof m_last);
	memset(m_buf, 0, sizeof m_buf);
}

void MagDriver::begin(uint64_t now_us)
{
	if (m_state != IDLE) return;
	if (!i2cbus_is_present(MAG_ADDR)) return;
	m_present    = true;
	m_state      = CONFIGURING;
	m_configStep = 0;
	m_nextUs     = now_us;
	m_fresh      = false;
}

void MagDriver::tick(uint64_t now_us)
{
	if (!m_present) return;
	if (now_us < m_nextUs) return;

	if (m_state == CONFIGURING) {
		if (m_configStep >= MAG_CONFIG_COUNT) {
			m_state  = ACTIVE;
			m_nextUs = now_us + 1000u; /* 1ms settle before first burst */
			return;
		}
		uint8_t reg   = MAG_CONFIG_SEQUENCE[m_configStep].reg;
		uint8_t value = MAG_CONFIG_SEQUENCE[m_configStep].value;
		m_buf[0] = reg;
		m_buf[1] = value;
		if (i2c_sync_write(MAG_ADDR, m_buf, 2) != I2C_SYNC_OK) {
			/* NACK or bus error — back off, retry same reg next tick. */
			m_nextUs = timebase_now_us() + MAG_BACKOFF_US;
			return;
		}
		m_configStep++;
		m_nextUs = timebase_now_us();  /* next write on next tick */
		return;
	}

	if (m_state == ACTIVE) {
		if (i2c_sync_read_reg(MAG_ADDR, MAG_BURST_REG, m_buf, MAG_BURST_LEN) != I2C_SYNC_OK) {
			m_nextUs = timebase_now_us() + MAG_BACKOFF_US;
			return;
		}
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
