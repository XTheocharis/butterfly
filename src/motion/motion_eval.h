/*
 * motion_eval.h - Pure-C motion evaluation layer (host-testable, no SDK deps).
 *
 * Contains: Madgwick 2010 AHRS fusion (6DoF + 9DoF with mag fallback),
 * quaternion normalization / finite check / Q30 conversion / Euler output,
 * air-mouse pointer integration (dead zone, sensitivity ramp, fractional
 * carry), tilt-to-D-pad hysteresis FSM, gravity/dot-product gesture
 * primitive, and magnetometer health/recovery state machine.
 *
 * The firmware C++ wrappers (fusion.cpp, air_mouse.cpp, tilt.cpp,
 * gesture.cpp) call these functions with sensor data from i2cBus transfers.
 *
 * Compiled on BOTH host (for unit tests) and device (linked into firmware).
 */
#ifndef MOTION_EVAL_H
#define MOTION_EVAL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Result codes ---------------------------------------------------- */

typedef enum {
	MOTION_RESULT_OK           = 0,
	MOTION_RESULT_SENSOR_FAULT = 1,  /* NaN/Inf in input or quaternion */
} motion_result_t;

/* ---- ODR / timing constants ------------------------------------------ */

#define MOTION_IMU_ODR_HZ    104u
#define MOTION_MAG_ODR_HZ     40u
#define MOTION_IMU_DT_NOMINAL_S  (1.0f / 104.0f)

/* ---- Fusion parameters (Madgwick 2010 AHRS) -------------------------- */

#define MOTION_FUSION_BETA   0.08f

/* ---- Magnetometer fallback / recovery -------------------------------- */

#define MOTION_MAG_DISTURBANCE_TIMEOUT_US   500000ull  /* 500ms */
#define MOTION_MAG_RECOVERY_DURATION_US    1000000ull  /* 1s healthy before recovery */

/* ---- Quaternion (single-precision float) ----------------------------- */

typedef struct {
	float w, x, y, z;  /* q0=w, q1=x, q2=y, q3=z */
} motion_quat_t;

/* ---- Fusion mode ----------------------------------------------------- */

typedef enum {
	MOTION_FUSION_MODE_9DOF = 0,  /* IMU + magnetometer */
	MOTION_FUSION_MODE_6DOF = 1,  /* IMU only (fallback) */
} motion_fusion_mode_t;

/* ---- Fusion state ---------------------------------------------------- */

typedef struct {
	motion_quat_t q;           /* current quaternion estimate */
	motion_fusion_mode_t mode; /* 9DoF or 6DoF fallback */

	/* Mag health tracking */
	bool mag_present;          /* false if sensor not detected */
	bool mag_calibrated;       /* false if no valid calibration */
	bool mag_healthy;          /* last field-health check result */
	uint64_t disturbance_start_us;  /* when mag first became unhealthy (0 = none) */
	uint64_t recovery_start_us;     /* when mag recovery started (0 = none) */
	bool recovering;

	/* Last calibrated mag sample (gauss) + timestamp */
	float last_mx, last_my, last_mz;
	uint64_t last_mag_us;

	/* Earth-frame magnetic reference */
	float ref_bx, ref_bz;      /* horizontal + vertical reference */
	bool ref_set;
} motion_fusion_state_t;

/* ---- Unit conversions (mg/mdps/milligauss → internal) ---------------- */

static inline float motion_mdps_to_rads(int32_t mdps) {
	return (float)mdps * 0.000017453293f;  /* /1000 * PI/180 */
}

static inline float motion_mg_to_g(int32_t mg) {
	return (float)mg * 0.001f;
}

static inline float motion_milligauss_to_gauss(int32_t milligauss) {
	return (float)milligauss * 0.001f;
}

/* ---- Fusion lifecycle ------------------------------------------------ */

void motion_fusion_init(motion_fusion_state_t *st);

/*
 * Evaluate magnetometer health and update fusion mode accordingly.
 *   mag_present:    sensor detected by i2cBus probe
 *   mag_calibrated: valid calibration data available
 *   mag_healthy:    field-health check passed (within 25% of expected)
 *   now_us:         current monotonic timestamp
 * Returns the new fusion mode (also stored in st->mode).
 */
motion_fusion_mode_t motion_mag_health_eval(motion_fusion_state_t *st,
                                            bool mag_present,
                                            bool mag_calibrated,
                                            bool mag_healthy,
                                            uint64_t now_us);

/*
 * Set the earth-frame magnetic reference from a calibrated mag reading.
 * Called on the first healthy mag sample (or when ref needs refresh).
 * Rotates mag to earth frame using current quaternion, projects to
 * horizontal (bx) and vertical (bz) components.
 */
void motion_fusion_set_mag_reference(motion_fusion_state_t *st,
                                     float mx, float my, float mz);

/*
 * Combined fusion update. Handles both 6DoF and 9DoF paths based on
 * st->mode. If mag_valid is false, forces 6DoF for this update.
 *
 *   gx/gy/gz: gyro in rad/s
 *   ax/ay/az: accel in g (will be normalized internally)
 *   mx/my/mz: mag in gauss (ignored if mag_valid=false)
 *   mag_valid: true if healthy mag data available this update
 *   now_us:   current monotonic timestamp
 *   dt:       measured seconds since last update (NOT assumed 1/104)
 *
 * Returns MOTION_RESULT_SENSOR_FAULT if any input is NaN/Inf or if
 * quaternion normalization fails (zero magnitude).
 */
