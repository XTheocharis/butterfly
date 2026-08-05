/*
 * imu_driver.cpp - ImuDriver wrapping LSM6DS33 i2cBus transfers.
 *
 * Configures the 4 sensor registers async, then bursts 12 bytes at
 * 104 Hz. Parsed samples are injected into BoardModule.
 */
#include "sensor_drivers.h"

#ifdef BOARD_CLUE

#include <string.h>
#include <whad.h>
#include "../boardModule.h"
#include "../timebase.h"

/* 104 Hz sample period in microseconds. */
#define IMU_PERIOD_US  9615u

ImuDriver::ImuDriver()
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

void ImuDriver::s_completion(i2cbus_result_t r, void *user)
{
	static_cast<ImuDriver *>(user)->onComplete(r);
}

void ImuDriver::begin(uint64_t now_us)
{
	if (m_state != IDLE) return;
	if (!i2cbus_is_present(IMU_ADDR)) return;
	m_present    = true;
	m_state      = CONFIGURING;
	m_configStep = 0;
	m_active     = false;
	m_nextUs     = now_us;
	m_fresh      = false;
}

void ImuDriver::tick(uint64_t now_us)
{
	if (!m_present) return;

	if (m_state == CONFIGURING) {
		if (m_active || now_us < m_nextUs) return;
		if (m_configStep >= IMU_CONFIG_COUNT) {
			m_state  = ACTIVE;
			m_nextUs = now_us + 1000u; /* 1ms settle before first burst */
			return;
		}
		uint8_t reg   = IMU_CONFIG_SEQUENCE[m_configStep].reg;
		uint8_t value = IMU_CONFIG_SEQUENCE[m_configStep].value;
		m_buf[0] = reg;
		m_buf[1] = value;
		i2cbus_transfer_t xfer;
		memset(&xfer, 0, sizeof xfer);
		xfer.addr       = IMU_ADDR;
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
		/* Issue a burst read: write OUTX_L_G, then read 12 bytes. */
		m_buf[0] = IMU_BURST_REG;
		i2cbus_transfer_t xfer;
		memset(&xfer, 0, sizeof xfer);
		xfer.addr       = IMU_ADDR;
		xfer.write_buf  = m_buf;
		xfer.write_len  = 1;
		xfer.read_buf   = m_buf;  /* overwrite the reg byte with the burst */
		xfer.read_len   = IMU_BURST_LEN;
		xfer.completion = s_completion;
		xfer.user       = this;
		if (i2cbus_enqueue(&xfer) != I2CBUS_TOKEN_INVALID) {
			m_active = true;
		}
	}
}

void ImuDriver::onComplete(i2cbus_result_t r)
{
	m_active = false;
	if (r != I2CBUS_OK) {
		/* NACK or bus error — back off briefly, retry next tick. */
		m_nextUs = timebase_now_us() + 5000u;
		return;
	}

	if (m_state == CONFIGURING) {
		m_configStep++;
		m_nextUs = timebase_now_us();  /* next write on next tick */
		return;
	}

	if (m_state == ACTIVE) {
		imu_raw_t raw;
		imu_parse_burst(m_buf, &raw);
		imu_convert(&raw, &m_last);
		imu_transform_axes(&m_last);
		m_fresh  = true;
		m_nextUs = timebase_now_us() + IMU_PERIOD_US;
		if (m_bm) {
			m_bm->injectImu(&m_last, timebase_now_us());
		}
	}
}

#endif /* BOARD_CLUE */
