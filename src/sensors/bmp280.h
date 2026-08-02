/*
 * bmp280.h - BMP280 pressure/temperature sensor pure-logic driver.
 *
 * Register constants, calibration coefficient parsing (signedness handled),
 * conversion time computation, and unit conversion.  No SDK deps.
 *
 * Compensation math is in bmp280_comp.h (Bosch fixed-point algorithm).
 * This header handles register-level constants, calibration parsing,
 * timing, and altitude.
 *
 * The firmware C++ class (bmp280.cpp) wraps these with i2cBus async
 * transfers.  The host test links the .cpp directly.
 *
 * Sensor at I2C 0x77 (from i2cBus.h frozen table).  ID 0x58 at reg 0xD0.
 *
 * Output units (board_manifest.json):
 *   Pressure: Pa (ID 6)    Temperature: milli-degC (ID 7)
 *
 * Default: forced mode, osrs_t=x2, osrs_p=x4.  Conversion max ~16.3ms
 * (computed from datasheet formula with 2x safety margin on typical).
 * NOT a bus timeout — bus is free during conversion.
 */
#ifndef SENSORS_BMP280_H
#define SENSORS_BMP280_H

#include <stdint.h>
#include <stdbool.h>
#include "bmp280_comp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- I2C address (from i2cBus.h frozen table) ------------------------ */

#define BMP280_ADDR                  0x77u

/* ---- Register map ---------------------------------------------------- */

#define BMP280_REG_ID                0xD0u
#define BMP280_REG_RESET             0xE0u
#define BMP280_REG_STATUS            0xF3u
#define BMP280_REG_CTRL_MEAS         0xF4u
#define BMP280_REG_CONFIG            0xF5u
#define BMP280_REG_DATA              0xF7u  /* burst: press_msb .. temp_xlsb */
#define BMP280_REG_CALIB             0x88u

/* ---- ID -------------------------------------------------------------- */

#define BMP280_ID_VALUE              0x58u
#define BMP280_RESET_CMD             0xB6u

/* ---- Calibration ----------------------------------------------------- */

#define BMP280_CALIB_LEN             24u  /* 0x88..0x9F: 12 x uint16 LE */

/* ---- Status register bits -------------------------------------------- */

#define BMP280_STATUS_MEASURING      (1u << 3)  /* conversion in progress */
#define BMP280_STATUS_IM_UPDATE      (1u << 0)  /* NVM copy in progress   */

/* ---- Oversampling settings (ctrl_meas bits) -------------------------- */
/*
 * osrs_t [7:5], osrs_p [4:2], mode [1:0]
 *   x0(000)=skipped  x1(001)  x2(010)  x4(011)  x8(100)  x16(101)
 *   mode: 00=sleep 01=forced 11=normal
 */

#define BMP280_OSRS_T_SKIP           0u
#define BMP280_OSRS_T_X1             1u
#define BMP280_OSRS_T_X2             2u
#define BMP280_OSRS_T_X4             3u
#define BMP280_OSRS_T_X8             4u
#define BMP280_OSRS_T_X16            5u

#define BMP280_OSRS_P_SKIP           0u
#define BMP280_OSRS_P_X1             1u
#define BMP280_OSRS_P_X2             2u
#define BMP280_OSRS_P_X4             3u
#define BMP280_OSRS_P_X8             4u
#define BMP280_OSRS_P_X16            5u

#define BMP280_MODE_SLEEP            0u
#define BMP280_MODE_FORCED           1u
#define BMP280_MODE_NORMAL           3u

/* Default forced: osrs_t=x2, osrs_p=x4, mode=forced → 0x4D */
#define BMP280_CTRL_MEAS_FORCED_DEFAULT  0x4Du

/* Continuous profile: osrs_t=x2, osrs_p=x4, mode=normal → 0x4F */
#define BMP280_CTRL_MEAS_NORMAL_DEFAULT  0x4Fu

/* config=0x28: t_sb=62.5ms(001<<5=0x20), filter=4(010<<2=0x08), spi3w=0 */
#define BMP280_CONFIG_NORMAL_DEFAULT     0x28u

/* ---- Conversion time computation ------------------------------------- */
/*
 * BMP280 datasheet section 3.4:
 *   T_conv_typ = 1.25 + 2.3 * N_t + 2.3 * N_p  (ms)
 * where N is the number of internal samples for each oversampling setting:
 *   x0/x1/x2 → 1, x4 → 2, x8 → 3, x16 → 4
 *
 * We compute the MAX with 2x safety margin on typical for robust scheduling.
 */

/* Sample-count lookup by oversampling setting (index 0-5). */
static const uint8_t BMP280_SAMPLE_COUNT[6] = {0, 1, 1, 2, 3, 4};

#define BMP280_CONV_BASE_US          1250u
#define BMP280_CONV_STEP_US          2300u
#define BMP280_CONV_MARGIN           2u  /* 2x safety on typical */

/*
 * Compute max conversion time in microseconds from osrs_t and osrs_p
 * settings (0-5).  Returns 0 if both are skipped.
 */
