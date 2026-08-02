/*
 * sht31d.h - SHT31-D humidity/temperature sensor pure-logic driver.
 *
 * Register constants, CRC-8 computation, integer conversion math, and
 * async state machine types.  No SDK deps — host-testable.
 *
 * The firmware C++ class (sht31d.cpp) wraps these with i2cBus async
 * transfers.  The host test links the .cpp directly.
 *
 * Sensor at I2C 0x44 (from i2cBus.h frozen table).  SHT31-D has NO
 * WHO_AM_I register — presence is confirmed by issuing a measurement
 * command and verifying CRC-valid response (handled by i2cBus probe).
 *
 * Output units (board_manifest.json):
 *   Temperature: milli-degC (ID 9)    Humidity: milli-%RH (ID 8)
 *
 * Default: single-shot medium repeatability, clock-stretching disabled.
 * Conversion max 6.5ms — NOT a bus timeout, bus is free during conversion.
 */
#ifndef SENSORS_SHT31D_H
#define SENSORS_SHT31D_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- I2C address (from i2cBus.h frozen table) ------------------------ */

#define SHT31D_ADDR                 0x44u

/* ---- Measurement commands (clock stretching disabled) ---------------- */
/*
 * Bits [15:8] = MSB, [7:0] = LSB.
 * MSB 0x24 = no clock stretching.
 * LSB bits [7:5] = repeatability: 000=high, 01=medium, 10=low.
 *                   (medium: 0x0B = 010_11, high: 0x00 = 000_00, low: 0x16)
 */
#define SHT31D_CMD_SINGLE_HIGH      0x2400u
#define SHT31D_CMD_SINGLE_MED       0x240Bu
#define SHT31D_CMD_SINGLE_LOW       0x2416u

/* ---- Conversion timing (microseconds, max from datasheet) ------------ */

#define SHT31D_CONV_HIGH_US         15500u
#define SHT31D_CONV_MED_US          6500u
#define SHT31D_CONV_LOW_US          2500u

/* ---- Data layout (6 bytes: T_msb T_lsb T_crc RH_msb RH_lsb RH_crc) -- */

#define SHT31D_RESP_LEN             6u
#define SHT31D_TEMP_OFFSET          0u
#define SHT31D_CRC_OFFSET           2u
#define SHT31D_HUM_OFFSET           3u

/* ---- CRC-8: polynomial 0x31, init 0xFF, no reflection ---------------- */

#define SHT31D_CRC_POLY             0x31u
#define SHT31D_CRC_INIT             0xFFu

uint8_t sht31d_crc(const uint8_t *data, size_t len);

/* ---- Sensor IDs (board_manifest.json) -------------------------------- */

#define SHT31D_SENSOR_ID_TEMP       9u
#define SHT31D_SENSOR_ID_HUM        8u

/* ---- Status / result codes ------------------------------------------- */

typedef enum {
	SHT31D_OK              = 0,
	SHT31D_ERR_CRC         = 1,
	SHT31D_ERR_RANGE       = 2,
	SHT31D_ERR_NOT_READY   = 3,
} sht31d_result_t;

/* ---- Raw to engineering units (integer milli-units) ------------------ */

typedef struct {
	int32_t  temp_milli_c;
	uint32_t humidity_milli_rh;
	sht31d_result_t status;
} sht31d_sample_t;

/*
 * Convert raw 16-bit temperature and humidity to milli-degC and milli-%RH.
 *
 * Temperature: T = -45 + 175 * raw/65535 (degC)
 *              → milli: -45000 + 175000 * raw / 65535
 * Humidity:    RH = 100 * raw/65535 (%RH)
 *              → milli: 100000 * raw / 65535
 *              Clamped to [0, 100000].
 *
 * Returns SHT31D_ERR_CRC if either CRC byte does not match.
 */
sht31d_result_t sht31d_convert(const uint8_t resp[SHT31D_RESP_LEN],
                               sht31d_sample_t *out);

/* ---- Async state machine (firmware wrapper uses this) ---------------- */

typedef enum {
	SHT31D_STATE_IDLE        = 0,
	SHT31D_STATE_WRITE_CMD   = 1,
	SHT31D_STATE_CONVERTING  = 2,
	SHT31D_STATE_READING     = 3,
	SHT31D_STATE_DONE        = 4,
} sht31d_state_t;

/* ---- Default configuration ------------------------------------------- */

#define SHT31D_DEFAULT_CMD          SHT31D_CMD_SINGLE_MED
#define SHT31D_DEFAULT_CONV_US      SHT31D_CONV_MED_US
#define SHT31D_DEFAULT_RATE_HZ      1u

/* ---- Clear status register command (optional, for heater/reset) ------ */

#define SHT31D_CMD_SOFT_RESET       0x30A2u
#define SHT31D_CMD_CLEAR_STATUS     0x3041u
#define SHT31D_CMD_READ_STATUS      0xF32Du

#ifdef __cplusplus
}
#endif
#endif /* SENSORS_SHT31D_H */
