/*
 * motion_manager.cpp - Motion subsystem coordinator implementation.
 *
 * Aggregates the per-domain motion wrappers (Fusion, AirMouse, TiltNav,
 * GesturePrimitive, RotationGesture) and exposes a single feed/tick/read
 * surface to BoardModule. All sensor data is injected — this file never
 * performs I2C transfers directly.
 */
#include "motion_manager.h"
#include <string.h>
#include "boardMotionEval.h"

#ifdef BOARD_CLUE

MotionManager::MotionManager()
	: m_imuFresh(false)
	, m_magFresh(false)
	, m_buttonA(false)
	, m_buttonB(false)
	, m_pendingApds(APDS9960_GESTURE_NONE)
	, m_apdsPending(false)
	, m_calibTarget(CALIB_NONE)
	, m_lastImuUs(0)
{
	memset(&m_lastImu, 0, sizeof(m_lastImu));
	memset(&m_lastMag, 0, sizeof(m_lastMag));
}

void MotionManager::init() {
	m_fusion.init();
	m_airMouse.init();
	m_tilt.init();
	m_rotation.init();
	m_imuFresh = false;
	m_magFresh = false;
	m_buttonA = false;
	m_buttonB = false;
	m_pendingApds = APDS9960_GESTURE_NONE;
	m_apdsPending = false;
	m_calibTarget = CALIB_NONE;
	m_lastImuUs = 0;
}

void MotionManager::feedImu(const imu_sample_t *imu, uint64_t now_us) {
	if (imu == nullptr) return;

	/* Drive fusion (consumes accel/gyro). */
	(void)m_fusion.updateImu(imu, now_us);

	/* Drive air-mouse integration. dt derived from sample timestamps;
	 * fall back to nominal 104Hz dt on the first sample. */
	float dt;
	if (m_lastImuUs == 0) {
		dt = MOTION_IMU_DT_NOMINAL_S;
	} else {
		uint64_t delta_us = now_us - m_lastImuUs;
		dt = (float)(double)delta_us * 0.000001f;
	}
	m_lastImuUs = now_us;
	(void)m_airMouse.update(imu->gyro_x_mdps, imu->gyro_z_mdps, dt);

	/* Feed active IMU calibration. */
	if (m_calibTarget == CALIB_IMU) {
		(void)imu_calib_feed(&m_imuCalib, imu);
	}

	m_lastImu = *imu;
	m_imuFresh = true;
}

void MotionManager::feedMag(const mag_sample_t *mag, bool healthy,
                            uint64_t now_us) {
	if (mag == nullptr) return;

	m_fusion.updateMag(mag, healthy, now_us);

	if (m_calibTarget == CALIB_MAG) {
		(void)mag_calib_feed(&m_magCalib, mag);
	}

	m_lastMag = *mag;
	m_magFresh = true;
}

void MotionManager::feedApdsGesture(apds9960_gesture_t g) {
	m_pendingApds = g;
	m_apdsPending = true;
}

void MotionManager::setMagAvailable(bool present, bool calibrated) {
	m_fusion.setMagAvailable(present, calibrated);
}

void MotionManager::feedButtons(bool button_a, bool button_b) {
	m_buttonA = button_a;
	m_buttonB = button_b;
}

rotg_event_t MotionManager::tick(uint64_t now_us) {
	/* Rotation gesture FSM. Use latest fusion quaternion. */
	const motion_quat_t *q = m_fusion.getQuaternion();
	rotg_event_t evt = m_rotation.tick(m_buttonA, m_buttonB, q, now_us);

	/* Drive tilt nav from the same quaternion (used for D-pad mapping).
	 * The tilt event is dispatched by BoardModule via ProfileManager
	 * when wired; here we just advance the FSM. */
	if (q != nullptr) {
		motion_tilt_event_t tilt_evt;
		m_tilt.update(q, &tilt_evt);
		(void)tilt_evt;
	}

	return evt;
}

