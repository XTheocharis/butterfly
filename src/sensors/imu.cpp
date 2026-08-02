/*
 * imu.cpp - LSM6DS33 / LSM6DS3TR-C pure-logic implementation.
 *
 * No SDK dependencies. All functions are host-testable. The firmware
 * wrapper layer issues async i2cBus transfers and calls these functions
 * to parse / convert / calibrate.
 */
#include "imu.h"
#include <string.h>

/* ---- Compile-time register assertions -------------------------------- */

static_assert(IMU_CFG_CTRL3_C   == 0x44, "CTRL3_C must be BDU+IF_INC");
static_assert(IMU_CFG_CTRL1_XL  == 0x48, "CTRL1_XL must be 104Hz +-4g");
static_assert(IMU_CFG_CTRL2_G   == 0x44, "CTRL2_G must be 104Hz +-500dps");
static_assert(IMU_CFG_INT1_CTRL == 0x03, "INT1 must enable XL+G DRDY");
static_assert(IMU_BURST_LEN     == 12,   "burst must be 12 bytes");
static_assert(IMU_CALIB_SAMPLE_COUNT == 208, "2s at 104Hz = 208 samples");

/* ---- Little-endian helpers ------------------------------------------- */

static int16_t imu_le16(const uint8_t *p) {
	return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int32_t imu_le32s(const uint8_t *p) {
	return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static void imu_i32_le(int32_t val, uint8_t *p) {
	uint32_t u = (uint32_t)val;
	p[0] = (uint8_t)(u);
	p[1] = (uint8_t)(u >> 8);
	p[2] = (uint8_t)(u >> 16);
	p[3] = (uint8_t)(u >> 24);
}

/* ---- Config sequence ------------------------------------------------- */

const imu_regval_t IMU_CONFIG_SEQUENCE[IMU_CONFIG_COUNT] = {
	{ IMU_REG_CTRL3_C,   IMU_CFG_CTRL3_C   },
	{ IMU_REG_CTRL1_XL,  IMU_CFG_CTRL1_XL  },
	{ IMU_REG_CTRL2_G,   IMU_CFG_CTRL2_G   },
	{ IMU_REG_INT1_CTRL, IMU_CFG_INT1_CTRL },
};

/* ---- Variant detection ----------------------------------------------- */

imu_variant_t imu_detect_variant(uint8_t who_am_i) {
	switch (who_am_i) {
	case IMU_WHO_AM_I_LSM6DS33:   return IMU_VARIANT_LSM6DS33;
	case IMU_WHO_AM_I_LSM6DS3TRC: return IMU_VARIANT_LSM6DS3TRC;
	default:                       return IMU_VARIANT_UNKNOWN;
	}
}

/* ---- Burst parsing --------------------------------------------------- */

void imu_parse_burst(const uint8_t burst[IMU_BURST_LEN], imu_raw_t *out) {
	out->gyro_x  = imu_le16(burst + 0);
	out->gyro_y  = imu_le16(burst + 2);
	out->gyro_z  = imu_le16(burst + 4);
	out->accel_x = imu_le16(burst + 6);
	out->accel_y = imu_le16(burst + 8);
	out->accel_z = imu_le16(burst + 10);
}

/* ---- Integer unit conversion ----------------------------------------- */

int32_t imu_accel_to_mg(int16_t raw) {
	return (int32_t)raw * (int32_t)IMU_ACCEL_SENS_NUMER /
	       (int32_t)IMU_ACCEL_SENS_DENOM;
}

int32_t imu_gyro_to_mdps(int16_t raw) {
	return (int32_t)raw * (int32_t)IMU_GYRO_SENS_NUMER /
	       (int32_t)IMU_GYRO_SENS_DENOM;
}

void imu_convert(const imu_raw_t *raw, imu_sample_t *out) {
	out->gyro_x_mdps = imu_gyro_to_mdps(raw->gyro_x);
	out->gyro_y_mdps = imu_gyro_to_mdps(raw->gyro_y);
	out->gyro_z_mdps = imu_gyro_to_mdps(raw->gyro_z);
	out->accel_x_mg  = imu_accel_to_mg(raw->accel_x);
	out->accel_y_mg  = imu_accel_to_mg(raw->accel_y);
	out->accel_z_mg  = imu_accel_to_mg(raw->accel_z);
}

/* ---- Status register ------------------------------------------------- */

void imu_parse_status(uint8_t status_reg, imu_status_t *out) {
	out->accel_ready = (status_reg & IMU_STATUS_XLDA) != 0;
	out->gyro_ready  = (status_reg & IMU_STATUS_GDA)  != 0;
	out->both_ready  = out->accel_ready && out->gyro_ready;
}

/* ---- Calibration state machine --------------------------------------- */

void imu_calib_init(imu_calib_t *c) {
	memset(c, 0, sizeof(*c));
	c->state      = IMU_CALIB_IDLE;
	c->stationary = true;
}

static int32_t imu_abs32(int32_t v) { return v < 0 ? -v : v; }

imu_calib_state_t imu_calib_feed(imu_calib_t *c, const imu_sample_t *s) {
	if (c->state == IMU_CALIB_IDLE) {
		c->state = IMU_CALIB_COLLECT;
	}
	if (c->state != IMU_CALIB_COLLECT) {
		return c->state;
	}

	/* Stationary check: gyro magnitude must stay under threshold. */
	if (imu_abs32(s->gyro_x_mdps) > IMU_CALIB_MOTION_THRESH_MDPS ||
	    imu_abs32(s->gyro_y_mdps) > IMU_CALIB_MOTION_THRESH_MDPS ||
	    imu_abs32(s->gyro_z_mdps) > IMU_CALIB_MOTION_THRESH_MDPS) {
		c->stationary = false;
	}

	/* Accumulate signed sums for bias averaging. */
	c->gyro_sum[0]  += s->gyro_x_mdps;
	c->gyro_sum[1]  += s->gyro_y_mdps;
	c->gyro_sum[2]  += s->gyro_z_mdps;
	c->accel_sum[0] += s->accel_x_mg;
	c->accel_sum[1] += s->accel_y_mg;
	c->accel_sum[2] += s->accel_z_mg;
	c->sample_count++;

	if (c->sample_count >= IMU_CALIB_SAMPLE_COUNT) {
		int32_t n = (int32_t)c->sample_count;
		c->gyro_bias[0]  = c->gyro_sum[0]  / n;
		c->gyro_bias[1]  = c->gyro_sum[1]  / n;
		c->gyro_bias[2]  = c->gyro_sum[2]  / n;
		c->accel_bias[0] = c->accel_sum[0] / n;
		c->accel_bias[1] = c->accel_sum[1] / n;
		c->accel_bias[2] = c->accel_sum[2] / n;
		c->state = IMU_CALIB_DONE;
	}
	return c->state;
}

void imu_apply_bias(const imu_calib_t *c, imu_sample_t *s) {
	s->gyro_x_mdps -= c->gyro_bias[0];
	s->gyro_y_mdps -= c->gyro_bias[1];
	s->gyro_z_mdps -= c->gyro_bias[2];
	s->accel_x_mg  -= c->accel_bias[0];
	s->accel_y_mg  -= c->accel_bias[1];
	s->accel_z_mg  -= c->accel_bias[2];
}

/* ---- Axis transform -------------------------------------------------- */

void imu_transform_axes(imu_sample_t *s) {
	/* CLUE sensor and board share orientation: identity transform. */
	(void)s;
}

/* ---- CRC32 (ISO-HDLC: poly 0xEDB88320, init/xorout 0xFFFFFFFF) ------ */

uint32_t imu_calib_crc32(const uint8_t *data, uint8_t len) {
	uint32_t crc = 0xFFFFFFFFu;
	for (uint8_t i = 0; i < len; i++) {
		crc ^= data[i];
		for (int b = 0; b < 8; b++) {
			uint32_t mask = (uint32_t)(0u - (crc & 1u));
			crc = (crc >> 1) ^ (0xEDB88320u & mask);
		}
	}
	return ~crc;
}

/* ---- Calibration serialization --------------------------------------- */

uint8_t imu_calib_serialize(const imu_calib_t *c, imu_variant_t v,
                            uint8_t *out, uint8_t out_size) {
	if (out == 0 || out_size < IMU_CALIB_SER_SIZE) return 0;
	memset(out, 0, IMU_CALIB_SER_SIZE);
	out[0] = IMU_CALIB_VERSION;
	out[1] = (uint8_t)v;

	uint8_t off = 4;
	for (int i = 0; i < 3; i++) { imu_i32_le(c->gyro_bias[i], out + off); off += 4; }
	for (int i = 0; i < 3; i++) { imu_i32_le(c->accel_bias[i], out + off); off += 4; }
	out[28] = c->stationary ? 1u : 0u;

	uint32_t crc = imu_calib_crc32(out, 32);
	imu_i32_le((int32_t)crc, out + 32);
	return IMU_CALIB_SER_SIZE;
}

bool imu_calib_deserialize(const uint8_t *data, uint8_t len,
                           imu_calib_t *c, imu_variant_t *v) {
	if (data == 0 || len < IMU_CALIB_SER_SIZE || c == 0 || v == 0) return false;
	if (data[0] != IMU_CALIB_VERSION) return false;

	uint32_t expected = (uint32_t)imu_le32s(data + 32);
	uint32_t actual   = imu_calib_crc32(data, 32);
	if (expected != actual) return false;

	*v = (imu_variant_t)data[1];
	uint8_t off = 4;
	for (int i = 0; i < 3; i++) { c->gyro_bias[i] = imu_le32s(data + off); off += 4; }
	for (int i = 0; i < 3; i++) { c->accel_bias[i] = imu_le32s(data + off); off += 4; }
	c->stationary   = data[28] != 0;
	c->state        = IMU_CALIB_DONE;
	c->sample_count = IMU_CALIB_SAMPLE_COUNT;
	return true;
}
