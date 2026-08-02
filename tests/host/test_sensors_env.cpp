/*
 * test_sensors_env.cpp - host tests for SHT31-D and BMP280 environment sensors.
 *
 * Covers:
 *   SHT31-D: CRC-8 known vectors, conversion math (temp/humidity), CRC
 *            rejection, conversion timing constants, command constants,
 *            clamping.
 *   BMP280:  ID register, calibration parsing signedness, raw data parsing,
 *            golden vector compensation (T and P), conversion timing
 *            computation, continuous mode rate, altitude calculation,
 *            divide-by-zero protection, status register decode.
 *
 * No SDK deps, no hardware. Includes driver .cpp files directly.
 */
#include "test_framework.h"
#include "../../src/sensors/sht31d.cpp"
#include "../../src/sensors/bmp280.cpp"
#include <string.h>

/* ====================================================================== */
/*  SHT31-D: register constants                                           */
/* ====================================================================== */

static void test_sht31d_constants(void) {
	TEST_ASSERT_EQ_INT(0x44, (int)SHT31D_ADDR);
	TEST_ASSERT_EQ_INT(0x240B, (int)SHT31D_CMD_SINGLE_MED);
	TEST_ASSERT_EQ_INT(0x2400, (int)SHT31D_CMD_SINGLE_HIGH);
	TEST_ASSERT_EQ_INT(0x2416, (int)SHT31D_CMD_SINGLE_LOW);
	TEST_ASSERT_EQ_INT(6, (int)SHT31D_RESP_LEN);
	TEST_ASSERT_EQ_INT(9, (int)SHT31D_SENSOR_ID_TEMP);
	TEST_ASSERT_EQ_INT(8, (int)SHT31D_SENSOR_ID_HUM);
}

static void test_sht31d_conv_timing(void) {
	TEST_ASSERT_EQ_INT(6500, (int)SHT31D_CONV_MED_US);
	TEST_ASSERT_EQ_INT(15500, (int)SHT31D_CONV_HIGH_US);
	TEST_ASSERT_EQ_INT(2500, (int)SHT31D_CONV_LOW_US);
}

/* ====================================================================== */
/*  SHT31-D: CRC-8                                                        */
/* ====================================================================== */

static void test_sht31d_crc_known_vector(void) {
	uint8_t data[2] = {0xBE, 0xEF};
	TEST_ASSERT_EQ_INT(0x92, (int)sht31d_crc(data, 2));
}

static void test_sht31d_crc_another_vector(void) {
	/* CRC-8 with poly 0x31 init 0xFF: known vector from Sensirion SHT3x datasheet */
	uint8_t data[2] = {0x00, 0x00};
	uint8_t crc = sht31d_crc(data, 2);
	TEST_ASSERT_EQ_INT(0x81, (int)crc);
}

static void test_sht31d_crc_single_byte(void) {
	uint8_t data[1] = {0x00};
	uint8_t crc = sht31d_crc(data, 1);
	TEST_ASSERT_EQ_INT(0xAC, (int)crc);
}

static void test_sht31d_crc_empty(void) {
	uint8_t crc = sht31d_crc((const uint8_t *)"", 0);
	TEST_ASSERT_EQ_INT(0xFF, (int)crc);
}

/* ====================================================================== */
/*  SHT31-D: conversion math                                              */
/* ====================================================================== */

static void test_sht31d_convert_valid(void) {
	uint8_t resp[6] = {0xBE, 0xEF, 0, 0x7B, 0x58, 0};
	resp[2] = sht31d_crc(resp, 2);
	resp[5] = sht31d_crc(resp + 3, 2);

	sht31d_sample_t s;
	sht31d_result_t r = sht31d_convert(resp, &s);
	TEST_ASSERT_EQ_INT((int)SHT31D_OK, (int)r);

	/* raw_t = 0xBEEF = 48879
	 * milli: -45000 + 175000*48879/65535 = -45000 + 130523 = 85523 */
	TEST_ASSERT_EQ_INT(85523, (int)s.temp_milli_c);

	/* raw_h = 0x7B58 = 31576 → milli: 100000*31576/65535 = 48181 */
	TEST_ASSERT_EQ_INT(48181, (int)s.humidity_milli_rh);
}

