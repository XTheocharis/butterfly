/*
 * apds9960_driver.cpp - Apds9960Driver wrapping APDS-9960 synchronous I2C transfers.
 *
 * Configures golden optical-mode registers via blocking writes, then
 * periodically reads 8-byte RGBC + 1-byte proximity. Cached for
 * handleReadSensor sensors 10 (color) and 11 (proximity). Gesture mode
 * is NOT entered (GEN never set); sensor 12 stays STALE by design.
 *
 * Uses the i2c_sync API (raw NRF_TWIM1 register polling, Adafruit Wire
 * pattern) instead of the async i2cbus pipeline. Transfers complete in
 * the caller's frame; no completion callback is required.
 */
#include "sensor_drivers.h"

#ifdef BOARD_CLUE

#include <string.h>
#include <whad.h>
#include "../boardModule.h"
#include "../timebase.h"
#include "../i2c_sync.h"

/* Optical mode sample period (~5 Hz, dominated by 178ms ATIME integration). */
#define APDS_PERIOD_US  200000u

/* Backoff after a NACK or bus error before retrying. */
#define APDS_BACKOFF_US 5000u

/* Golden optical-mode config sequence (reg, value) pairs.
 * ENABLE (0x80) is written LAST so PON+AEN+PEN activate together. */
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
	, m_present(false)
	, m_fresh(false)
	, m_nextUs(0)
	, m_proxVal(0)
{
	memset(m_rgbcBuf, 0, sizeof m_rgbcBuf);
	memset(&m_last, 0, sizeof m_last);
}

void Apds9960Driver::begin(uint64_t now_us)
{
	if (m_state != IDLE) return;
	if (!i2cbus_is_present(APDS9960_ADDR)) return;
	m_present    = true;
	m_state      = CONFIGURING;
	m_configStep = 0;
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
	if (!m_present || now_us < m_nextUs) return;

	if (m_state == CONFIGURING) {
		if (m_configStep >= APDS_INIT_COUNT) {
			m_state  = OPTICAL_BURST;
			m_nextUs = now_us + 1000u; /* 1ms settle before first burst */
			return;
		}
		i2c_sync_result_t r = i2c_sync_write_reg_byte(
			APDS9960_ADDR,
			APDS_INIT_SEQ[m_configStep][0],
			APDS_INIT_SEQ[m_configStep][1]);
		if (r == I2C_SYNC_OK) {
			m_configStep++;
			m_nextUs = now_us;  /* next write on next tick */
		} else {
			/* NACK or bus error — back off briefly, retry next tick. */
			m_nextUs = now_us + APDS_BACKOFF_US;
		}
		return;
	}

	if (m_state == OPTICAL_BURST) {
		/* Read 8-byte RGBC burst from CDATAL=0x94 (auto-increment). */
		i2c_sync_result_t r = i2c_sync_read_reg(
			APDS9960_ADDR, APDS9960_REG_CDATAL,
			m_rgbcBuf, APDS9960_RGBC_LEN);
		if (r != I2C_SYNC_OK) {
			m_nextUs = now_us + APDS_BACKOFF_US;
			return;
		}
		apds9960_parse_rgbc(m_rgbcBuf, &m_last);
		m_state  = READING_PROX;
		m_nextUs = now_us;  /* chain to proximity read on next tick */
		return;
	}

	if (m_state == READING_PROX) {
		/* Read 1-byte proximity from PDATA=0x9C. */
		i2c_sync_result_t r = i2c_sync_read_reg(
			APDS9960_ADDR, APDS9960_REG_PDATA, &m_proxVal, 1);
		if (r != I2C_SYNC_OK) {
			m_nextUs = now_us + APDS_BACKOFF_US;
			return;
		}
		m_last.proximity = m_proxVal;
		m_fresh = true;
		m_state = OPTICAL_BURST;
		m_nextUs = now_us + APDS_PERIOD_US;
		/* Gesture detection (sensor 12) is not wired here by design. */
		(void)m_bm;
		return;
	}
}

#endif /* BOARD_CLUE */
