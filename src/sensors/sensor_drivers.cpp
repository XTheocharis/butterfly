/*
 * sensor_drivers.cpp - SensorDrivers aggregator implementation.
 *
 * Owns the 5 wrapper instances + drives i2cbus_tick each pass.
 * T21 constructs this inside BoardModule and forwards ticks.
 */
#include "sensor_drivers.h"

#ifdef BOARD_CLUE

#include <string.h>
#include "../pinRegistry.h"
#include "../timebase.h"

SensorDrivers::SensorDrivers()
	: m_inited(false)
{
}

void SensorDrivers::setListener(BoardModule *bm)
{
	m_imu.setListener(bm);
	m_mag.setListener(bm);
	m_apds.setListener(bm);
}

bool SensorDrivers::init(uint64_t now_us, uint32_t group_lease)
{
	if (m_inited) return true;

	/* group_lease: pinreg token from main.cpp's PINREG_GROUP_TWIM1
	 * acquisition. T21 wires the real acquire; T20 just needs the
	 * drivers constructed so they compile. The lease is currently
	 * unused — i2cbus_init stored it for future cancellation. */
	(void)group_lease;

	/* Begin each present sensor (probe was done by i2cbus_probe_all). */
	m_imu.begin(now_us);
	m_mag.begin(now_us);
	m_bmp.begin(now_us);
	m_sht.begin(now_us);
	m_apds.begin(now_us);

	m_inited = true;
	return true;
}

void SensorDrivers::tick(uint64_t now_us)
{
	if (!m_inited) return;

	/* Drive each sensor's FSM. They enqueue transfers as needed; the
	 * i2cbus_tick() call below drains the queue and fires completions
	 * (which in turn advance each sensor's state on the next tick). */
	m_imu.tick(now_us);
	m_mag.tick(now_us);
	m_bmp.tick(now_us);
	m_sht.tick(now_us);
	m_apds.tick(now_us);

	/* Pump the bus manager so queued transfers start and complete. */
	i2cbus_tick();
}

bool SensorDrivers::isAnyPresent() const
{
	return m_imu.isPresent()  || m_mag.isPresent()  ||
	       m_bmp.isPresent()  || m_sht.isPresent()  ||
	       m_apds.isPresent();
}

#endif /* BOARD_CLUE */