static void test_sht31d_convert_zero_raw(void) {
	uint8_t resp[6] = {0, 0, 0, 0, 0, 0};
	resp[2] = sht31d_crc(resp, 2);
	resp[5] = sht31d_crc(resp + 3, 2);

	sht31d_sample_t s;
	sht31d_result_t r = sht31d_convert(resp, &s);
	TEST_ASSERT_EQ_INT((int)SHT31D_OK, (int)r);

	/* T = -45000 + 0 = -45000 milli-C = -45.0C */
	TEST_ASSERT_EQ_INT(-45000, (int)s.temp_milli_c);
	/* RH = 0 */
	TEST_ASSERT_EQ_INT(0, (int)s.humidity_milli_rh);
}

static void test_sht31d_convert_max_raw(void) {
	uint8_t resp[6] = {0xFF, 0xFF, 0, 0xFF, 0xFF, 0};
	resp[2] = sht31d_crc(resp, 2);
	resp[5] = sht31d_crc(resp + 3, 2);

	sht31d_sample_t s;
	sht31d_result_t r = sht31d_convert(resp, &s);
	TEST_ASSERT_EQ_INT((int)SHT31D_OK, (int)r);

	/* T = -45000 + 175000 = 130000 milli-C = 130.0C */
	TEST_ASSERT_EQ_INT(130000, (int)s.temp_milli_c);
	/* RH = 100000 milli-% = 100.0% */
	TEST_ASSERT_EQ_INT(100000, (int)s.humidity_milli_rh);
}

static void test_sht31d_convert_bad_crc_temp(void) {
	uint8_t resp[6] = {0xBE, 0xEF, 0xFF, 0x7B, 0x58, 0x9B};
	sht31d_sample_t s;
	sht31d_result_t r = sht31d_convert(resp, &s);
	TEST_ASSERT_EQ_INT((int)SHT31D_ERR_CRC, (int)r);
}

static void test_sht31d_convert_bad_crc_hum(void) {
	uint8_t resp[6] = {0xBE, 0xEF, 0x92, 0x7B, 0x58, 0xFF};
	sht31d_sample_t s;
	sht31d_result_t r = sht31d_convert(resp, &s);
	TEST_ASSERT_EQ_INT((int)SHT31D_ERR_CRC, (int)r);
}

/* ====================================================================== */
/*  BMP280: register constants                                            */
/* ====================================================================== */

static void test_bmp280_constants(void) {
	TEST_ASSERT_EQ_INT(0x77, (int)BMP280_ADDR);
	TEST_ASSERT_EQ_INT(0x58, (int)BMP280_ID_VALUE);
	TEST_ASSERT_EQ_INT(0xD0, (int)BMP280_REG_ID);
	TEST_ASSERT_EQ_INT(0xF4, (int)BMP280_REG_CTRL_MEAS);
	TEST_ASSERT_EQ_INT(0xF5, (int)BMP280_REG_CONFIG);
	TEST_ASSERT_EQ_INT(0xF7, (int)BMP280_REG_DATA);
	TEST_ASSERT_EQ_INT(0x88, (int)BMP280_REG_CALIB);
	TEST_ASSERT_EQ_INT(24, (int)BMP280_CALIB_LEN);
	TEST_ASSERT_EQ_INT(6, (int)BMP280_BURST_LEN);
	TEST_ASSERT_EQ_INT(0x4D, (int)BMP280_CTRL_MEAS_FORCED_DEFAULT);
	TEST_ASSERT_EQ_INT(0x4F, (int)BMP280_CTRL_MEAS_NORMAL_DEFAULT);
	TEST_ASSERT_EQ_INT(0x28, (int)BMP280_CONFIG_NORMAL_DEFAULT);
	TEST_ASSERT_EQ_INT(6, (int)BMP280_SENSOR_ID_PRESSURE);
	TEST_ASSERT_EQ_INT(7, (int)BMP280_SENSOR_ID_TEMP);
}

