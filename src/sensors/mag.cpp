/*
 * mag.cpp - LIS3MDL magnetometer pure-logic implementation.
 *
 * No SDK dependencies. All functions host-testable.
 */
#include "mag.h"
#include <string.h>

/* ---- Compile-time register assertions -------------------------------- */

static_assert(MAG_CFG_CTRL1 == 0xF8, "CTRL1 must be TEMP_COMP+UHP+FAST_ODR");
static_assert(MAG_CFG_CTRL2 == 0x00, "CTRL2 must be +-4 gauss");
static_assert(MAG_CFG_CTRL3 == 0x00, "CTRL3 must be continuous mode");
static_assert(MAG_CFG_CTRL4 == 0x0C, "CTRL4 must be Z UHP");
static_assert(MAG_CFG_CTRL5 == 0x40, "CTRL5 must be BDU");
static_assert(MAG_BURST_LEN == 6,    "burst must be 6 bytes");
static_assert(MAG_CALIB_SAMPLE_COUNT == 1200, "30s at 40Hz = 1200");

/* ---- Little-endian helpers ------------------------------------------- */

static int16_t mag_le16(const uint8_t *p) {
	return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int32_t mag_le32s(const uint8_t *p) {
	return (int32_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	                 ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static void mag_i32_le(int32_t val, uint8_t *p) {
	uint32_t u = (uint32_t)val;
	p[0] = (uint8_t)(u);
	p[1] = (uint8_t)(u >> 8);
	p[2] = (uint8_t)(u >> 16);
	p[3] = (uint8_t)(u >> 24);
}

/* ---- Config sequence ------------------------------------------------- */

const mag_regval_t MAG_CONFIG_SEQUENCE[MAG_CONFIG_COUNT] = {
	{ MAG_REG_CTRL1, MAG_CFG_CTRL1 },
	{ MAG_REG_CTRL2, MAG_CFG_CTRL2 },
	{ MAG_REG_CTRL3, MAG_CFG_CTRL3 },
	{ MAG_REG_CTRL4, MAG_CFG_CTRL4 },
	{ MAG_REG_CTRL5, MAG_CFG_CTRL5 },
};

/* ---- Detection ------------------------------------------------------- */

bool mag_detect(uint8_t who_am_i) {
	return who_am_i == MAG_WHO_AM_I;
}

/* ---- Burst parsing --------------------------------------------------- */

void mag_parse_burst(const uint8_t burst[MAG_BURST_LEN], mag_raw_t *out) {
	out->x = mag_le16(burst + 0);
	out->y = mag_le16(burst + 2);
	out->z = mag_le16(burst + 4);
}

/* ---- Integer unit conversion ----------------------------------------- */

int32_t mag_raw_to_milligauss(int16_t raw) {
	return (int32_t)raw * (int32_t)MAG_SENS_NUMER /
	       (int32_t)MAG_SENS_DENOM;
}

void mag_convert(const mag_raw_t *raw, mag_sample_t *out) {
	out->x_mg = mag_raw_to_milligauss(raw->x);
	out->y_mg = mag_raw_to_milligauss(raw->y);
	out->z_mg = mag_raw_to_milligauss(raw->z);
}

/* ---- Field magnitude (integer sqrt) ---------------------------------- */

/* Babylonian sqrt approximation for non-negative int32. */
static uint32_t mag_isqrt32(uint32_t n) {
	if (n == 0) return 0;
	uint32_t x = n;
	uint32_t y = (x + 1u) / 2u;
	while (y < x) {
		x = y;
		y = (x + n / x) / 2u;
	}
	return x;
}

int32_t mag_magnitude_mg(const mag_sample_t *s) {
	int64_t sq = (int64_t)s->x_mg * s->x_mg +
	             (int64_t)s->y_mg * s->y_mg +
	             (int64_t)s->z_mg * s->z_mg;
	if (sq < 0) sq = 0;
	return (int32_t)mag_isqrt32((uint32_t)sq);
}

/* ---- Field health check ---------------------------------------------- */

bool mag_field_healthy(const mag_sample_t *s, int32_t expected_norm_mg,
                       uint32_t tolerance_pct) {
	if (expected_norm_mg <= 0) return false;
	int32_t actual = mag_magnitude_mg(s);
	int32_t diff = actual - expected_norm_mg;
	if (diff < 0) diff = -diff;
	/* diff * 100 <= expected * tolerance_pct */
	return (int64_t)diff * 100LL <=
	       (int64_t)expected_norm_mg * (int64_t)tolerance_pct;
}

/* ---- Calibration state machine --------------------------------------- */

void mag_calib_init(mag_calib_t *c) {
	memset(c, 0, sizeof(*c));
	c->state = MAG_CALIB_IDLE;
	/* Initialize min/max to extremes so first sample sets them. */
	c->min_mg[0] = c->min_mg[1] = c->min_mg[2] = INT32_MAX;
	c->max_mg[0] = c->max_mg[1] = c->max_mg[2] = INT32_MIN;
}

static int32_t mag_mini32(int32_t a, int32_t b) { return a < b ? a : b; }
static int32_t mag_maxi32(int32_t a, int32_t b) { return a > b ? a : b; }

static int32_t mag_abs32(int32_t v) { return v < 0 ? -v : v; }

mag_calib_state_t mag_calib_feed(mag_calib_t *c, const mag_sample_t *s) {
	if (c->state == MAG_CALIB_IDLE) {
		c->state = MAG_CALIB_COLLECT;
	}
	if (c->state != MAG_CALIB_COLLECT) {
		return c->state;
	}

	int32_t vals[3] = { s->x_mg, s->y_mg, s->z_mg };
	for (int i = 0; i < 3; i++) {
		c->min_mg[i] = mag_mini32(c->min_mg[i], vals[i]);
		c->max_mg[i] = mag_maxi32(c->max_mg[i], vals[i]);
	}
	c->sample_count++;

	if (c->sample_count >= MAG_CALIB_SAMPLE_COUNT) {
		/* Hard-iron: midpoint of min/max. */
		for (int i = 0; i < 3; i++) {
			c->hard_iron[i] = (c->min_mg[i] + c->max_mg[i]) / 2;
		}
		/* Soft-iron: diagonal scale based on average half-range.
		 * avg_half = mean of ((max-min)/2) across 3 axes.
		 * scale[i] = avg_half / half[i] in Q16. */
		int32_t half[3], sum = 0;
		for (int i = 0; i < 3; i++) {
			half[i] = mag_abs32(c->max_mg[i] - c->min_mg[i]) / 2;
			sum += half[i];
		}
		int32_t avg = sum / 3;
		for (int i = 0; i < 3; i++) {
			/* Diagonal: per-axis scale from average half-range.
			 * Off-diagonal stays at the zero initialized by
			 * mag_calib_init() until a full ellipsoid fit lands. */
			if (half[i] > 0) {
				c->soft_iron_q16[i][i] =
					(int32_t)(((int64_t)avg << 16) / half[i]);
			} else {
				c->soft_iron_q16[i][i] = 65536; /* 1.0 in Q16 */
			}
		}
		c->complete = true;
		c->state = MAG_CALIB_DONE;
	}
	return c->state;
}

void mag_apply_calib(const mag_calib_t *c, mag_sample_t *s) {
	int32_t vals[3] = { s->x_mg, s->y_mg, s->z_mg };
	int32_t centered[3];
	for (int j = 0; j < 3; j++) {
		centered[j] = vals[j] - c->hard_iron[j];
	}
	int32_t out[3];
	for (int i = 0; i < 3; i++) {
		int64_t acc = 0;
		for (int j = 0; j < 3; j++) {
			acc += (int64_t)c->soft_iron_q16[i][j] * centered[j];
		}
		out[i] = (int32_t)(acc >> 16);
	}
	s->x_mg = out[0];
	s->y_mg = out[1];
	s->z_mg = out[2];
}

bool mag_soft_iron_valid(const mag_calib_t *c) {
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			int32_t v = c->soft_iron_q16[i][j];
			if (i != j && v == 0) {
				/* Off-diagonal may be 0 until a full fit lands. */
				continue;
			}
			if (v < 16384 || v > 262144) { /* 0.25x to 4.0x */
				return false;
			}
		}
	}
	return true;
}

/* ---- Axis transform -------------------------------------------------- */

void mag_transform_axes(mag_sample_t *s) {
	/* CLUE sensor and board share orientation: identity transform. */
	(void)s;
}

/* ---- CRC32 (ISO-HDLC) ------------------------------------------------ */

uint32_t mag_calib_crc32(const uint8_t *data, uint8_t len) {
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

uint8_t mag_calib_serialize(const mag_calib_t *c,
                            uint8_t *out, uint8_t out_size) {
	if (out == 0 || out_size < MAG_CALIB_SER_SIZE) return 0;
	memset(out, 0, MAG_CALIB_SER_SIZE);
	out[0] = MAG_CALIB_VERSION;
	out[1] = c->complete ? 1u : 0u;

	uint8_t off = 4;
	for (int i = 0; i < 3; i++) { mag_i32_le(c->hard_iron[i], out + off); off += 4; }
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			mag_i32_le(c->soft_iron_q16[i][j], out + off);
			off += 4;
		}
	}
	/* out[52..55] reserved */

	uint32_t crc = mag_calib_crc32(out, 56);
	mag_i32_le((int32_t)crc, out + 56);
	return MAG_CALIB_SER_SIZE;
}

bool mag_calib_deserialize(const uint8_t *data, uint8_t len, mag_calib_t *c) {
	if (data == 0 || len < MAG_CALIB_SER_SIZE || c == 0) return false;
	if (data[0] != MAG_CALIB_VERSION) return false;

	uint32_t expected = (uint32_t)mag_le32s(data + 56);
	uint32_t actual   = mag_calib_crc32(data, 56);
	if (expected != actual) return false;

	uint8_t off = 4;
	for (int i = 0; i < 3; i++) { c->hard_iron[i] = mag_le32s(data + off); off += 4; }
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 3; j++) {
			c->soft_iron_q16[i][j] = mag_le32s(data + off);
			off += 4;
		}
	}
	c->complete = data[1] != 0;
	c->state    = MAG_CALIB_DONE;
	c->sample_count = MAG_CALIB_SAMPLE_COUNT;
	return true;
}
