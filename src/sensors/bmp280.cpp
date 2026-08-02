/*
 * bmp280.cpp - BMP280 pure-logic implementation.
 *
 * No SDK dependencies.  All functions are host-testable.  Compensation
 * uses the Bosch fixed-point algorithm from bmp280_comp.h.
 */
#include "bmp280.h"

/* ---- Compile-time assertions ----------------------------------------- */

static_assert(BMP280_ADDR == 0x77u, "BMP280 address must be 0x77");
static_assert(BMP280_ID_VALUE == 0x58u, "BMP280 ID must be 0x58");
static_assert(BMP280_CALIB_LEN == 24u, "calibration must be 24 bytes");
static_assert(BMP280_BURST_LEN == 6u, "burst must be 6 bytes");
static_assert(BMP280_CTRL_MEAS_FORCED_DEFAULT == 0x4Du, "forced default ctrl");
static_assert(BMP280_CTRL_MEAS_NORMAL_DEFAULT == 0x4Fu, "normal default ctrl");
static_assert(BMP280_CONFIG_NORMAL_DEFAULT == 0x28u, "normal default config");
static_assert(BMP280_CONV_FORCED_DEFAULT_US == 16300u, "forced conv bound");

/* ---- Little-endian helpers ------------------------------------------- */

static uint16_t le16u(const uint8_t *p) {
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int16_t le16s(const uint8_t *p) {
	return (int16_t)le16u(p);
}

/* ---- Calibration parsing --------------------------------------------- */

void bmp280_parse_calib(const uint8_t raw[BMP280_CALIB_LEN],
                        bmp280_calib_t *cal)
{
	cal->dig_T1 = le16u(raw + 0);   /* unsigned */
	cal->dig_T2 = le16s(raw + 2);   /* signed   */
	cal->dig_T3 = le16s(raw + 4);   /* signed   */
	cal->dig_P1 = le16u(raw + 6);   /* unsigned */
	cal->dig_P2 = le16s(raw + 8);   /* signed   */
	cal->dig_P3 = le16s(raw + 10);  /* signed   */
	cal->dig_P4 = le16s(raw + 12);  /* signed   */
	cal->dig_P5 = le16s(raw + 14);  /* signed   */
	cal->dig_P6 = le16s(raw + 16);  /* signed   */
	cal->dig_P7 = le16s(raw + 18);  /* signed   */
	cal->dig_P8 = le16s(raw + 20);  /* signed   */
	cal->dig_P9 = le16s(raw + 22);  /* signed   */
}

/* ---- Raw data parsing (20-bit left-justified) ------------------------ */

int32_t bmp280_raw_pressure(const uint8_t burst[BMP280_BURST_LEN])
{
	return ((int32_t)burst[0] << 12) | ((int32_t)burst[1] << 4)
	       | ((int32_t)burst[2] >> 4);
}

int32_t bmp280_raw_temperature(const uint8_t burst[BMP280_BURST_LEN])
{
	return ((int32_t)burst[3] << 12) | ((int32_t)burst[4] << 4)
	       | ((int32_t)burst[5] >> 4);
}

/* ---- Full compensation ----------------------------------------------- */

void bmp280_compensate(const uint8_t burst[BMP280_BURST_LEN],
                       const bmp280_calib_t *cal,
                       int32_t *t_fine_storage,
                       bmp280_sample_t *out)
{
	int32_t adc_T = bmp280_raw_temperature(burst);
	int32_t adc_P = bmp280_raw_pressure(burst);

	int32_t t_fine = 0;
	int32_t T_centi = bmp280_compensate_T_int32(adc_T, cal, &t_fine);

	if (t_fine_storage)
		*t_fine_storage = t_fine;

	uint32_t P_q248 = bmp280_compensate_P_int64(adc_P, t_fine, cal);

	out->temp_milli_c = bmp280_centi_to_milli(T_centi);
	out->pressure_pa = bmp280_q248_to_pa(P_q248);
}

/* ---- Altitude from sea-level pressure -------------------------------- */
/*
 * Barometric formula:  h = 44330 * (1 - (P/P0)^(1/5.255))  meters
 *
 * Integer fixed-point implementation. The fractional exponent is
 * evaluated via a 5-term Taylor expansion of f(x) = x^(1/5.255)
 * around x=1:
 *
 *   f(1+t) = c0 + c1*t + c2*t^2 + c3*t^3 + c4*t^4 + c5*t^5
 *
 *   c0 = 1
 *   c1 =  0.19029497
 *   c2 = -0.07704150
 *   c3 =  0.04647467
 *   c4 = -0.03264262
 *   c5 =  0.02487306
 *
 * For typical atmospheric pressure ratios (P/P0 ∈ [0.5, 1.5], i.e.
 * -5500m..+5500m), five terms give <0.1% relative error.
 *
 * All arithmetic is in Q32 fixed-point (1.0 = 2^32) using int64_t
 * intermediates. nRF52840 (Cortex-M4F) handles 64-bit multiply in
 * ~5 cycles.
 */
int32_t bmp280_altitude_meters(uint32_t pressure_pa, uint32_t sea_level_pa)
{
	if (sea_level_pa == 0u || pressure_pa == 0u)
		return 0;

	/* x = P/P0 in Q32, then t = x - 1 in Q32. */
	uint64_t x_q32 = ((uint64_t)pressure_pa << 32) / sea_level_pa;
	int64_t  t_q32 = (int64_t)x_q32 - ((int64_t)1LL << 32);

	/* Q32 polynomial coefficients for x^(1/5.255) around x=1
	 * (compile-time float→int conversion; no runtime FP needed). */
	const int64_t C5 = (int64_t)(0.02487306 * (double)(1LL << 32));
	const int64_t C4 = (int64_t)(-0.03264262 * (double)(1LL << 32));
	const int64_t C3 = (int64_t)(0.04647467 * (double)(1LL << 32));
	const int64_t C2 = (int64_t)(-0.07704150 * (double)(1LL << 32));
	const int64_t C1 = (int64_t)(0.19029497 * (double)(1LL << 32));

	/* Horner evaluation: y = (((((c5*t + c4)*t + c3)*t + c2)*t + c1)*t + 1)
	 * Each step keeps the result in Q32. */
	int64_t y = C5;
	y = ((t_q32 * y) >> 32) + C4;
	y = ((t_q32 * y) >> 32) + C3;
	y = ((t_q32 * y) >> 32) + C2;
	y = ((t_q32 * y) >> 32) + C1;
	y = ((t_q32 * y) >> 32) + (1LL << 32);  /* + c0 = 1.0 */

	/* h = 44330 * (1 - y) meters, with diff in Q32.
	 * 44330 * 2^32 ≈ 1.9e14, fits in int64_t (max 9.2e18). */
	int64_t diff_q32 = (1LL << 32) - y;
	return (int32_t)((44330LL * diff_q32) >> 32);
}

/* ---- Status register decode ------------------------------------------ */

void bmp280_parse_status(uint8_t status_reg, bmp280_status_t *out)
{
	out->measuring = (status_reg & BMP280_STATUS_MEASURING) != 0;
	out->nvm_copying = (status_reg & BMP280_STATUS_IM_UPDATE) != 0;
}