static void test_bmp280_status_bits(void) {
	TEST_ASSERT_EQ_INT(8, (int)BMP280_STATUS_MEASURING);
	TEST_ASSERT_EQ_INT(1, (int)BMP280_STATUS_IM_UPDATE);
}

/* ====================================================================== */
/*  BMP280: calibration parsing signedness                                */
/* ====================================================================== */

static void test_bmp280_parse_calib_signedness(void) {
	bmp280_calib_t golden = {27504, 26435, -1000,
	                         36477, -10685, 3024,
	                         2855, 132, -3475,
	                         1467, -5697, 7153};

	uint8_t raw[24];
	const uint16_t vals_u[2] = {27504, 36477};
	const int16_t vals_s[10] = {26435, -1000, -10685, 3024, 2855, 132,
	                            -3475, 1467, -5697, 7153};

	raw[0] = (uint8_t)(vals_u[0]); raw[1] = (uint8_t)(vals_u[0] >> 8);
	raw[2] = (uint8_t)(vals_s[0]); raw[3] = (uint8_t)(vals_s[0] >> 8);
	raw[4] = (uint8_t)(vals_s[1]); raw[5] = (uint8_t)(vals_s[1] >> 8);
	raw[6] = (uint8_t)(vals_u[1]); raw[7] = (uint8_t)(vals_u[1] >> 8);
	for (int i = 0; i < 8; i++) {
		raw[8 + i*2]     = (uint8_t)(vals_s[i+2]);
		raw[8 + i*2 + 1] = (uint8_t)((uint16_t)vals_s[i+2] >> 8);
	}

	bmp280_calib_t parsed;
	bmp280_parse_calib(raw, &parsed);

	TEST_ASSERT_EQ_INT((int)golden.dig_T1, (int)parsed.dig_T1);
	TEST_ASSERT_EQ_INT((int)golden.dig_T2, (int)parsed.dig_T2);
	TEST_ASSERT_EQ_INT((int)golden.dig_T3, (int)parsed.dig_T3);
	TEST_ASSERT_EQ_INT((int)golden.dig_P1, (int)parsed.dig_P1);
	TEST_ASSERT_EQ_INT((int)golden.dig_P2, (int)parsed.dig_P2);
	TEST_ASSERT_EQ_INT((int)golden.dig_P3, (int)parsed.dig_P3);
	TEST_ASSERT_EQ_INT((int)golden.dig_P4, (int)parsed.dig_P4);
	TEST_ASSERT_EQ_INT((int)golden.dig_P5, (int)parsed.dig_P5);
	TEST_ASSERT_EQ_INT((int)golden.dig_P6, (int)parsed.dig_P6);
	TEST_ASSERT_EQ_INT((int)golden.dig_P7, (int)parsed.dig_P7);
	TEST_ASSERT_EQ_INT((int)golden.dig_P8, (int)parsed.dig_P8);
	TEST_ASSERT_EQ_INT((int)golden.dig_P9, (int)parsed.dig_P9);
}

static void test_bmp280_parse_calib_negative_values(void) {
	/* All-negative signed coefficients to verify sign extension */
	uint8_t raw[24] = {0};
	/* T1 = 0 (unsigned) */
	/* T2 = -1 (signed, LE: 0xFF 0xFF) */
	raw[2] = 0xFF; raw[3] = 0xFF;
	/* T3 = -1 */
	raw[4] = 0xFF; raw[5] = 0xFF;
	/* P1 = 1 (unsigned, to avoid div-by-zero) */
	raw[6] = 0x01; raw[7] = 0x00;
	/* P2 = -32000 */
	int16_t p2 = -32000;
	raw[8] = (uint8_t)(p2 & 0xFF); raw[9] = (uint8_t)(p2 >> 8);

	bmp280_calib_t cal;
	bmp280_parse_calib(raw, &cal);

	TEST_ASSERT_EQ_INT(0, (int)cal.dig_T1);
	TEST_ASSERT_EQ_INT(-1, (int)cal.dig_T2);
	TEST_ASSERT_EQ_INT(-1, (int)cal.dig_T3);
	TEST_ASSERT_EQ_INT(1, (int)cal.dig_P1);
	TEST_ASSERT_EQ_INT(-32000, (int)cal.dig_P2);
}

