/*
 * sensor_drivers.h - firmware I2C sensor wrappers for the 5 CLUE onboard
 * sensors, plus the SensorDrivers aggregator.
 *
 * Each wrapper follows the eval/C++ split idiom used across the firmware:
 * the pure-logic parsers in sensors/{imu,mag,bmp280,sht31d,apds9960}.cpp
 * are host-testable and SDK-free; the C++ wrappers here own the async
 * i2cBus transfers, drive the per-sensor state machine, parse via the
 * pure-C functions, and inject results into BoardModule (motion sensors)
 * or cache them for handleReadSensor (environment / color / proximity).
 *
 * Lifecycle:
 *   1. setListener(boardModule)        - wire the inject target (T21)
 *   2. begin(now_us)                   - configure sensor registers async
 *   3. tick(now_us)                    - drive the burst/poll FSM
 *
 * Completion callbacks run from i2cbus_tick() in thread context (not ISR).
 * Wrappers are sized so each fits comfortably under the 250-LOC ceiling.
 *
 * Firmware-only: the entire header is inside #ifdef BOARD_CLUE.
 */
#ifndef SENSORS_SENSOR_DRIVERS_H
#define SENSORS_SENSOR_DRIVERS_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus

#include "imu.h"
#include "mag.h"
#include "bmp280.h"
#include "sht31d.h"
#include "apds9960.h"
#include "../i2cBus.h"

#ifdef BOARD_CLUE

class BoardModule;  /* forward decl — full def in boardModule.h */

/* ---- ImuDriver (LSM6DS33 / LSM6DS3TR-C) ------------------------------
 * Sensors 1 (acceleration mg) + 2 (gyroscope mdps). 104 Hz burst.
 * Calls BoardModule::injectImu on each completed burst. */
class ImuDriver {
public:
	ImuDriver();
	void setListener(BoardModule *bm) { m_bm = bm; }
	void begin(uint64_t now_us);
	void tick(uint64_t now_us);
	bool isPresent() const { return m_present; }
private:
	enum State { IDLE, CONFIGURING, ACTIVE };
	static void s_completion(i2cbus_result_t r, void *user);
	void onComplete(i2cbus_result_t r);
	BoardModule *m_bm;
	State    m_state;
	uint8_t  m_configStep;
	bool     m_active;
	bool     m_present;
	bool     m_fresh;
	uint64_t m_nextUs;
	uint8_t  m_buf[IMU_BURST_LEN];
	imu_sample_t m_last;
};

/* ---- MagDriver (LIS3MDL) ---------------------------------------------
 * Sensor 3 (magnetic milligauss). 40 Hz burst, field-health checked.
 * Calls BoardModule::injectMag on each completed burst. */
class MagDriver {
public:
	MagDriver();
	void setListener(BoardModule *bm) { m_bm = bm; }
	void begin(uint64_t now_us);
	void tick(uint64_t now_us);
	bool isPresent() const { return m_present; }
private:
	enum State { IDLE, CONFIGURING, ACTIVE };
	static void s_completion(i2cbus_result_t r, void *user);
	void onComplete(i2cbus_result_t r);
	BoardModule *m_bm;
	State    m_state;
	uint8_t  m_configStep;
	bool     m_active;
	bool     m_present;
	bool     m_fresh;
	uint64_t m_nextUs;
	uint8_t  m_buf[MAG_BURST_LEN];
	mag_sample_t m_last;
};

/* ---- Bmp280Driver (BMP280) -------------------------------------------
 * Sensors 6 (pressure Pa) + 7 (temperature milli-degC).
 * Async FSM: read 24-byte calibration -> write ctrl -> periodic forced
 * conversions. Cached for handleReadSensor. */
