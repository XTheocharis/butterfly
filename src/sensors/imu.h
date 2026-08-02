/*
 * imu.h - LSM6DS33 / LSM6DS3TR-C 6-axis IMU pure-logic driver.
 *
 * Register constants, burst parsing, integer unit conversion, stationary
 * calibration state machine, and calibration serialization. No SDK deps.
 *
 * The firmware C++ class (imu.cpp) wraps these with async i2cBus transfers.
 * The host test (test_sensors_imu.cpp) links the .cpp directly.
 *
 * Sensor is at I2C address 0x6A. Two variants share the same register map:
 *   LSM6DS33   WHO_AM_I = 0x69
 *   LSM6DS3TR-C WHO_AM_I = 0x6A
 *
 * Configured at 104 Hz, accel +-4g (0.122 mg/LSB), gyro +-500 dps
 * (17.5 mdps/LSB). BDU + auto-increment enabled. Data-ready IRQ on INT1
 * (P1.06).
 *
 * Burst layout from OUTX_L_G (0x22), 12 bytes little-endian:
 *   [0..1]  gyro X  [2..3]  gyro Y  [4..5]  gyro Z
 *   [6..7]  accel X  [8..9]  accel Y  [10..11] accel Z
 */
#ifndef SENSORS_IMU_H
#define SENSORS_IMU_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- I2C address (from i2cBus.h frozen table) ------------------------ */

#define IMU_ADDR                0x6Au

/* ---- Register map ---------------------------------------------------- */

#define IMU_REG_WHO_AM_I        0x0Fu
#define IMU_REG_INT1_CTRL       0x0Du
#define IMU_REG_STATUS          0x1Eu
#define IMU_REG_CTRL1_XL        0x10u
#define IMU_REG_CTRL2_G         0x11u
#define IMU_REG_CTRL3_C         0x12u
#define IMU_REG_OUTX_L_G        0x22u

/* WHO_AM_I expected values */
#define IMU_WHO_AM_I_LSM6DS33    0x69u
#define IMU_WHO_AM_I_LSM6DS3TRC  0x6Au

/* ---- Configuration register values ----------------------------------- */

/* CTRL3_C: BDU=1 (block data update), IF_INC=1 (auto-increment) */
#define IMU_CFG_CTRL3_C         0x44u

/* CTRL1_XL: ODR=104Hz(0100), FS=+-4g(10), BW-LPF=00 → 0x48 */
#define IMU_CFG_CTRL1_XL        0x48u

/* CTRL2_G: ODR=104Hz(0100), FS=+-500dps(01), FS_125=0 → 0x44 */
#define IMU_CFG_CTRL2_G         0x44u

/* INT1_CTRL: INT1_DRDY_G(1) + INT1_DRDY_XL(1) → 0x03 */
#define IMU_CFG_INT1_CTRL       0x03u

/* ---- Burst read ------------------------------------------------------ */

#define IMU_BURST_REG           IMU_REG_OUTX_L_G
#define IMU_BURST_LEN           12u

/* STATUS_REG bits */
#define IMU_STATUS_XLDA         (1u << 0)  /* accel data ready */
#define IMU_STATUS_GDA          (1u << 1)  /* gyro data ready */

/* ---- Full-scale sensitivities (integer milli-units per LSB) ---------- */

/* Accel +-4g: 0.122 mg/LSB. Integer: raw * 122 / 1000. */
#define IMU_ACCEL_SENS_NUMER    122u
#define IMU_ACCEL_SENS_DENOM    1000u

/* Gyro +-500dps: 17.5 mdps/LSB. Integer: raw * 35 / 2. */
#define IMU_GYRO_SENS_NUMER     35u
#define IMU_GYRO_SENS_DENOM     2u

/* ---- Sensor IDs (board_manifest.json) -------------------------------- */

#define IMU_SENSOR_ID_ACCEL     1u
#define IMU_SENSOR_ID_GYRO      2u

/* ---- Calibration parameters ------------------------------------------ */

#define IMU_ODR_HZ              104u
#define IMU_CALIB_DURATION_S    2u
#define IMU_CALIB_SAMPLE_COUNT  (IMU_ODR_HZ * IMU_CALIB_DURATION_S)