/* ====================================================================== */
/*  BMP280: raw data parsing                                              */
/* ====================================================================== */

static void test_bmp280_raw_parsing(void) {
	uint8_t burst[6] = {0x66, 0x6E, 0x07, 0x65, 0x2A, 0x9C};
	int32_t p = bmp280_raw_pressure(burst);
	int32_t t = bmp280_raw_temperature(burst);

	/* p = (0x66<<12)|(0x6E<<4)|(0x07>>4) = 0x666E0 = 419552 */
	TEST_ASSERT_EQ_INT(419552, (int)p);
	/* t = (0x65<<12)|(0x2A<<4)|(0x9C>>4) = 0x652A9 = 414377 */
	TEST_ASSERT_EQ_INT(414377, (int)t);
}

static void test_bmp280_raw_parsing_golden(void) {
	/* Encode adc_T=519888, adc_P=415148 into burst format */
	/* adc_T = 519888 = 0x7EE30 → msb=0x7E, lsb=0xE3, xlsb=0x0_ (shifted left 4) */
	/* Actually: 20-bit left-justified: raw = value, burst = value>>12, (value>>4)&0xFF, (value<<4)&0xFF */
	int32_t adc_T = 519888;
	int32_t adc_P = 415148;
	uint8_t burst[6];
	burst[0] = (uint8_t)(adc_P >> 12);
	burst[1] = (uint8_t)((adc_P >> 4) & 0xFF);
	burst[2] = (uint8_t)((adc_P << 4) & 0xFF);
	burst[3] = (uint8_t)(adc_T >> 12);
	burst[4] = (uint8_t)((adc_T >> 4) & 0xFF);
	burst[5] = (uint8_t)((adc_T << 4) & 0xFF);

	TEST_ASSERT_EQ_INT(415148, (int)bmp280_raw_pressure(burst));
	TEST_ASSERT_EQ_INT(519888, (int)bmp280_raw_temperature(burst));
}

/* ====================================================================== */
/*  BMP280: golden vector compensation                                    */
/* ====================================================================== */

static void test_bmp280_golden_vector(void) {
	bmp280_calib_t cal = {27504, 26435, -1000,
	                      36477, -10685, 3024,
	                      2855, 132, -3475,
	                      1467, -5697, 7153};
	int32_t adc_T = 519888;
	int32_t adc_P = 415148;

	int32_t t_fine = 0;
	int32_t T_centi = bmp280_compensate_T_int32(adc_T, &cal, &t_fine);
	TEST_ASSERT_EQ_INT(128422, (int)t_fine);
	TEST_ASSERT_EQ_INT(2508, (int)T_centi);  /* 25.08 degC */

	uint32_t P_q248 = bmp280_compensate_P_int64(adc_P, t_fine, &cal);
	uint32_t P_pa = bmp280_q248_to_pa(P_q248);
	TEST_ASSERT_EQ_INT(101827, (int)P_pa);  /* ≈1018.27 hPa */
}

static void test_bmp280_golden_full_compensate(void) {
	bmp280_calib_t cal = {27504, 26435, -1000,
	                      36477, -10685, 3024,
	                      2855, 132, -3475,
	                      1467, -5697, 7153};

	/* Encode golden ADC values into burst */
	int32_t adc_T = 519888, adc_P = 415148;
	uint8_t burst[6];
	burst[0] = (uint8_t)(adc_P >> 12);
	burst[1] = (uint8_t)((adc_P >> 4) & 0xFF);
	burst[2] = (uint8_t)((adc_P << 4) & 0xFF);
	burst[3] = (uint8_t)(adc_T >> 12);
	burst[4] = (uint8_t)((adc_T >> 4) & 0xFF);
	burst[5] = (uint8_t)((adc_T << 4) & 0xFF);

	int32_t t_fine = 0;
	bmp280_sample_t out;
	bmp280_compensate(burst, &cal, &t_fine, &out);

	TEST_ASSERT_EQ_INT(25080, (int)out.temp_milli_c);  /* 25.08C in milli */
	TEST_ASSERT_EQ_INT(101827, (int)out.pressure_pa);   /* ≈1018 hPa */
	TEST_ASSERT_EQ_INT(128422, (int)t_fine);
}

