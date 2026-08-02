/*
 * mag.h - LIS3MDL 3-axis magnetometer pure-logic driver.
 *
 * Register constants, burst parsing, integer unit conversion, hard-iron
 * + soft-iron calibration, and field-health check. No SDK deps.
 *
 * Sensor is at I2C address 0x1C. Configured at 40 Hz continuous, +-4 gauss
 * (6842 LSB/gauss), BDU enabled, ultra-high performance XY + Z.
 *
 * Burst layout from OUT_X_L (0x28), 6 bytes little-endian:
 *   [0..1] X  [2..3] Y  [4..5] Z
 *
 * Calibration: guided 30s figure-eight. Hard-iron offset subtracted from
 * each axis. Soft-iron correction is a 3x3 Q16 matrix (off-diagonal
 * elements computed by full ellipsoid fit; current feed populates only
 * the diagonal from per-axis half-ranges, off-diagonal = 0). Field
 * magnitude health check: if calibrated norm deviates >25% from
 * expected, driver falls back to 6DoF (IMU only).
 */
#ifndef SENSORS_MAG_H
#define SENSORS_MAG_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- I2C address ----------------------------------------------------- */

#define MAG_ADDR                0x1Cu

/* ---- Register map ---------------------------------------------------- */

#define MAG_REG_WHO_AM_I        0x0Fu
#define MAG_REG_CTRL1           0x20u
#define MAG_REG_CTRL2           0x21u
#define MAG_REG_CTRL3           0x22u
#define MAG_REG_CTRL4           0x23u
#define MAG_REG_CTRL5           0x24u
#define MAG_REG_STATUS          0x27u
#define MAG_REG_OUT_X_L         0x28u

/* WHO_AM_I expected */
#define MAG_WHO_AM_I            0x3Du

/* ---- Configuration register values ----------------------------------- */

/* CTRL1: TEMP_COMP=1, OM[1:0]=11(UHP), FAST_ODR=1 → 0xF8 */
#define MAG_CFG_CTRL1           0xF8u

/* CTRL2: FS[1:0]=00 (+-4 gauss) → 0x00 */
#define MAG_CFG_CTRL2           0x00u

/* CTRL3: MD[1:0]=00 (continuous) → 0x00 */
#define MAG_CFG_CTRL3           0x00u

/* CTRL4: OMZ[1:0]=11 (Z UHP) → 0x0C */
#define MAG_CFG_CTRL4           0x0Cu

/* CTRL5: BDU=1, FAST_READ=0 → 0x40 */
#define MAG_CFG_CTRL5           0x40u

/* ---- Burst read ------------------------------------------------------ */

#define MAG_BURST_REG           MAG_REG_OUT_X_L
#define MAG_BURST_LEN           6u

/* STATUS_REG bit 7 = ZYXDA (new data for all axes) */
#define MAG_STATUS_NEW_DATA     (1u << 3)  /* ZYXDA */

/* ---- Full-scale sensitivity ------------------------------------------ */

/* +-4 gauss: 6842 LSB/gauss → milligauss = raw * 1000 / 6842 */
#define MAG_SENS_NUMER          1000u
#define MAG_SENS_DENOM          6842u

/* ---- Sensor ID ------------------------------------------------------- */

#define MAG_SENSOR_ID           3u

/* Earth field health: if calibrated magnitude deviates >25% from
 * expected, fall back to 6DoF. Expected ~250-650 mGauss depending on
 * location. Default 500 mGauss (mid-latitude average). */
#define MAG_FIELD_NORM_DEFAULT_MG   500u
#define MAG_FIELD_HEALTH_PCT         25u   /* ±25% tolerance */

/* ---- Calibration parameters ------------------------------------------ */

#define MAG_CALIB_DURATION_S    30u
#define MAG_CALIB_ODR_HZ        40u
#define MAG_CALIB_SAMPLE_COUNT  (MAG_CALIB_ODR_HZ * MAG_CALIB_DURATION_S)

/* ---- Variant / detection --------------------------------------------- */

bool mag_detect(uint8_t who_am_i);

