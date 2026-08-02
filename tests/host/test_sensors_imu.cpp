/*
 * test_sensors_imu.cpp - host tests for LSM6DS33/LSM6DS3TR-C IMU and
 * LIS3MDL magnetometer pure-logic drivers.
 *
 * Covers: WHO_AM_I variant detection, register constant values, config
 * sequence, little-endian burst parsing, integer unit conversion, status
 * register decode, calibration state machine (stationary detection, bias
 * computation), bias application, axis transforms, CRC32 known vectors,
 * serialization round-trip, deserialization rejection (wrong version,
 * corrupted CRC), mag field magnitude and health check, mag hard/soft-iron
 * calibration min/max tracking, soft-iron validation.
 *
 * No SDK deps, no hardware. Includes driver .cpp files directly.
 */
#include "test_framework.h"
#include "../../src/sensors/imu.cpp"
#include "../../src/sensors/mag.cpp"
#include <string.h>

/* ====================================================================== */
/*  IMU: register constants                                               */
/* ====================================================================== */

static void test_imu_register_constants(void) {
	TEST_ASSERT_EQ_INT(0x6A, (int)IMU_ADDR);
	TEST_ASSERT_EQ_INT(0x0F, (int)IMU_REG_WHO_AM_I);
	TEST_ASSERT_EQ_INT(0x69, (int)IMU_WHO_AM_I_LSM6DS33);
	TEST_ASSERT_EQ_INT(0x6A, (int)IMU_WHO_AM_I_LSM6DS3TRC);
	TEST_ASSERT_EQ_INT(0x44, (int)IMU_CFG_CTRL3_C);
	TEST_ASSERT_EQ_INT(0x48, (int)IMU_CFG_CTRL1_XL);
	TEST_ASSERT_EQ_INT(0x44, (int)IMU_CFG_CTRL2_G);
	TEST_ASSERT_EQ_INT(0x03, (int)IMU_CFG_INT1_CTRL);
	TEST_ASSERT_EQ_INT(0x22, (int)IMU_REG_OUTX_L_G);
	TEST_ASSERT_EQ_INT(12,   (int)IMU_BURST_LEN);
}

static void test_imu_sensor_ids(void) {
	TEST_ASSERT_EQ_INT(1, (int)IMU_SENSOR_ID_ACCEL);
	TEST_ASSERT_EQ_INT(2, (int)IMU_SENSOR_ID_GYRO);
}

static void test_imu_config_sequence_order(void) {
	TEST_ASSERT_EQ_INT(4, (int)IMU_CONFIG_COUNT);
	TEST_ASSERT_EQ_INT(0x12, (int)IMU_CONFIG_SEQUENCE[0].reg); /* CTRL3_C first */
	TEST_ASSERT_EQ_INT(0x44, (int)IMU_CONFIG_SEQUENCE[0].value);
	TEST_ASSERT_EQ_INT(0x10, (int)IMU_CONFIG_SEQUENCE[1].reg); /* CTRL1_XL */
	TEST_ASSERT_EQ_INT(0x11, (int)IMU_CONFIG_SEQUENCE[2].reg); /* CTRL2_G */
	TEST_ASSERT_EQ_INT(0x0D, (int)IMU_CONFIG_SEQUENCE[3].reg); /* INT1_CTRL */
}

/* ====================================================================== */
/*  IMU: variant detection                                               */
/* ====================================================================== */

static void test_imu_detect_lsm6ds33(void) {
	TEST_ASSERT_EQ_INT((int)IMU_VARIANT_LSM6DS33,
	                   (int)imu_detect_variant(0x69));
}

static void test_imu_detect_lsm6ds3trc(void) {
	TEST_ASSERT_EQ_INT((int)IMU_VARIANT_LSM6DS3TRC,
	                   (int)imu_detect_variant(0x6A));
}

static void test_imu_detect_unknown(void) {
	TEST_ASSERT_EQ_INT((int)IMU_VARIANT_UNKNOWN,
	                   (int)imu_detect_variant(0xFF));
	TEST_ASSERT_EQ_INT((int)IMU_VARIANT_UNKNOWN,
	                   (int)imu_detect_variant(0x00));
}

/* ====================================================================== */
/*  IMU: little-endian burst parsing                                     */
/* ====================================================================== */

static void test_imu_burst_parse_all_positive(void) {
	uint8_t burst[12] = {
		0x64, 0x00,  /* gyro X = 100 */
		0xC8, 0x00,  /* gyro Y = 200 */
		0x2C, 0x01,  /* gyro Z = 300 */
		0xE8, 0x03,  /* accel X = 1000 */
		0x10, 0x27,  /* accel Y = 10000 */
		0xD0, 0x07,  /* accel Z = 2000 */
	};
	imu_raw_t raw;
	imu_parse_burst(burst, &raw);
	TEST_ASSERT_EQ_INT(100,   raw.gyro_x);
	TEST_ASSERT_EQ_INT(200,   raw.gyro_y);
	TEST_ASSERT_EQ_INT(300,   raw.gyro_z);
	TEST_ASSERT_EQ_INT(1000,  raw.accel_x);
	TEST_ASSERT_EQ_INT(10000, raw.accel_y);
	TEST_ASSERT_EQ_INT(2000,  raw.accel_z);
}