motion_result_t motion_fusion_update(motion_fusion_state_t *st,
                                     float gx, float gy, float gz,
                                     float ax, float ay, float az,
                                     float mx, float my, float mz,
                                     bool mag_valid,
                                     uint64_t now_us,
                                     float dt);

/* ---- Quaternion utilities -------------------------------------------- */

bool motion_quat_is_finite(const motion_quat_t *q);

/* Normalize in-place. Returns SENSOR_FAULT if magnitude is zero or NaN. */
motion_result_t motion_quat_normalize(motion_quat_t *q);

/*
 * Convert float quaternion to signed Q30 int32 array [w, x, y, z].
 * Q30: value = stored / 2^30. Unit quaternion components ∈ [-1, +1]
 * map to [-2^30, +2^30]. Clamps to int32 range.
 * Returns SENSOR_FAULT if any component is NaN/Inf.
 */
motion_result_t motion_quat_to_q30(const motion_quat_t *q, int32_t out[4]);

/*
 * Convert quaternion to Euler angles in millidegrees [yaw, pitch, roll].
 * Uses ZYX Tait-Bryan convention (yaw around Z, pitch around Y, roll around X).
 * Returns SENSOR_FAULT if quaternion is non-finite.
 */
motion_result_t motion_quat_to_euler_millideg(const motion_quat_t *q,
                                              int32_t *yaw_mdeg,
                                              int32_t *pitch_mdeg,
                                              int32_t *roll_mdeg);

/*
 * Compute normalized gravity direction in sensor frame from quaternion.
 * g = R(q)^T * [0, 0, 1].
 * Returns SENSOR_FAULT if quaternion is non-finite.
 */
motion_result_t motion_gravity_from_quat(const motion_quat_t *q, float g_out[3]);

/* Dot product of two 3-vectors. */
float motion_vec3_dot(const float a[3], const float b[3]);

/* ---- Air mouse ------------------------------------------------------- */

#define MOTION_AIR_MOUSE_DEADZONE_DPS  0.8f
#define MOTION_AIR_MOUSE_SENS_BASE     8.0f    /* px/degree at low rates */
#define MOTION_AIR_MOUSE_SENS_HIGH     14.0f   /* px/degree at high rates */
#define MOTION_AIR_MOUSE_RAMP_START    120.0f  /* dps: ramp begins */
#define MOTION_AIR_MOUSE_RAMP_END      240.0f  /* dps: full high sensitivity */

typedef struct {
	float carry_h;   /* fractional pixel carry, horizontal */
	float carry_v;   /* fractional pixel carry, vertical */
	int16_t dx;      /* last horizontal output delta */
	int16_t dy;      /* last vertical output delta */
	bool enabled;
} motion_air_mouse_state_t;

void motion_air_mouse_init(motion_air_mouse_state_t *s);

/* Enable + recenter (dx=dy=0, carry=0). */
void motion_air_mouse_enable(motion_air_mouse_state_t *s);

void motion_air_mouse_disable(motion_air_mouse_state_t *s);

/*
 * Compute sensitivity (px/degree) for a given angular-rate magnitude (dps).
 * Smoothstep ramp from RAMP_START to RAMP_END. Exported for unit testing.
 */
float motion_air_mouse_sensitivity(float rate_magnitude_dps);

/*
 * Air mouse integration step.
 *   rate_h_dps: angular rate for horizontal axis (yaw) in dps
 *   rate_v_dps: angular rate for vertical axis (pitch) in dps
 *   dt: measured seconds since last update
 * Output in s->dx, s->dy (int16, clamped).
 * Returns SENSOR_FAULT if rates or dt are NaN/Inf.
 */
motion_result_t motion_air_mouse_update(motion_air_mouse_state_t *s,
                                        float rate_h_dps,
                                        float rate_v_dps,
                                        float dt);

/* ---- Tilt-to-D-pad --------------------------------------------------- */

#define MOTION_TILT_ENGAGE_DEG    25.0f
#define MOTION_TILT_RELEASE_DEG   17.0f

typedef enum {
	MOTION_TILT_DIR_NONE  = 0,
	MOTION_TILT_DIR_UP    = 1,
	MOTION_TILT_DIR_DOWN  = 2,
	MOTION_TILT_DIR_LEFT  = 3,
	MOTION_TILT_DIR_RIGHT = 4,
} motion_tilt_dir_t;

typedef struct {
	motion_tilt_dir_t current;  /* currently engaged direction */
} motion_tilt_state_t;

typedef struct {
	motion_tilt_dir_t pressed;   /* newly pressed this update (NONE if none) */
	motion_tilt_dir_t released;  /* newly released this update (NONE if none) */
} motion_tilt_event_t;

void motion_tilt_init(motion_tilt_state_t *s);

/*
 * Tilt navigation update with hysteresis.
 *   pitch_deg: device pitch angle (positive = tilted up)
 *   roll_deg:  device roll angle (positive = tilted right)
 * At most one direction engaged at a time. Release always paired.
 * Engages at 25 degrees, releases at 17 degrees.
 */
void motion_tilt_update(motion_tilt_state_t *s,
                        float pitch_deg, float roll_deg,
                        motion_tilt_event_t *event);

#ifdef __cplusplus
}
#endif
#endif /* MOTION_EVAL_H */