/* ====================================================================== */
/*  BMP280: divide-by-zero protection (dig_P1 = 0)                       */
/* ====================================================================== */

static void test_bmp280_divide_by_zero_protection(void) {
	bmp280_calib_t cal = {};
	cal.dig_P1 = 0;
	int32_t t_fine = 512300;  /* ~25C */
	uint32_t P = bmp280_compensate_P_int64(400000, t_fine, &cal);
	TEST_ASSERT_EQ_INT(0, (int)P);
}

/* ====================================================================== */
/*  BMP280: conversion timing                                            */
/* ====================================================================== */

static void test_bmp280_conv_time_forced_default(void) {
	/* x2 temp (osrs_t=2), x4 pressure (osrs_p=3) */
	/* typ = 1250 + 2300*1 + 2300*2 = 8150us */
	/* max = 8150 * 2 = 16300us */
	uint32_t conv = bmp280_conv_time_us(BMP280_OSRS_T_X2, BMP280_OSRS_P_X4);
	TEST_ASSERT_EQ_INT(16300, (int)conv);
	TEST_ASSERT_EQ_INT(16300, (int)BMP280_CONV_FORCED_DEFAULT_US);
}

static void test_bmp280_conv_time_exceeds_bus_timeout(void) {
	/* The i2cBus has a 10ms bus-operation timeout. The BMP280 forced
	 * conversion bound (16.3ms) exceeds 10ms — proves we must schedule
	 * the conversion as a separate state, not hold the bus. */
	uint32_t conv = bmp280_conv_time_us(BMP280_OSRS_T_X2, BMP280_OSRS_P_X4);
	TEST_ASSERT(conv > 10000, "forced conversion must exceed 10ms bus timeout");
}

static void test_bmp280_conv_time_x16_both(void) {
	/* x16 temp + x16 pressure: typ = 1250 + 2300*4 + 2300*4 = 19650us */
	/* max = 19650 * 2 = 39300us */
	uint32_t conv = bmp280_conv_time_us(BMP280_OSRS_T_X16, BMP280_OSRS_P_X16);
	TEST_ASSERT_EQ_INT(39300, (int)conv);
}

static void test_bmp280_conv_time_skipped(void) {
	/* Both skipped: typ = 1250 + 0 + 0 = 1250us */
	/* max = 1250 * 2 = 2500us */
	uint32_t conv = bmp280_conv_time_us(BMP280_OSRS_T_SKIP, BMP280_OSRS_P_SKIP);
	TEST_ASSERT_EQ_INT(2500, (int)conv);
}

/* ====================================================================== */
/*  BMP280: continuous mode rate                                         */
/* ====================================================================== */

static void test_bmp280_continuous_rate(void) {
	/* ctrl_meas=0x4F + config=0x28 → normal mode, t_sb=62.5ms
	 * Max conversion = 16.3ms + 62.5ms standby = 78.8ms period
	 * Rate_min = 1/0.0788 ≈ 12.69 Hz
	 * We advertise floor rate, NOT optimistic ~14 Hz */
	TEST_ASSERT(BMP280_CONTINUOUS_RATE_HZ <= 13,
	            "continuous rate must not be optimistic ~14Hz");
	TEST_ASSERT(BMP280_CONTINUOUS_RATE_HZ >= 12,
	            "continuous rate must be measured ~12.8Hz");
	TEST_ASSERT_EQ_INT(62500, (int)BMP280_T_SB_62_5_US);
}

static void test_bmp280_continuous_period_bound(void) {
	/* Max period = conv_max + standby = 16300 + 62500 = 78800us */
	uint32_t conv_max = bmp280_conv_time_us(BMP280_OSRS_T_X2, BMP280_OSRS_P_X4);
	uint32_t period_max_us = conv_max + BMP280_T_SB_62_5_US;
	TEST_ASSERT_EQ_INT(78800, (int)period_max_us);
	/* Min rate = 1e6 / 78800 = 12.69 Hz */
	uint32_t rate_min = 1000000u / period_max_us;
	TEST_ASSERT_EQ_INT(12, (int)rate_min);
}