/* ---- Raw burst parsing ----------------------------------------------- */

typedef struct {
	int16_t x, y, z;
} mag_raw_t;

void mag_parse_burst(const uint8_t burst[MAG_BURST_LEN], mag_raw_t *out);

/* ---- Integer unit conversion ----------------------------------------- */

int32_t mag_raw_to_milligauss(int16_t raw);

typedef struct {
	int32_t x_mg, y_mg, z_mg;
} mag_sample_t;

void mag_convert(const mag_raw_t *raw, mag_sample_t *out);

/* ---- Field health check ---------------------------------------------- */

/* Compute magnitude. Returns milligauss. */
int32_t mag_magnitude_mg(const mag_sample_t *s);

/* Check if calibrated field is within tolerance of expected norm.
 * tolerance_pct = MAG_FIELD_HEALTH_PCT.
 * Returns true if healthy, false if >25% deviation (fall back to 6DoF). */
bool mag_field_healthy(const mag_sample_t *s, int32_t expected_norm_mg,
                       uint32_t tolerance_pct);

/* ---- Hard-iron + soft-iron calibration ------------------------------- */

typedef enum {
	MAG_CALIB_IDLE      = 0,
	MAG_CALIB_COLLECT   = 1,
	MAG_CALIB_DONE      = 2,
} mag_calib_state_t;

typedef struct {
	mag_calib_state_t state;
	uint16_t sample_count;
	int32_t min_mg[3];
	int32_t max_mg[3];
	int32_t hard_iron[3];        /* milligauss offset */
	int32_t soft_iron_q16[3][3]; /* Q16 3x3 scale matrix (65536 = 1.0) */
	bool complete;
} mag_calib_t;

void mag_calib_init(mag_calib_t *c);

/* Feed one sample. Tracks min/max per axis. Transitions COLLECT→DONE
 * when sample_count reaches MAG_CALIB_SAMPLE_COUNT. On completion,
 * computes hard-iron offsets and soft-iron diagonal scales. */
mag_calib_state_t mag_calib_feed(mag_calib_t *c, const mag_sample_t *s);

/* Apply calibration: subtract hard-iron, apply soft-iron 3x3 matrix.
 * soft_iron_q16 is Q16 fixed-point:
 *   corrected[i] = (sum_j soft_iron_q16[i][j] * (raw[j] - hard_iron[j])) >> 16 */
void mag_apply_calib(const mag_calib_t *c, mag_sample_t *s);

/* Validate soft-iron matrix: every element must be finite and in
 * a reasonable range (Q16 between 16384 and 262144, i.e. 0.25x to 4x).
 * Off-diagonal elements are additionally allowed to be 0 (not yet
 * fit); non-zero off-diagonal must fall in the same range. */
bool mag_soft_iron_valid(const mag_calib_t *c);

/* ---- Axis transform -------------------------------------------------- */

/* LIS3MDL on CLUE: X→right, Y→up, Z→out-of-screen.
 * Board convention: X→right, Y→up, Z→toward viewer.
 * Identity transform. */
void mag_transform_axes(mag_sample_t *s);

/* ---- Calibration serialization --------------------------------------- */

#define MAG_CALIB_VERSION   1u
#define MAG_CALIB_SER_SIZE  60u  /* version(1)+status(1)+pad(2)+
                                    hard_iron[3]*4+soft_q16[3][3]*4+
                                    pad(4)+crc32(4) */

uint8_t mag_calib_serialize(const mag_calib_t *c,
                            uint8_t *out, uint8_t out_size);
bool mag_calib_deserialize(const uint8_t *data, uint8_t len,
                           mag_calib_t *c);
uint32_t mag_calib_crc32(const uint8_t *data, uint8_t len);

/* ---- Config sequence ------------------------------------------------- */

typedef struct { uint8_t reg; uint8_t value; } mag_regval_t;

#define MAG_CONFIG_COUNT  5u
extern const mag_regval_t MAG_CONFIG_SEQUENCE[MAG_CONFIG_COUNT];

#ifdef __cplusplus
}
#endif
#endif /* SENSORS_MAG_H */