/* Motion threshold during stationary calibration: 500 mdps = 0.5 dps. */
#define IMU_CALIB_MOTION_THRESH_MDPS  500

/* ---- Variant detection ----------------------------------------------- */

typedef enum {
	IMU_VARIANT_UNKNOWN     = 0,
	IMU_VARIANT_LSM6DS33    = 1,
	IMU_VARIANT_LSM6DS3TRC  = 2,
} imu_variant_t;

imu_variant_t imu_detect_variant(uint8_t who_am_i);

/* ---- Raw burst parsing (little-endian int16) ------------------------- */

typedef struct {
	int16_t gyro_x, gyro_y, gyro_z;
	int16_t accel_x, accel_y, accel_z;
} imu_raw_t;

void imu_parse_burst(const uint8_t burst[IMU_BURST_LEN], imu_raw_t *out);

/* ---- Integer unit conversion ----------------------------------------- */

int32_t imu_accel_to_mg(int16_t raw);
int32_t imu_gyro_to_mdps(int16_t raw);

typedef struct {
	int32_t accel_x_mg, accel_y_mg, accel_z_mg;
	int32_t gyro_x_mdps, gyro_y_mdps, gyro_z_mdps;
} imu_sample_t;

void imu_convert(const imu_raw_t *raw, imu_sample_t *out);

/* ---- Status register decode ------------------------------------------ */

typedef struct {
	bool accel_ready;
	bool gyro_ready;
	bool both_ready;
} imu_status_t;

void imu_parse_status(uint8_t status_reg, imu_status_t *out);

/* ---- Calibration state machine --------------------------------------- */

typedef enum {
	IMU_CALIB_IDLE      = 0,
	IMU_CALIB_COLLECT   = 1,
	IMU_CALIB_DONE      = 2,
} imu_calib_state_t;

typedef struct {
	imu_calib_state_t state;
	uint16_t sample_count;
	int32_t gyro_sum[3];   /* accumulated mdps (signed) */
	int32_t accel_sum[3];  /* accumulated mg (signed)   */
	int32_t gyro_bias[3];  /* mdps */
	int32_t accel_bias[3]; /* mg, includes gravity offset */
	bool stationary;
} imu_calib_t;

void imu_calib_init(imu_calib_t *c);

/* Feed one sample. Returns new state. Transitions COLLECT→DONE when
 * sample_count reaches IMU_CALIB_SAMPLE_COUNT. If any gyro axis exceeds
 * motion threshold, stationary flag is cleared (bias unreliable). */
imu_calib_state_t imu_calib_feed(imu_calib_t *c, const imu_sample_t *s);

/* Subtract bias from sample in-place. */
void imu_apply_bias(const imu_calib_t *c, imu_sample_t *s);

/* ---- Axis transform (sensor PCB → CLUE board frame) ------------------ */
/* LSM6DS33 on CLUE: X→right, Y→up, Z→out-of-screen.
 * Board convention: X→right, Y→up, Z→toward viewer.
 * Identity transform (sensor and board share orientation). */
void imu_transform_axes(imu_sample_t *s);

/* ---- Calibration serialization --------------------------------------- */

#define IMU_CALIB_VERSION   1u
#define IMU_CALIB_SER_SIZE  40u  /* version(1)+variant(1)+pad(2)+
                                    gyro_bias[3]*4+accel_bias[3]*4+
                                    stationary(1)+pad(3)+crc32(4) */

uint8_t imu_calib_serialize(const imu_calib_t *c, imu_variant_t v,
                            uint8_t *out, uint8_t out_size);
bool imu_calib_deserialize(const uint8_t *data, uint8_t len,
                           imu_calib_t *c, imu_variant_t *v);
uint32_t imu_calib_crc32(const uint8_t *data, uint8_t len);

/* ---- Config sequence (for I2C writes) -------------------------------- */

typedef struct { uint8_t reg; uint8_t value; } imu_regval_t;

#define IMU_CONFIG_COUNT  4u
extern const imu_regval_t IMU_CONFIG_SEQUENCE[IMU_CONFIG_COUNT];

#ifdef __cplusplus
}
#endif
#endif /* SENSORS_IMU_H */
