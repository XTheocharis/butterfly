/*
 * apds9960_driver.cpp - Apds9960Driver wrapping APDS-9960 i2cBus transfers.
 *
 * Configures golden optical-mode registers async, then periodically reads
 * 8-byte RGBC + 1-byte proximity. Cached for handleReadSensor sensors
 * 10 (color) and 11 (proximity). Gesture mode wiring is left to T21+.
 *
 * Sensor 12 (gesture) is currently optical-only; gestures are not yet
 * detected here. The motion subsystem requests gesture mode via a
 * separate API path (T21+).
 */
#include "sensor_drivers.h"

#ifdef BOARD_CLUE

#include <string.h>
#include <whad.h>
#include "../boardModule.h"
#include "../timebase.h"

/* Optical mode sample period (~10 Hz, dominated by 178ms ATIME). */
#define APDS_PERIOD_US  200000u

/* Golden optical-mode config sequence (reg, value) pairs.
 * ENABLE is written last so PON+AEN+PEN activate together. */
static const uint8_t APDS_INIT_SEQ[][2] = {
	{ APDS9960_REG_ATIME,  APDS9960_GOLDEN_ATIME  },
	{ APDS9960_REG_WTIME,  APDS9960_GOLDEN_WTIME  },
	{ APDS9960_REG_PPULSE, APDS9960_GOLDEN_PPULSE },
	{ APDS9960_REG_CONTROL, APDS9960_GOLDEN_CONTROL },
	{ APDS9960_REG_CONFIG2, APDS9960_GOLDEN_CONFIG2 },
	{ APDS9960_REG_PILT,   0u                       },
	{ APDS9960_REG_PIHT,   255u                     },
	{ APDS9960_REG_PERS,   0x11u                    }, /* 1 ALS + 1 prox persistence */
	{ APDS9960_REG_ENABLE, APDS9960_GOLDEN_ENABLE   },
};
#define APDS_INIT_COUNT  (sizeof(APDS_INIT_SEQ) / sizeof(APDS_INIT_SEQ[0]))

Apds9960Driver::Apds9960Driver()
	: m_bm(NULL)
	, m_state(IDLE)
	, m_configStep(0)
	, m_active(false)
	, m_present(false)
	, m_fresh(false)
	, m_nextUs(0)
	, m_proxVal(0)
{
	memset(m_rgbcBuf, 0, sizeof m_rgbcBuf);
	memset(&m_last, 0, sizeof m_last);
}

void Apds9960Driver::s_completion(i2cbus_result_t r, void *user)
{
	static_cast<Apds9960Driver *>(user)->onComplete(r);
}

void Apds9960Driver::begin(uint64_t now_us)
{
	if (m_state != IDLE) return;
	if (!i2cbus_is_present(APDS9960_ADDR)) return;
	m_present    = true;
	m_state      = CONFIGURING;
	m_configStep = 0;
	m_active     = false;
	m_fresh      = false;
	m_nextUs     = now_us;
}

bool Apds9960Driver::getLatestOptical(apds9960_optical_sample_t *out) const
{
	if (!m_fresh || out == NULL) return false;
	*out = m_last;
	return true;
}

void Apds9960Driver::tick(uint64_t now_us)
{
	if (!m_present || m_active || now_us < m_nextUs) return;

	if (m_state == CONFIGURING) {
		if (m_configStep >= APDS_INIT_COUNT) {
			m_state  = OPTICAL_BURST;
			m_nextUs = now_us + 1000u;
			return;
		}
		m_rgbcBuf[0] = APDS_INIT_SEQ[m_configStep][0];
		m_rgbcBuf[1] = APDS_INIT_SEQ[m_configStep][1];
		i2cbus_transfer_t xfer;
		memset(&xfer, 0, sizeof xfer);
		xfer.addr       = APDS9960_ADDR;
		xfer.write_buf  = m_rgbcBuf;
		xfer.write_len  = 2;
		xfer.completion = s_completion;
		xfer.user       = this;
		if (i2cbus_enqueue(&xfer) != I2CBUS_TOKEN_INVALID) {
			m_active = true;
		}
		return;
	}

	if (m_state == OPTICAL_BURST) {
		/* Read RGBC (8 bytes from CDATAL=0x94). */
		m_rgbcBuf[0] = APDS9960_REG_CDATAL;
		i2cbus_transfer_t xfer;
		memset(&xfer, 0, sizeof xfer);
		xfer.addr       = APDS9960_ADDR;
		xfer.write_buf  = m_rgbcBuf;
		xfer.write_len  = 1;
		xfer.read_buf   = m_rgbcBuf;
		xfer.read_len   = APDS9960_RGBC_LEN;
		xfer.completion = s_completion;
		xfer.user       = this;
		if (i2cbus_enqueue(&xfer) != I2CBUS_TOKEN_INVALID) {
			m_active = true;
		}
		return;
	}

	if (m_state == READING_PROX) {
		/* Read proximity (1 byte from PDATA=0x9C). */
		m_rgbcBuf[0] = APDS9960_REG_PDATA;
		i2cbus_transfer_t xfer;
		memset(&xfer, 0, sizeof xfer);
		xfer.addr       = APDS9960_ADDR;
		xfer.write_buf  = m_rgbcBuf;
		xfer.write_len  = 1;
		xfer.read_buf   = &m_proxVal;
		xfer.read_len   = 1;
		xfer.completion = s_completion;
		xfer.user       = this;
		if (i2cbus_enqueue(&xfer) != I2CBUS_TOKEN_INVALID) {
			m_active = true;
		}
		return;
	}
}

void Apds9960Driver::onComplete(i2cbus_result_t r)
{
	m_active = false;
	uint64_t now = timebase_now_us();

	if (r != I2CBUS_OK) {
		m_nextUs = now + 5000u;
		return;
	}

	if (m_state == CONFIGURING) {
		m_configStep++;
		m_nextUs = now;
		return;
	}

	if (m_state == OPTICAL_BURST) {
		/* RGBC read done — parse it, then chain to proximity read. */
		apds9960_parse_rgbc(m_rgbcBuf, &m_last);
		m_last.proximity = m_proxVal;  /* last known prox carried over */
		m_state  = READING_PROX;
		m_nextUs = now;
		return;
	}

	if (m_state == READING_PROX) {
		m_last.proximity = apds9960_parse_proximity(m_proxVal);
		m_fresh = true;
		m_state = OPTICAL_BURST;
		m_nextUs = now + APDS_PERIOD_US;
		/* Gesture detection (sensor 12) is not yet wired here. */
		(void)m_bm;
		return;
	}
}

#endif /* BOARD_CLUE */
