/*
 * fusion.cpp - Madgwick AHRS fusion firmware wrapper.
 *
 * Converts sensor units (mg/mdps/milligauss) to internal float units
 * and delegates all math to motion_eval.c. The dt is measured from
 * timestamps — NOT assumed to be exactly 1/104s.
 */
#include "fusion.h"

Fusion::Fusion()
	: m_lastImuUs(0)
	, m_firstUpdate(true)
	, m_magPresent(false)
	, m_magCalibrated(false)
{
	motion_fusion_init(&m_state);
}

void Fusion::init() {
	motion_fusion_init(&m_state);
	m_lastImuUs = 0;
	m_firstUpdate = true;
}

void Fusion::setMagAvailable(bool present, bool calibrated) {
	m_magPresent = present;
	m_magCalibrated = calibrated;
}

void Fusion::updateMag(const mag_sample_t *mag, bool healthy, uint64_t now_us) {
	if (mag != nullptr) {
		m_state.last_mx = motion_milligauss_to_gauss(mag->x_mg);
		m_state.last_my = motion_milligauss_to_gauss(mag->y_mg);
		m_state.last_mz = motion_milligauss_to_gauss(mag->z_mg);
		m_state.last_mag_us = now_us;
	}
	motion_mag_health_eval(&m_state, m_magPresent, m_magCalibrated,
	                       healthy, now_us);
	/* Set magnetic reference on first healthy 9DoF reading */
	if (m_state.mode == MOTION_FUSION_MODE_9DOF && healthy && !m_state.ref_set
	    && mag != nullptr) {
		float mx = motion_milligauss_to_gauss(mag->x_mg);
		float my = motion_milligauss_to_gauss(mag->y_mg);
		float mz = motion_milligauss_to_gauss(mag->z_mg);
		motion_fusion_set_mag_reference(&m_state, mx, my, mz);
	}
}

motion_result_t Fusion::updateImu(const imu_sample_t *imu, uint64_t now_us) {
	if (imu == nullptr)
		return MOTION_RESULT_SENSOR_FAULT;

	float gx = motion_mdps_to_rads(imu->gyro_x_mdps);
	float gy = motion_mdps_to_rads(imu->gyro_y_mdps);
	float gz = motion_mdps_to_rads(imu->gyro_z_mdps);
	float ax = motion_mg_to_g(imu->accel_x_mg);
	float ay = motion_mg_to_g(imu->accel_y_mg);
	float az = motion_mg_to_g(imu->accel_z_mg);

	float dt;
	if (m_firstUpdate) {
		dt = MOTION_IMU_DT_NOMINAL_S;
		m_firstUpdate = false;
	} else {
		uint64_t delta_us = now_us - m_lastImuUs;
		dt = (float)(double)delta_us * 0.000001;  /* us → s via double */
	}
	m_lastImuUs = now_us;

	bool mag_valid = (m_state.mode == MOTION_FUSION_MODE_9DOF)
	                 && m_state.mag_healthy && m_state.ref_set;
	return motion_fusion_update(&m_state, gx, gy, gz, ax, ay, az,
	                            m_state.last_mx, m_state.last_my, m_state.last_mz,
	                            mag_valid, now_us, dt);
}

const motion_quat_t *Fusion::getQuaternion() const {
	return &m_state.q;
}

motion_result_t Fusion::getQuaternionQ30(int32_t out[4]) const {
	return motion_quat_to_q30(&m_state.q, out);
}

motion_result_t Fusion::getEulerMillideg(int32_t *yaw, int32_t *pitch,
                                         int32_t *roll) const {
	return motion_quat_to_euler_millideg(&m_state.q, yaw, pitch, roll);
}