class Bmp280Driver {
public:
	Bmp280Driver();
	void begin(uint64_t now_us);
	void tick(uint64_t now_us);
	bool isPresent() const { return m_present; }
	bool getLatest(bmp280_sample_t *out) const;
private:
	enum State { IDLE, READING_CALIB, WRITING_CTRL, READING_DATA };
	static void s_completion(i2cbus_result_t r, void *user);
	void onComplete(i2cbus_result_t r);
	State          m_state;
	bool           m_present;
	bool           m_fresh;
	bool           m_active;
	uint8_t        m_configStep;
	uint64_t       m_nextUs;
	uint8_t        m_calibRaw[BMP280_CALIB_LEN];
	uint8_t        m_dataBuf[BMP280_BURST_LEN];
	bmp280_calib_t m_calib;
	int32_t        m_tFine;
	bmp280_sample_t m_last;
};

/* ---- Sht31dDriver (SHT31-D) ------------------------------------------
 * Sensors 8 (humidity milli-%RH) + 9 (temperature milli-degC).
 * Single-shot medium repeatability; write cmd -> convert -> read 6 bytes.
 * Cached for handleReadSensor. */
class Sht31dDriver {
public:
	Sht31dDriver();
	void begin(uint64_t now_us);
	void tick(uint64_t now_us);
	bool isPresent() const { return m_present; }
	bool getLatest(sht31d_sample_t *out) const;
private:
	enum State { IDLE, WRITING_CMD, CONVERTING, READING };
	static void s_completion(i2cbus_result_t r, void *user);
	void onComplete(i2cbus_result_t r);
	State         m_state;
	bool          m_present;
	bool          m_fresh;
	bool          m_active;
	uint64_t      m_nextUs;
	uint64_t      m_convertDeadline;
	uint8_t       m_resp[SHT31D_RESP_LEN];
	sht31d_sample_t m_last;
};

/* ---- Apds9960Driver (APDS-9960) --------------------------------------
 * Sensors 10 (color counts) + 11 (proximity) + 12 (gesture enum).
 * Optical mode by default (RGBC + proximity). Gesture mode entered when
 * the motion subsystem requests it; gesture FIFO decode is fed back via
 * BoardModule::injectApdsGesture. */
class Apds9960Driver {
public:
	Apds9960Driver();
	void setListener(BoardModule *bm) { m_bm = bm; }
	void begin(uint64_t now_us);
	void tick(uint64_t now_us);
	bool isPresent() const { return m_present; }
	bool getLatestOptical(apds9960_optical_sample_t *out) const;
private:
	enum State { IDLE, CONFIGURING, OPTICAL_BURST, READING_PROX };
	static void s_completion(i2cbus_result_t r, void *user);
	void onComplete(i2cbus_result_t r);
	BoardModule *m_bm;
	State         m_state;
	uint8_t       m_configStep;
	bool          m_active;
	bool          m_present;
	bool          m_fresh;
	uint64_t      m_nextUs;
	uint8_t       m_rgbcBuf[APDS9960_RGBC_LEN];
	uint8_t       m_proxVal;
	apds9960_optical_sample_t m_last;
};

/* ---- SensorDrivers aggregator --------------------------------------- */
/* Owns the 5 wrappers + drives i2cbus_tick. Constructed by T21 inside
 * BoardModule; init() acquires the TWIM1 pin group and configures each
 * present sensor. tick(now_us) advances each wrapper and the bus. */
class SensorDrivers {
public:
	SensorDrivers();
	void setListener(BoardModule *bm);
	/* Acquire TWIM1 group lease + begin() each present sensor.
	 * Returns false if the pin group cannot be acquired. */
	bool init(uint64_t now_us, uint32_t group_lease);
	void tick(uint64_t now_us);
	bool isAnyPresent() const;

	ImuDriver       &imu()       { return m_imu; }
	MagDriver       &mag()       { return m_mag; }
	Bmp280Driver    &bmp280()    { return m_bmp; }
	Sht31dDriver    &sht31d()    { return m_sht; }
	Apds9960Driver  &apds9960()  { return m_apds; }
private:
	ImuDriver      m_imu;
	MagDriver      m_mag;
	Bmp280Driver   m_bmp;
	Sht31dDriver   m_sht;
	Apds9960Driver m_apds;
	bool           m_inited;
};

#endif /* BOARD_CLUE */
#endif /* __cplusplus */
#endif /* SENSORS_SENSOR_DRIVERS_H */
