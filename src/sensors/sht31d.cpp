/*
 * sht31d.cpp - SHT31-D pure-logic implementation.
 *
 * No SDK dependencies.  All functions are host-testable.  The firmware
 * wrapper layer issues async i2cBus transfers and calls these functions
 * to validate CRC and convert raw ADC to engineering units.
 */
#include "sht31d.h"

/* ---- Compile-time register assertions -------------------------------- */

static_assert(SHT31D_ADDR == 0x44u, "SHT31-D address must be 0x44");
static_assert(SHT31D_RESP_LEN == 6u, "response must be 6 bytes");
static_assert(SHT31D_CMD_SINGLE_MED == 0x240Bu, "medium repeatability cmd");
static_assert(SHT31D_CONV_MED_US == 6500u, "medium conversion 6.5ms");

/* ---- CRC-8 (polynomial 0x31, init 0xFF, no reflection) --------------- */

uint8_t sht31d_crc(const uint8_t *data, size_t len) {
	uint8_t crc = SHT31D_CRC_INIT;
	for (size_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int bit = 0; bit < 8; bit++) {
			if (crc & 0x80u)
				crc = (uint8_t)((crc << 1) ^ SHT31D_CRC_POLY);
			else
				crc = (uint8_t)(crc << 1);
		}
	}
	return crc;
}

/* ---- Raw to engineering units ---------------------------------------- */

sht31d_result_t sht31d_convert(const uint8_t resp[SHT31D_RESP_LEN],
                               sht31d_sample_t *out)
{
	uint8_t crc_t = sht31d_crc(resp, 2);
	if (crc_t != resp[SHT31D_CRC_OFFSET])
		return SHT31D_ERR_CRC;

	uint8_t crc_h = sht31d_crc(resp + SHT31D_HUM_OFFSET, 2);
	if (crc_h != resp[SHT31D_CRC_OFFSET + 3])
		return SHT31D_ERR_CRC;

	uint16_t raw_t = (uint16_t)((uint16_t)resp[0] << 8 | resp[1]);
	uint16_t raw_h = (uint16_t)((uint16_t)resp[3] << 8 | resp[4]);

	/* Temperature: milli-degC = -45000 + 175000 * raw / 65535 */
	out->temp_milli_c = (int32_t)(-45000 + (int64_t)175000 * raw_t / 65535);

	/* Humidity: milli-%RH = 100000 * raw / 65535, clamped [0, 100000] */
	int64_t rh = (int64_t)100000 * raw_h / 65535;
	if (rh < 0) rh = 0;
	if (rh > 100000) rh = 100000;
	out->humidity_milli_rh = (uint32_t)rh;

	out->status = SHT31D_OK;
	return SHT31D_OK;
}
