/*
 * fusion.h - Madgwick AHRS fusion firmware wrapper (BOARD_CLUE only).
 *
 * Wraps motion_eval pure-C fusion with i2cBus sensor input. Consumes
 * calibrated imu_sample_t (mg/mdps) and mag_sample_t (milligauss),
 * outputs quaternion in Q30 fixed-point (sensor ID 4) and Euler angles
 * in millidegrees (sensor ID 5).
 */
#ifndef MOTION_FUSION_H
#define MOTION_FUSION_H

#include "motion_eval.h"
#include "../sensors/imu.h"
#include "../sensors/mag.h"

#ifdef __cplusplus

class Fusion {
public:
	Fusion();

	void init();

	/* Feed one IMU sample at 104Hz. dt is computed from timestamps.
	 * Returns SENSOR_FAULT on NaN/Inf input. */
	motion_result_t updateImu(const imu_sample_t *imu, uint64_t now_us);

	/* Feed one mag sample at 40Hz. Updates health state and reference. */
	void updateMag(const mag_sample_t *mag, bool healthy, uint64_t now_us);

	/* Configure magnetometer availability (from i2cBus probe + calib). */
	void setMagAvailable(bool present, bool calibrated);

	/* Outputs */
	const motion_quat_t *getQuaternion() const;
	motion_fusion_mode_t getMode() const { return m_state.mode; }

	/* Fill Q30 quaternion array (sensor ID 4 output). */
	motion_result_t getQuaternionQ30(int32_t out[4]) const;

	/* Fill Euler angles in millidegrees (sensor ID 5 output). */
	motion_result_t getEulerMillideg(int32_t *yaw, int32_t *pitch, int32_t *roll) const;

private:
	motion_fusion_state_t m_state;
	uint64_t m_lastImuUs;
	bool m_firstUpdate;
	bool m_magPresent;
	bool m_magCalibrated;
};

#endif /* __cplusplus */
#endif /* MOTION_FUSION_H */