/* ====================================================================== */
/*  BMP280: altitude calculation (barometric formula)                    */
/* ====================================================================== */

static void test_bmp280_altitude_sea_level(void) {
	/* At sea level, P=P0 → ratio=1 → altitude = 0 */
	int32_t alt = bmp280_altitude_meters(101325, 101325);
	TEST_ASSERT(alt >= -1 && alt <= 1, "altitude at sea level ~0");
}

static void test_bmp280_altitude_elevated(void) {
	/* P=100000 vs P0=101325 → barometric formula gives ~111m.
	 * (linear gave ~580m, which was wrong for non-small deltas.) */
	int32_t alt = bmp280_altitude_meters(100000, 101325);
	TEST_ASSERT(alt > 105 && alt < 120, "barometric altitude for 100000 Pa");
}

static void test_bmp280_altitude_1000m_reference(void) {
	/* Standard atmosphere: at 1000m, P ≈ 89875 Pa.
	 * Reverse: feeding 90000 Pa should give ~990m (plan spec: ~1000m). */
	int32_t alt = bmp280_altitude_meters(90000, 101325);
	TEST_ASSERT(alt > 985 && alt < 1005, "altitude for 90000 Pa ~990m");
}

static void test_bmp280_altitude_zero_sea_level(void) {
	int32_t alt = bmp280_altitude_meters(101325, 0);
	TEST_ASSERT_EQ_INT(0, (int)alt);
}

static void test_bmp280_altitude_zero_pressure(void) {
	int32_t alt = bmp280_altitude_meters(0, 101325);
	TEST_ASSERT_EQ_INT(0, (int)alt);
}

static void test_bmp280_altitude_negative(void) {
	/* Pressure above sea level → negative altitude */
	int32_t alt = bmp280_altitude_meters(102000, 101325);
	TEST_ASSERT(alt < 0, "above sea level pressure → negative altitude");
}

/* ====================================================================== */
/*  BMP280: Q24.8 conversion                                             */
/* ====================================================================== */

static void test_bmp280_q248_to_pa(void) {
	TEST_ASSERT_EQ_INT(101827, (int)bmp280_q248_to_pa(26067708u));
	TEST_ASSERT_EQ_INT(256, (int)bmp280_q248_to_pa(65536u));
	TEST_ASSERT_EQ_INT(0, (int)bmp280_q248_to_pa(0u));
	TEST_ASSERT_EQ_INT(1, (int)bmp280_q248_to_pa(128u));  /* rounds to 1 */
	TEST_ASSERT_EQ_INT(0, (int)bmp280_q248_to_pa(127u));  /* rounds to 0 */
}

static void test_bmp280_centi_to_milli(void) {
	TEST_ASSERT_EQ_INT(25080, (int)bmp280_centi_to_milli(2508));
	TEST_ASSERT_EQ_INT(0, (int)bmp280_centi_to_milli(0));
	TEST_ASSERT_EQ_INT(-45000, (int)bmp280_centi_to_milli(-4500));
}

/* ====================================================================== */
/*  BMP280: status register decode                                       */
/* ====================================================================== */

static void test_bmp280_status_measuring(void) {
	bmp280_status_t st;
	bmp280_parse_status(0x08, &st);
	TEST_ASSERT(st.measuring, "measuring bit set");
	TEST_ASSERT(!st.nvm_copying, "nvm bit clear");
}

static void test_bmp280_status_nvm(void) {
	bmp280_status_t st;
	bmp280_parse_status(0x01, &st);
	TEST_ASSERT(!st.measuring, "measuring bit clear");
	TEST_ASSERT(st.nvm_copying, "nvm bit set");
}

