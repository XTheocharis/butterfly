/*
 * imu_driver.cpp - ImuDriver wrapping LSM6DS33 synchronous I2C transfers.
 *
 * Configures the 4 sensor registers via blocking writes, then bursts
 * 12 bytes at 104 Hz. Parsed samples are injected into BoardModule.
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

/* 104 Hz sample period in microseconds. */
#define IMU_PERIOD_US  9615u

/* Backoff after a NACK or bus error before retrying. */
#define IMU_BACKOFF_US 5000u

ImuDriver::ImuDriver()
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

void ImuDriver::begin(uint64_t now_us)
{
	if (m_state != IDLE) return;
	if (!i2cbus_is_present(IMU_ADDR)) return;
	m_present    = true;
	m_state      = CONFIGURING;
	m_configStep = 0;
	m_nextUs     = now_us;
	m_fresh      = false;
}

void ImuDriver::tick(uint64_t now_us)
{
	if (!m_present) return;

	if (m_state == CONFIGURING) {
		if (now_us < m_nextUs) return;
		if (m_configStep >= IMU_CONFIG_COUNT) {
			m_state  = ACTIVE;
			m_nextUs = now_us + 1000u; /* 1ms settle before first burst */
			return;
		}
		m_buf[0] = IMU_CONFIG_SEQUENCE[m_configStep].reg;
		m_buf[1] = IMU_CONFIG_SEQUENCE[m_configStep].value;
		i2c_sync_result_t r = i2c_sync_write(IMU_ADDR, m_buf, 2);
		if (r == I2C_SYNC_OK) {
			m_configStep++;
			m_nextUs = now_us;  /* next write on next tick */
		} else {
			/* NACK or bus error — back off briefly, retry next tick. */
			m_nextUs = now_us + IMU_BACKOFF_US;
		}
		return;
	}

	if (m_state == ACTIVE && now_us >= m_nextUs) {
		i2c_sync_result_t r = i2c_sync_read_reg(IMU_ADDR, IMU_BURST_REG,
		                                        m_buf, IMU_BURST_LEN);
		if (r != I2C_SYNC_OK) {
			m_nextUs = now_us + IMU_BACKOFF_US;
			return;
		}
		imu_raw_t raw;
		imu_parse_burst(m_buf, &raw);
		imu_convert(&raw, &m_last);
		imu_transform_axes(&m_last);
		m_fresh  = true;
		m_nextUs = now_us + IMU_PERIOD_US;
		if (m_bm) {
			m_bm->injectImu(&m_last, timebase_now_us());
		}
	}
}

#endif /* BOARD_CLUE */