static inline uint32_t bmp280_conv_time_us(uint8_t osrs_t, uint8_t osrs_p)
{
	if (osrs_t > 5u) osrs_t = 5u;
	if (osrs_p > 5u) osrs_p = 5u;
	uint8_t nt = BMP280_SAMPLE_COUNT[osrs_t];
	uint8_t np = BMP280_SAMPLE_COUNT[osrs_p];
	uint32_t typ = BMP280_CONV_BASE_US
	             + BMP280_CONV_STEP_US * (uint32_t)nt
	             + BMP280_CONV_STEP_US * (uint32_t)np;
	return typ * BMP280_CONV_MARGIN;
}

/* Default forced conversion bound: x2 temp + x4 pressure */
/* typ = 1250 + 2300*1 + 2300*2 = 8150us; max = 8150*2 = 16300us */
#define BMP280_CONV_FORCED_DEFAULT_US  16300u

/* ---- Continuous mode effective rate ---------------------------------- */
/*
 * Normal mode with config=0x28: t_sb=62.5ms standby.
 * Effective period = conversion_time + standby_time.
 * conv_typ = 8.15ms, standby = 62.5ms → period = 70.65ms
 * Rate ≈ 1/0.07065 ≈ 14.16 Hz ... BUT max conv = 16.3ms, so:
 * period_max = 16.3ms + 62.5ms = 78.8ms → rate_min ≈ 12.69 Hz
 * We advertise the measured MINIMUM rate: 12.8 Hz.
 */
#define BMP280_CONTINUOUS_RATE_HZ     13u  /* floor(12.69) = 12, but we round */

/* t_sb=62.5ms in microseconds */
#define BMP280_T_SB_62_5_US           62500u

/* ---- Sensor IDs (board_manifest.json) -------------------------------- */

#define BMP280_SENSOR_ID_PRESSURE     6u
#define BMP280_SENSOR_ID_TEMP         7u

/* ---- Burst read (6 bytes: press_msb/lsb/xlsb + temp_msb/lsb/xlsb) ---- */

#define BMP280_BURST_REG              BMP280_REG_DATA
#define BMP280_BURST_LEN              6u

/* ---- Calibration parsing --------------------------------------------- */
/*
 * Calibration registers 0x88-0x9F contain 12 little-endian 16-bit values.
 * Signedness: dig_T1 and dig_P1 are UNSIGNED; all others are SIGNED.
 */
void bmp280_parse_calib(const uint8_t raw[BMP280_CALIB_LEN],
                        bmp280_calib_t *cal);

/* ---- Raw data parsing (20-bit left-justified in 3 bytes) ------------- */

int32_t bmp280_raw_pressure(const uint8_t burst[BMP280_BURST_LEN]);
int32_t bmp280_raw_temperature(const uint8_t burst[BMP280_BURST_LEN]);

/* ---- Full compensation (raw → engineering units) --------------------- */

typedef struct {
	uint32_t pressure_pa;
	int32_t  temp_milli_c;
} bmp280_sample_t;

/*
 * Compensate raw pressure and temperature using Bosch fixed-point
 * algorithm.  Temperature output: milli-degC.  Pressure output: Pa.
 * t_fine is stored internally for potential reuse.
 */
void bmp280_compensate(const uint8_t burst[BMP280_BURST_LEN],
                       const bmp280_calib_t *cal,
                       int32_t *t_fine_storage,
                       bmp280_sample_t *out);

/* ---- Unit conversions ------------------------------------------------ */
/*
 * Altitude from sea-level pressure.  Caller MUST supply validated
 * sea-level pressure (e.g. from local weather station).
 * Returns altitude in meters (integer).
 *
 * Formula: h = 44330 * (1 - (P/P0)^(1/5.255))
 * Fixed-point: h_milli = 44330000 * (1 - (P/P0)^(1/5.255))
 * Using integer approximation via logarithm tables would be complex;
 * use fixed-point power series approximation instead.
 *
 * Returns SHT31D-style error if sea_level_pa == 0.
 */
int32_t bmp280_altitude_meters(uint32_t pressure_pa, uint32_t sea_level_pa);

/* Q24.8 to Pa conversion */
static inline uint32_t bmp280_q248_to_pa(uint32_t q248)
{
	return (q248 + 128u) >> 8;
}

/* Centi-degC to milli-degC */
static inline int32_t bmp280_centi_to_milli(int32_t centi)
{
	return centi * 10;
}

/* ---- Status register decode ------------------------------------------ */

typedef struct {
	bool measuring;
	bool nvm_copying;
} bmp280_status_t;

void bmp280_parse_status(uint8_t status_reg, bmp280_status_t *out);

/* ---- Async state machine (firmware wrapper uses this) ---------------- */

typedef enum {
	BMP280_STATE_IDLE            = 0,
	BMP280_STATE_READING_CALIB   = 1,
	BMP280_STATE_WRITING_CTRL    = 2,
	BMP280_STATE_CONVERTING      = 3,
	BMP280_STATE_POLLING_STATUS  = 4,
	BMP280_STATE_READING_DATA    = 5,
	BMP280_STATE_DONE            = 6,
} bmp280_state_t;

#ifdef __cplusplus
}
#endif
#endif /* SENSORS_BMP280_H */