static void test_bmp280_status_idle(void) {
	bmp280_status_t st;
	bmp280_parse_status(0x00, &st);
	TEST_ASSERT(!st.measuring, "measuring bit clear");
	TEST_ASSERT(!st.nvm_copying, "nvm bit clear");
}

/* ====================================================================== */
/*  BMP280: compensation header constexpr golden vector (compile-time)    */
/* ====================================================================== */

static void test_bmp280_compile_time_golden_validated(void) {
	/* The static_asserts in bmp280_comp.h validate the golden vector
	 * at compile time. If we reach this test, compilation succeeded,
	 * meaning the temperature golden vector matches. */
	TEST_ASSERT(1, "bmp280_comp.h static_asserts passed at compile time");
	TEST_ASSERT_EQ_INT(128422,
		(int)bmp280_golden::GOLDEN_T_FINE_EXPECTED);
	TEST_ASSERT_EQ_INT(2508,
		(int)bmp280_golden::GOLDEN_T_EXPECTED);
	TEST_ASSERT_EQ_INT(26067708,
		(int)bmp280_golden::GOLDEN_P_EXPECTED);
}

/* ====================================================================== */
/*  main                                                                  */
/* ====================================================================== */

int main(void) {
	test_framework_init();

	/* SHT31-D constants */
	RUN_TEST(test_sht31d_constants);
	RUN_TEST(test_sht31d_conv_timing);

	/* SHT31-D CRC */
	RUN_TEST(test_sht31d_crc_known_vector);
	RUN_TEST(test_sht31d_crc_another_vector);
	RUN_TEST(test_sht31d_crc_single_byte);
	RUN_TEST(test_sht31d_crc_empty);

	/* SHT31-D conversion */
	RUN_TEST(test_sht31d_convert_valid);
	RUN_TEST(test_sht31d_convert_zero_raw);
	RUN_TEST(test_sht31d_convert_max_raw);
	RUN_TEST(test_sht31d_convert_bad_crc_temp);
	RUN_TEST(test_sht31d_convert_bad_crc_hum);

	/* BMP280 constants */
	RUN_TEST(test_bmp280_constants);
	RUN_TEST(test_bmp280_status_bits);

	/* BMP280 calibration parsing */
	RUN_TEST(test_bmp280_parse_calib_signedness);
	RUN_TEST(test_bmp280_parse_calib_negative_values);

	/* BMP280 raw parsing */
	RUN_TEST(test_bmp280_raw_parsing);
	RUN_TEST(test_bmp280_raw_parsing_golden);

	/* BMP280 golden vector */
	RUN_TEST(test_bmp280_golden_vector);
	RUN_TEST(test_bmp280_golden_full_compensate);
	RUN_TEST(test_bmp280_compile_time_golden_validated);

	/* BMP280 divide-by-zero */
	RUN_TEST(test_bmp280_divide_by_zero_protection);

	/* BMP280 conversion timing */
	RUN_TEST(test_bmp280_conv_time_forced_default);
	RUN_TEST(test_bmp280_conv_time_exceeds_bus_timeout);
	RUN_TEST(test_bmp280_conv_time_x16_both);
	RUN_TEST(test_bmp280_conv_time_skipped);

	/* BMP280 continuous mode */
	RUN_TEST(test_bmp280_continuous_rate);
	RUN_TEST(test_bmp280_continuous_period_bound);

	/* BMP280 altitude */
	RUN_TEST(test_bmp280_altitude_sea_level);
	RUN_TEST(test_bmp280_altitude_elevated);
	RUN_TEST(test_bmp280_altitude_1000m_reference);
	RUN_TEST(test_bmp280_altitude_zero_sea_level);
	RUN_TEST(test_bmp280_altitude_zero_pressure);
	RUN_TEST(test_bmp280_altitude_negative);

	/* BMP280 Q24.8 conversion */
	RUN_TEST(test_bmp280_q248_to_pa);
	RUN_TEST(test_bmp280_centi_to_milli);

	/* BMP280 status decode */
	RUN_TEST(test_bmp280_status_measuring);
	RUN_TEST(test_bmp280_status_nvm);
	RUN_TEST(test_bmp280_status_idle);

	return test_framework_finish();
}