static void test_imu_burst_parse_negative_values(void) {
	uint8_t burst[12] = {
		0x00, 0xFF,  /* gyro X = -256 */
		0x9C, 0xFF,  /* gyro Y = -100 */
		0x00, 0x80,  /* gyro Z = -32768 (min int16) */
		0x01, 0x80,  /* accel X = -32767 */
		0xFF, 0x7F,  /* accel Y = 32767 (max int16) */
		0x00, 0x00,  /* accel Z = 0 */
	};
	imu_raw_t raw;
	imu_parse_burst(burst, &raw);
	TEST_ASSERT_EQ_INT(-256,   raw.gyro_x);
	TEST_ASSERT_EQ_INT(-100,   raw.gyro_y);
	TEST_ASSERT_EQ_INT(-32768, raw.gyro_z);
	TEST_ASSERT_EQ_INT(-32767, raw.accel_x);
	TEST_ASSERT_EQ_INT(32767,  raw.accel_y);
	TEST_ASSERT_EQ_INT(0,      raw.accel_z);
}

static void test_imu_burst_parse_byte_order(void) {
	/* Value 0x0102 = 258: LSB=0x02, MSB=0x01 */
	uint8_t burst[12] = {
		0x02, 0x01, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	imu_raw_t raw;
	imu_parse_burst(burst, &raw);
	TEST_ASSERT_EQ_INT(258, raw.gyro_x);
}

/* ====================================================================== */
/*  IMU: unit conversion                                                  */
/* ====================================================================== */

static void test_imu_accel_conversion(void) {
	/* raw=8192 → 8192 * 0.122 = 999.4 ≈ 999 mg */
	TEST_ASSERT_EQ_INT(999, imu_accel_to_mg(8192));
	/* raw=0 → 0 mg */
	TEST_ASSERT_EQ_INT(0, imu_accel_to_mg(0));
	/* raw=-8192 → -999 mg */
	TEST_ASSERT_EQ_INT(-999, imu_accel_to_mg(-8192));
	/* raw=16384 → 16384 * 0.122 = 1998.8 ≈ 1998 mg (~2g at +-4g) */
	TEST_ASSERT_EQ_INT(1998, imu_accel_to_mg(16384));
}

static void test_imu_gyro_conversion(void) {
	/* raw=1000 → 1000 * 17.5 = 17500 mdps */
	TEST_ASSERT_EQ_INT(17500, imu_gyro_to_mdps(1000));
	/* raw=0 → 0 */
	TEST_ASSERT_EQ_INT(0, imu_gyro_to_mdps(0));
	/* raw=-1000 → -17500 */
	TEST_ASSERT_EQ_INT(-17500, imu_gyro_to_mdps(-1000));
	/* raw=28571 → 28571*35/2 = 499992 (int truncation of 499992.5) */
	TEST_ASSERT_EQ_INT(499992, imu_gyro_to_mdps(28571));
}

static void test_imu_convert_all_axes(void) {
	imu_raw_t raw = { 100, -200, 300, 1000, -2000, 3000 };
	imu_sample_t s;
	imu_convert(&raw, &s);
	TEST_ASSERT_EQ_INT(imu_gyro_to_mdps(100),   s.gyro_x_mdps);
	TEST_ASSERT_EQ_INT(imu_gyro_to_mdps(-200),  s.gyro_y_mdps);
	TEST_ASSERT_EQ_INT(imu_gyro_to_mdps(300),   s.gyro_z_mdps);
	TEST_ASSERT_EQ_INT(imu_accel_to_mg(1000),   s.accel_x_mg);
	TEST_ASSERT_EQ_INT(imu_accel_to_mg(-2000),  s.accel_y_mg);
	TEST_ASSERT_EQ_INT(imu_accel_to_mg(3000),   s.accel_z_mg);
}

/* ====================================================================== */
/*  IMU: status register                                                  */
/* ====================================================================== */

static void test_imu_status_both_ready(void) {
	imu_status_t st;
	imu_parse_status(IMU_STATUS_XLDA | IMU_STATUS_GDA, &st);
	TEST_ASSERT(st.accel_ready, "accel ready");
	TEST_ASSERT(st.gyro_ready, "gyro ready");
	TEST_ASSERT(st.both_ready, "both ready");
}

static void test_imu_status_accel_only(void) {
	imu_status_t st;
	imu_parse_status(IMU_STATUS_XLDA, &st);
	TEST_ASSERT(st.accel_ready, "accel ready");
	TEST_ASSERT(!st.gyro_ready, "gyro not ready");
	TEST_ASSERT(!st.both_ready, "not both ready");
}

static void test_imu_status_neither_ready(void) {
	imu_status_t st;
	imu_parse_status(0x00, &st);
	TEST_ASSERT(!st.accel_ready, "accel not ready");
	TEST_ASSERT(!st.gyro_ready, "gyro not ready");
	TEST_ASSERT(!st.both_ready, "not both ready");
}

/* ====================================================================== */
/*  IMU: calibration state machine                                        */
/* ====================================================================== */

static void test_imu_calib_init_state(void) {
	imu_calib_t c;
	imu_calib_init(&c);
	TEST_ASSERT_EQ_INT((int)IMU_CALIB_IDLE, (int)c.state);
	TEST_ASSERT_EQ_INT(0, c.sample_count);
	TEST_ASSERT(c.stationary, "should start stationary");
}

static void test_imu_calib_transitions_to_collect(void) {
	imu_calib_t c;
	imu_calib_init(&c);
	imu_sample_t s = {};
	imu_calib_state_t st = imu_calib_feed(&c, &s);
	TEST_ASSERT_EQ_INT((int)IMU_CALIB_COLLECT, (int)st);
	TEST_ASSERT_EQ_INT(1, c.sample_count);
}

static void test_imu_calib_completes_at_208_samples(void) {
	imu_calib_t c;
	imu_calib_init(&c);
	imu_sample_t s = {};
	for (uint32_t i = 0; i < IMU_CALIB_SAMPLE_COUNT - 1; i++) {
		imu_calib_feed(&c, &s);
	}
	TEST_ASSERT_EQ_INT((int)IMU_CALIB_COLLECT, (int)c.state);
	imu_calib_feed(&c, &s);
	TEST_ASSERT_EQ_INT((int)IMU_CALIB_DONE, (int)c.state);
}

static void test_imu_calib_detects_motion(void) {
	imu_calib_t c;
	imu_calib_init(&c);
	/* Sample with gyro > threshold (500 mdps) */
	imu_sample_t moving = { 0, 0, 0, 600, 0, 0 };
	imu_calib_feed(&c, &moving);
	TEST_ASSERT(!c.stationary, "should detect non-stationary");
}

static void test_imu_calib_stays_stationary_under_threshold(void) {
	imu_calib_t c;
	imu_calib_init(&c);
	imu_sample_t quiet = { 0, 0, 0, 499, 499, 499 };
	imu_calib_feed(&c, &quiet);
	TEST_ASSERT(c.stationary, "should remain stationary under threshold");
}

static void test_imu_calib_bias_computation(void) {
	imu_calib_t c;
	imu_calib_init(&c);
	/* Feed 208 samples with constant bias */
	imu_sample_t s;
	s.gyro_x_mdps = 100;
	s.gyro_y_mdps = -200;
	s.gyro_z_mdps = 0;
	s.accel_x_mg = 500;
	s.accel_y_mg = 0;
	s.accel_z_mg = 1000; /* ~1g gravity */
	for (uint32_t i = 0; i < IMU_CALIB_SAMPLE_COUNT; i++) {
		imu_calib_feed(&c, &s);
	}
	TEST_ASSERT_EQ_INT((int)IMU_CALIB_DONE, (int)c.state);
	TEST_ASSERT_EQ_INT(100,  c.gyro_bias[0]);
	TEST_ASSERT_EQ_INT(-200, c.gyro_bias[1]);
	TEST_ASSERT_EQ_INT(0,    c.gyro_bias[2]);
	TEST_ASSERT_EQ_INT(500,  c.accel_bias[0]);
	TEST_ASSERT_EQ_INT(0,    c.accel_bias[1]);
	TEST_ASSERT_EQ_INT(1000, c.accel_bias[2]);
}

static void test_imu_apply_bias_subtracts(void) {
	imu_calib_t c;
	imu_calib_init(&c);
	c.gyro_bias[0] = 100; c.gyro_bias[1] = -50; c.gyro_bias[2] = 0;
	c.accel_bias[0] = 0; c.accel_bias[1] = 200; c.accel_bias[2] = 980;
	/* imu_sample_t field order: accel_xyz, gyro_xyz */
	imu_sample_t s = { 5, 250, 1000, 150, -30, 10 };
	imu_apply_bias(&c, &s);
	TEST_ASSERT_EQ_INT(50,   s.gyro_x_mdps);   /* 150-100 */
	TEST_ASSERT_EQ_INT(20,   s.gyro_y_mdps);   /* -30+50  */
	TEST_ASSERT_EQ_INT(10,   s.gyro_z_mdps);   /* 10-0    */
	TEST_ASSERT_EQ_INT(5,    s.accel_x_mg);    /* 5-0     */
	TEST_ASSERT_EQ_INT(50,   s.accel_y_mg);    /* 250-200 */
	TEST_ASSERT_EQ_INT(20,   s.accel_z_mg);    /* 1000-980 */
}

/* ====================================================================== */
/*  IMU: axis transform                                                   */
/* ====================================================================== */

static void test_imu_transform_identity(void) {
	imu_sample_t s = { 400, -500, 600, 100, -200, 300 };
	imu_transform_axes(&s);
	TEST_ASSERT_EQ_INT(100,  s.gyro_x_mdps);
	TEST_ASSERT_EQ_INT(-200, s.gyro_y_mdps);
	TEST_ASSERT_EQ_INT(300,  s.gyro_z_mdps);
	TEST_ASSERT_EQ_INT(400,  s.accel_x_mg);
	TEST_ASSERT_EQ_INT(-500, s.accel_y_mg);
	TEST_ASSERT_EQ_INT(600,  s.accel_z_mg);
}

/* ====================================================================== */
/*  IMU: CRC32 known vector                                               */
/* ====================================================================== */

static void test_imu_crc32_known_vector(void) {
	/* CRC32("123456789") = 0xCBF43926 (ISO-HDLC / zlib) */
	const uint8_t data[] = { '1','2','3','4','5','6','7','8','9' };
	TEST_ASSERT_EQ_INT((int)0xCBF43926u, (int)imu_calib_crc32(data, 9));
}

static void test_imu_crc32_empty(void) {
	TEST_ASSERT_EQ_INT((int)0xFFFFFFFFu ^ (int)0xFFFFFFFFu,
	                   (int)imu_calib_crc32((const uint8_t*)"", 0));
}

/* ====================================================================== */
/*  IMU: calibration serialization round-trip                            */
/* ====================================================================== */

static void test_imu_calib_serialize_deserialize(void) {
	imu_calib_t orig;
	imu_calib_init(&orig);
	orig.gyro_bias[0] = 100;  orig.gyro_bias[1] = -200; orig.gyro_bias[2] = 50;
	orig.accel_bias[0] = 980; orig.accel_bias[1] = 10;  orig.accel_bias[2] = -5;
	orig.stationary = true;
	orig.state = IMU_CALIB_DONE;

	uint8_t buf[IMU_CALIB_SER_SIZE];
	uint8_t n = imu_calib_serialize(&orig, IMU_VARIANT_LSM6DS33, buf, sizeof buf);
	TEST_ASSERT_EQ_INT(IMU_CALIB_SER_SIZE, n);

	imu_calib_t decoded;
	imu_variant_t v;
	bool ok = imu_calib_deserialize(buf, n, &decoded, &v);
	TEST_ASSERT(ok, "deserialize should succeed");
	TEST_ASSERT_EQ_INT((int)IMU_VARIANT_LSM6DS33, (int)v);
	TEST_ASSERT_EQ_INT(100,  decoded.gyro_bias[0]);
	TEST_ASSERT_EQ_INT(-200, decoded.gyro_bias[1]);
	TEST_ASSERT_EQ_INT(50,   decoded.gyro_bias[2]);
	TEST_ASSERT_EQ_INT(980,  decoded.accel_bias[0]);
	TEST_ASSERT_EQ_INT(10,   decoded.accel_bias[1]);
	TEST_ASSERT_EQ_INT(-5,   decoded.accel_bias[2]);
	TEST_ASSERT(decoded.stationary, "stationary flag preserved");
}

static void test_imu_calib_serialize_rejects_small_buffer(void) {
	imu_calib_t c;
	imu_calib_init(&c);
	uint8_t buf[10];
	uint8_t n = imu_calib_serialize(&c, IMU_VARIANT_LSM6DS33, buf, sizeof buf);
	TEST_ASSERT_EQ_INT(0, n);
}

static void test_imu_calib_deserialize_rejects_wrong_version(void) {
	uint8_t buf[IMU_CALIB_SER_SIZE];
	memset(buf, 0, sizeof buf);
	buf[0] = 99; /* wrong version */
	imu_calib_t c;
	imu_variant_t v;
	bool ok = imu_calib_deserialize(buf, sizeof buf, &c, &v);
	TEST_ASSERT(!ok, "wrong version should fail");
}

static void test_imu_calib_deserialize_rejects_corrupt_crc(void) {
	imu_calib_t orig;
	imu_calib_init(&orig);
	orig.gyro_bias[0] = 42;
	uint8_t buf[IMU_CALIB_SER_SIZE];
	imu_calib_serialize(&orig, IMU_VARIANT_LSM6DS33, buf, sizeof buf);
	/* Corrupt one data byte (not CRC) */
	buf[4] ^= 0xFF;
	imu_calib_t decoded;
	imu_variant_t v;
	bool ok = imu_calib_deserialize(buf, sizeof buf, &decoded, &v);
	TEST_ASSERT(!ok, "corrupted data should fail CRC check");
}

/* ====================================================================== */
/*  MAG: register constants                                               */
/* ====================================================================== */

static void test_mag_register_constants(void) {
	TEST_ASSERT_EQ_INT(0x1C,  (int)MAG_ADDR);
	TEST_ASSERT_EQ_INT(0x0F,  (int)MAG_REG_WHO_AM_I);
	TEST_ASSERT_EQ_INT(0x3D,  (int)MAG_WHO_AM_I);
	TEST_ASSERT_EQ_INT(0xF8,  (int)MAG_CFG_CTRL1);
	TEST_ASSERT_EQ_INT(0x00,  (int)MAG_CFG_CTRL2);
	TEST_ASSERT_EQ_INT(0x00,  (int)MAG_CFG_CTRL3);
	TEST_ASSERT_EQ_INT(0x0C,  (int)MAG_CFG_CTRL4);
	TEST_ASSERT_EQ_INT(0x40,  (int)MAG_CFG_CTRL5);
	TEST_ASSERT_EQ_INT(0x28,  (int)MAG_REG_OUT_X_L);
	TEST_ASSERT_EQ_INT(6,     (int)MAG_BURST_LEN);
}

static void test_mag_sensor_id(void) {
	TEST_ASSERT_EQ_INT(3, (int)MAG_SENSOR_ID);
}

static void test_mag_config_sequence(void) {
	TEST_ASSERT_EQ_INT(5, (int)MAG_CONFIG_COUNT);
	TEST_ASSERT_EQ_INT(0x20, (int)MAG_CONFIG_SEQUENCE[0].reg);
	TEST_ASSERT_EQ_INT(0xF8, (int)MAG_CONFIG_SEQUENCE[0].value);
	TEST_ASSERT_EQ_INT(0x21, (int)MAG_CONFIG_SEQUENCE[1].reg);
	TEST_ASSERT_EQ_INT(0x23, (int)MAG_CONFIG_SEQUENCE[3].reg);
	TEST_ASSERT_EQ_INT(0x24, (int)MAG_CONFIG_SEQUENCE[4].reg);
}

/* ====================================================================== */
/*  MAG: detection                                                        */
/* ====================================================================== */

static void test_mag_detect_valid(void) {
	TEST_ASSERT(mag_detect(0x3D), "should detect LIS3MDL");
}

static void test_mag_detect_invalid(void) {
	TEST_ASSERT(!mag_detect(0xFF), "should reject unknown ID");
	TEST_ASSERT(!mag_detect(0x00), "should reject zero");
}

/* ====================================================================== */
/*  MAG: burst parsing and conversion                                     */
/* ====================================================================== */

static void test_mag_burst_parse(void) {
	uint8_t burst[6] = {
		0x64, 0x00,  /* X = 100 */
		0xC8, 0x00,  /* Y = 200 */
		0x2C, 0x01,  /* Z = 300 */
	};
	mag_raw_t raw;
	mag_parse_burst(burst, &raw);
	TEST_ASSERT_EQ_INT(100, raw.x);
	TEST_ASSERT_EQ_INT(200, raw.y);
	TEST_ASSERT_EQ_INT(300, raw.z);
}

static void test_mag_burst_parse_negative(void) {
	uint8_t burst[6] = {
		0x9C, 0xFF,  /* X = -100 */
		0x00, 0x80,  /* Y = -32768 */
		0xFF, 0x7F,  /* Z = 32767 */
	};
	mag_raw_t raw;
	mag_parse_burst(burst, &raw);
	TEST_ASSERT_EQ_INT(-100,   raw.x);
	TEST_ASSERT_EQ_INT(-32768, raw.y);
	TEST_ASSERT_EQ_INT(32767,  raw.z);
}

static void test_mag_conversion(void) {
	/* raw=6842 → 6842 * 1000 / 6842 = 1000 milligauss = 1 gauss */
	TEST_ASSERT_EQ_INT(1000, mag_raw_to_milligauss(6842));
	TEST_ASSERT_EQ_INT(0, mag_raw_to_milligauss(0));
	TEST_ASSERT_EQ_INT(-1000, mag_raw_to_milligauss(-6842));
	/* raw=3421 → 3421 * 1000 / 6842 ≈ 500 milligauss */
	TEST_ASSERT_EQ_INT(500, mag_raw_to_milligauss(3421));
}

static void test_mag_convert_all_axes(void) {
	mag_raw_t raw = { 6842, -6842, 3421 };
	mag_sample_t s;
	mag_convert(&raw, &s);
	TEST_ASSERT_EQ_INT(1000,  s.x_mg);
	TEST_ASSERT_EQ_INT(-1000, s.y_mg);
	TEST_ASSERT_EQ_INT(500,   s.z_mg);
}

/* ====================================================================== */
/*  MAG: field magnitude and health check                                 */
/* ====================================================================== */

static void test_mag_magnitude(void) {
	/* 3-4-5 triangle: 300, 400, 500 → magnitude = sqrt(90000+160000+250000) = sqrt(500000) ≈ 707 */
	mag_sample_t s = { 300, 400, 0 };
	int32_t mag = mag_magnitude_mg(&s);
	TEST_ASSERT_EQ_INT(500, mag); /* sqrt(300^2+400^2) = 500 */
}

static void test_mag_magnitude_zero(void) {
	mag_sample_t s = { 0, 0, 0 };
	TEST_ASSERT_EQ_INT(0, mag_magnitude_mg(&s));
}

static void test_mag_field_healthy_within_tolerance(void) {
	mag_sample_t s = { 480, 0, 0 }; /* magnitude = 480, expected 500 → 4% off */
	bool ok = mag_field_healthy(&s, 500, 25);
	TEST_ASSERT(ok, "4% deviation is within 25% tolerance");
}

static void test_mag_field_unhealthy_beyond_tolerance(void) {
	mag_sample_t s = { 300, 0, 0 }; /* magnitude = 300, expected 500 → 40% off */
	bool ok = mag_field_healthy(&s, 500, 25);
	TEST_ASSERT(!ok, "40% deviation exceeds 25% tolerance");
}

static void test_mag_field_health_boundary(void) {
	/* At exactly 25% boundary: magnitude=375, expected=500 → 25% exactly */
	mag_sample_t s = { 375, 0, 0 };
	bool ok = mag_field_healthy(&s, 500, 25);
	TEST_ASSERT(ok, "exactly 25% deviation is within tolerance (<=)");
}

/* ====================================================================== */
/*  MAG: calibration state machine                                        */
/* ====================================================================== */

static void test_mag_calib_init(void) {
	mag_calib_t c;
	mag_calib_init(&c);
	TEST_ASSERT_EQ_INT((int)MAG_CALIB_IDLE, (int)c.state);
	TEST_ASSERT_EQ_INT(0, c.sample_count);
	TEST_ASSERT(!c.complete, "should not be complete at init");
}

static void test_mag_calib_tracks_min_max(void) {
	mag_calib_t c;
	mag_calib_init(&c);
	mag_sample_t s1 = { 100, -200, 300 };
	mag_sample_t s2 = { -50, 400, 100 };
	mag_sample_t s3 = { 200, -100, 250 };
	mag_calib_feed(&c, &s1);
	mag_calib_feed(&c, &s2);
	mag_calib_feed(&c, &s3);
	TEST_ASSERT_EQ_INT(-50,  c.min_mg[0]);
	TEST_ASSERT_EQ_INT(200,  c.max_mg[0]);
	TEST_ASSERT_EQ_INT(-200, c.min_mg[1]);
	TEST_ASSERT_EQ_INT(400,  c.max_mg[1]);
	TEST_ASSERT_EQ_INT(100,  c.min_mg[2]);
	TEST_ASSERT_EQ_INT(300,  c.max_mg[2]);
	TEST_ASSERT_EQ_INT(3,    c.sample_count);
}

static void test_mag_calib_completes_at_1200(void) {
	mag_calib_t c;
	mag_calib_init(&c);
	mag_sample_t s = { 0, 0, 0 };
	for (uint32_t i = 0; i < MAG_CALIB_SAMPLE_COUNT - 1; i++) {
		mag_calib_feed(&c, &s);
	}
	TEST_ASSERT_EQ_INT((int)MAG_CALIB_COLLECT, (int)c.state);
	mag_calib_feed(&c, &s);
	TEST_ASSERT_EQ_INT((int)MAG_CALIB_DONE, (int)c.state);
	TEST_ASSERT(c.complete, "should be complete");
}

static void test_mag_calib_hard_iron_computation(void) {
	mag_calib_t c;
	mag_calib_init(&c);
	/* Simulate field: X ranges [-100,300], Y [-200,200], Z [0,400] */
	mag_sample_t samples[] = {
		{ 100, 0, 200 }, { -100, -200, 0 }, { 300, 200, 400 },
	};
	/* Pad to MAG_CALIB_SAMPLE_COUNT with mid-range values */
	mag_sample_t mid = { 100, 0, 200 };
	for (int i = 0; i < 3; i++) mag_calib_feed(&c, &samples[i]);
	for (uint32_t i = 3; i < MAG_CALIB_SAMPLE_COUNT; i++) mag_calib_feed(&c, &mid);

	TEST_ASSERT_EQ_INT((int)MAG_CALIB_DONE, (int)c.state);
	/* hard_iron = (min+max)/2 */
	TEST_ASSERT_EQ_INT((-100 + 300) / 2, c.hard_iron[0]); /* 100 */
	TEST_ASSERT_EQ_INT((-200 + 200) / 2, c.hard_iron[1]); /* 0 */
	TEST_ASSERT_EQ_INT((0 + 400) / 2,    c.hard_iron[2]); /* 200 */
}

static void test_mag_calib_soft_iron_valid(void) {
	mag_calib_t c;
	mag_calib_init(&c);
	/* Simulate well-conditioned data: similar ranges per axis */
	mag_sample_t s;
	for (uint32_t i = 0; i < MAG_CALIB_SAMPLE_COUNT; i++) {
		int32_t v = (i % 400) - 200; /* -200..199 */
		s.x_mg = v;
		s.y_mg = v;
		s.z_mg = v;
		mag_calib_feed(&c, &s);
	}
	TEST_ASSERT(mag_soft_iron_valid(&c), "soft-iron scales should be valid");
	/* Diagonal should be close to 1.0 (65536 in Q16) since all axes have same range.
	 * Off-diagonal stays at 0 (not yet fit) and is allowed by the validator. */
	for (int i = 0; i < 3; i++) {
		TEST_ASSERT(c.soft_iron_q16[i][i] >= 60000 && c.soft_iron_q16[i][i] <= 70000,
			"diagonal scale should be near 1.0");
		for (int j = 0; j < 3; j++) {
			if (i != j)
				TEST_ASSERT_EQ_INT(0, c.soft_iron_q16[i][j]);
		}
	}
}

static void test_mag_soft_iron_invalid_extreme(void) {
	mag_calib_t c;
	memset(&c, 0, sizeof c);
	/* Set absurd scale values on the diagonal */
	c.soft_iron_q16[0][0] = 100;    /* 0.0015x — way too small */
	c.soft_iron_q16[1][1] = 65536;  /* 1.0x — fine */
	c.soft_iron_q16[2][2] = 65536;
	TEST_ASSERT(!mag_soft_iron_valid(&c), "should reject extreme scale");
}

static void test_mag_apply_calib(void) {
	mag_calib_t c;
	memset(&c, 0, sizeof c);
	c.hard_iron[0] = 100; c.hard_iron[1] = -50; c.hard_iron[2] = 0;
	c.soft_iron_q16[0][0] = 65536; /* 1.0x */
	c.soft_iron_q16[1][1] = 65536;
	c.soft_iron_q16[2][2] = 65536;
	mag_sample_t s = { 200, 0, 500 };
	mag_apply_calib(&c, &s);
	TEST_ASSERT_EQ_INT(100,  s.x_mg); /* 200-100 = 100 */
	TEST_ASSERT_EQ_INT(50,   s.y_mg); /* 0-(-50) = 50 */
	TEST_ASSERT_EQ_INT(500,  s.z_mg); /* 500-0 = 500 */
}

static void test_mag_apply_calib_with_scale(void) {
	mag_calib_t c;
	memset(&c, 0, sizeof c);
	c.hard_iron[0] = 0;
	c.soft_iron_q16[0][0] = 131072; /* 2.0x */
	c.soft_iron_q16[1][1] = 32768;  /* 0.5x */
	c.soft_iron_q16[2][2] = 65536;  /* 1.0x */
	mag_sample_t s = { 100, 200, 300 };
	mag_apply_calib(&c, &s);
	TEST_ASSERT_EQ_INT(200,  s.x_mg); /* 100 * 2.0 = 200 */
	TEST_ASSERT_EQ_INT(100,  s.y_mg); /* 200 * 0.5 = 100 */
	TEST_ASSERT_EQ_INT(300,  s.z_mg); /* 300 * 1.0 = 300 */
}

/* ====================================================================== */
/*  MAG: calibration serialization                                        */
/* ====================================================================== */

static void test_mag_calib_serialize_deserialize(void) {
	mag_calib_t orig;
	mag_calib_init(&orig);
	orig.hard_iron[0] = 150; orig.hard_iron[1] = -75; orig.hard_iron[2] = 0;
	orig.soft_iron_q16[0][0] = 70000;
	orig.soft_iron_q16[1][1] = 60000;
	orig.soft_iron_q16[2][2] = 65536;
	/* Off-diagonal left at 0 (no full ellipsoid fit yet). */
	orig.complete = true;
	orig.state = MAG_CALIB_DONE;

	uint8_t buf[MAG_CALIB_SER_SIZE];
	uint8_t n = mag_calib_serialize(&orig, buf, sizeof buf);
	TEST_ASSERT_EQ_INT(MAG_CALIB_SER_SIZE, n);

	mag_calib_t decoded;
	bool ok = mag_calib_deserialize(buf, n, &decoded);
	TEST_ASSERT(ok, "deserialize should succeed");
	TEST_ASSERT_EQ_INT(150,   decoded.hard_iron[0]);
	TEST_ASSERT_EQ_INT(-75,   decoded.hard_iron[1]);
	TEST_ASSERT_EQ_INT(0,     decoded.hard_iron[2]);
	TEST_ASSERT_EQ_INT(70000, decoded.soft_iron_q16[0][0]);
	TEST_ASSERT_EQ_INT(60000, decoded.soft_iron_q16[1][1]);
	TEST_ASSERT_EQ_INT(65536, decoded.soft_iron_q16[2][2]);
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			if (i != j)
				TEST_ASSERT_EQ_INT(0, decoded.soft_iron_q16[i][j]);
	TEST_ASSERT(decoded.complete, "complete flag preserved");
}

static void test_mag_calib_serialize_rejects_small_buffer(void) {
	mag_calib_t c;
	mag_calib_init(&c);
	uint8_t buf[10];
	uint8_t n = mag_calib_serialize(&c, buf, sizeof buf);
	TEST_ASSERT_EQ_INT(0, n);
}

static void test_mag_calib_deserialize_rejects_corrupt_crc(void) {
	mag_calib_t orig;
	mag_calib_init(&orig);
	orig.hard_iron[0] = 42;
	uint8_t buf[MAG_CALIB_SER_SIZE];
	mag_calib_serialize(&orig, buf, sizeof buf);
	buf[4] ^= 0xFF; /* corrupt data */
	mag_calib_t decoded;
	bool ok = mag_calib_deserialize(buf, sizeof buf, &decoded);
	TEST_ASSERT(!ok, "corrupted data should fail CRC check");
}

/* ====================================================================== */
/*  main                                                                  */
/* ====================================================================== */

int main(void) {
	test_framework_init();

	/* IMU register constants */
	RUN_TEST(test_imu_register_constants);
	RUN_TEST(test_imu_sensor_ids);
	RUN_TEST(test_imu_config_sequence_order);

	/* IMU variant detection */
	RUN_TEST(test_imu_detect_lsm6ds33);
	RUN_TEST(test_imu_detect_lsm6ds3trc);
	RUN_TEST(test_imu_detect_unknown);

	/* IMU burst parsing */
	RUN_TEST(test_imu_burst_parse_all_positive);
	RUN_TEST(test_imu_burst_parse_negative_values);
	RUN_TEST(test_imu_burst_parse_byte_order);

	/* IMU unit conversion */
	RUN_TEST(test_imu_accel_conversion);
	RUN_TEST(test_imu_gyro_conversion);
	RUN_TEST(test_imu_convert_all_axes);

	/* IMU status register */
	RUN_TEST(test_imu_status_both_ready);
	RUN_TEST(test_imu_status_accel_only);
	RUN_TEST(test_imu_status_neither_ready);

	/* IMU calibration */
	RUN_TEST(test_imu_calib_init_state);
	RUN_TEST(test_imu_calib_transitions_to_collect);
	RUN_TEST(test_imu_calib_completes_at_208_samples);
	RUN_TEST(test_imu_calib_detects_motion);
	RUN_TEST(test_imu_calib_stays_stationary_under_threshold);
	RUN_TEST(test_imu_calib_bias_computation);
	RUN_TEST(test_imu_apply_bias_subtracts);

	/* IMU axis transform */
	RUN_TEST(test_imu_transform_identity);

	/* IMU CRC32 */
	RUN_TEST(test_imu_crc32_known_vector);
	RUN_TEST(test_imu_crc32_empty);

	/* IMU serialization */
	RUN_TEST(test_imu_calib_serialize_deserialize);
	RUN_TEST(test_imu_calib_serialize_rejects_small_buffer);
	RUN_TEST(test_imu_calib_deserialize_rejects_wrong_version);
	RUN_TEST(test_imu_calib_deserialize_rejects_corrupt_crc);

	/* MAG register constants */
	RUN_TEST(test_mag_register_constants);
	RUN_TEST(test_mag_sensor_id);
	RUN_TEST(test_mag_config_sequence);

	/* MAG detection */
	RUN_TEST(test_mag_detect_valid);
	RUN_TEST(test_mag_detect_invalid);

	/* MAG burst and conversion */
	RUN_TEST(test_mag_burst_parse);
	RUN_TEST(test_mag_burst_parse_negative);
	RUN_TEST(test_mag_conversion);
	RUN_TEST(test_mag_convert_all_axes);

	/* MAG field health */
	RUN_TEST(test_mag_magnitude);
	RUN_TEST(test_mag_magnitude_zero);
	RUN_TEST(test_mag_field_healthy_within_tolerance);
	RUN_TEST(test_mag_field_unhealthy_beyond_tolerance);
	RUN_TEST(test_mag_field_health_boundary);

	/* MAG calibration */
	RUN_TEST(test_mag_calib_init);
	RUN_TEST(test_mag_calib_tracks_min_max);
	RUN_TEST(test_mag_calib_completes_at_1200);
	RUN_TEST(test_mag_calib_hard_iron_computation);
	RUN_TEST(test_mag_calib_soft_iron_valid);
	RUN_TEST(test_mag_soft_iron_invalid_extreme);
	RUN_TEST(test_mag_apply_calib);
	RUN_TEST(test_mag_apply_calib_with_scale);

	/* MAG serialization */
	RUN_TEST(test_mag_calib_serialize_deserialize);
	RUN_TEST(test_mag_calib_serialize_rejects_small_buffer);
	RUN_TEST(test_mag_calib_deserialize_rejects_corrupt_crc);

	return test_framework_finish();
}