uint32_t MotionManager::readSensor(uint32_t sensor_id,
                                   int32_t out_values[10],
                                   SampleStatus *out_status)
{
	if (out_status != nullptr) *out_status = STALE;

	switch (sensor_id) {

	case IMU_SENSOR_ID_ACCEL: /* 1 */
		out_values[0] = m_lastImu.accel_x_mg;
		out_values[1] = m_lastImu.accel_y_mg;
		out_values[2] = m_lastImu.accel_z_mg;
		if (m_imuFresh && out_status != nullptr) {
			*out_status = FRESH;
			m_imuFresh = false;
		}
		return 3;

	case IMU_SENSOR_ID_GYRO: /* 2 */
		out_values[0] = m_lastImu.gyro_x_mdps;
		out_values[1] = m_lastImu.gyro_y_mdps;
		out_values[2] = m_lastImu.gyro_z_mdps;
		if (m_imuFresh && out_status != nullptr) {
			*out_status = FRESH;
			m_imuFresh = false;
		}
		return 3;

	case MAG_SENSOR_ID: /* 3 */
		out_values[0] = m_lastMag.x_mg;
		out_values[1] = m_lastMag.y_mg;
		out_values[2] = m_lastMag.z_mg;
		if (m_magFresh && out_status != nullptr) {
			*out_status = FRESH;
			m_magFresh = false;
		}
		return 3;

	case BOARD_MOTION_SENSOR_QUATERNION: { /* 4 */
		if (m_fusion.getQuaternionQ30(out_values) != MOTION_RESULT_OK) {
			return 0;
		}
		if (out_status != nullptr) *out_status = FRESH;
		return 4;
	}

	case BOARD_MOTION_SENSOR_ORIENTATION: { /* 5 */
		int32_t yaw, pitch, roll;
		if (m_fusion.getEulerMillideg(&yaw, &pitch, &roll) != MOTION_RESULT_OK) {
			return 0;
		}
		out_values[0] = yaw;
		out_values[1] = pitch;
		out_values[2] = roll;
		if (out_status != nullptr) *out_status = FRESH;
		return 3;
	}

	case BOARD_MOTION_SENSOR_AIR_MOUSE: { /* 14 */
		out_values[0] = m_airMouse.getDx();
		out_values[1] = m_airMouse.getDy();
		out_values[2] = 0; /* wheel */
		out_values[3] = 0; /* buttons — owned by BoardModule state */
		if (out_status != nullptr) *out_status = FRESH;
		return 4;
	}

	default:
		/* Environment / color / proximity / gesture / audio sensors are
		 * not owned by MotionManager — caller returns STALE. */
		return 0;
	}
}

apds9960_gesture_t MotionManager::consumeApdsGesture() {
	apds9960_gesture_t g = m_pendingApds;
	m_pendingApds = APDS9960_GESTURE_NONE;
	m_apdsPending = false;
	return g;
}

bool MotionManager::startCalibration(CalibTarget target) {
	if (m_calibTarget != CALIB_NONE) return false;
	if (target == CALIB_NONE) return false;

	m_calibTarget = target;
	if (target == CALIB_IMU) {
		imu_calib_init(&m_imuCalib);
	} else if (target == CALIB_MAG) {
		mag_calib_init(&m_magCalib);
	}
	return true;
}

MotionManager::CalibResult MotionManager::tickCalibration(
    const imu_sample_t *imu_sample,
    const mag_sample_t *mag_sample,
    uint8_t *out_progress_pct)
{
	if (out_progress_pct != nullptr) *out_progress_pct = 0;
	if (m_calibTarget == CALIB_NONE) {
		return CALIB_RESULT_DONE_OK;
	}

	if (m_calibTarget == CALIB_IMU) {
		if (imu_sample == nullptr) return CALIB_RESULT_CONTINUE;
		imu_calib_state_t s = imu_calib_feed(&m_imuCalib, imu_sample);
		if (out_progress_pct != nullptr) {
			*out_progress_pct = (uint8_t)(
			    (uint32_t)m_imuCalib.sample_count * 100u /
			    IMU_CALIB_SAMPLE_COUNT);
		}
		if (s == IMU_CALIB_DONE) {
			m_calibTarget = CALIB_NONE;
			return CALIB_RESULT_DONE_OK;
		}
		if (!m_imuCalib.stationary) {
			/* Keep collecting — stationary flag only blocks result
			 * validity, not collection. Caller decides whether to
			 * reject non-stationary results. */
		}
		return CALIB_RESULT_CONTINUE;
	}

	if (m_calibTarget == CALIB_MAG) {
		if (mag_sample == nullptr) return CALIB_RESULT_CONTINUE;
		mag_calib_state_t s = mag_calib_feed(&m_magCalib, mag_sample);
		if (out_progress_pct != nullptr) {
			*out_progress_pct = (uint8_t)(
			    (uint32_t)m_magCalib.sample_count * 100u /
			    MAG_CALIB_SAMPLE_COUNT);
		}
		if (s == MAG_CALIB_DONE) {
			m_calibTarget = CALIB_NONE;
			return CALIB_RESULT_DONE_OK;
		}
		return CALIB_RESULT_CONTINUE;
	}

	return CALIB_RESULT_CONTINUE;
}

#endif /* BOARD_CLUE */
