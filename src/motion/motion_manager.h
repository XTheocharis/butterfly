/*
 * motion_manager.h - Motion subsystem coordinator (BOARD_CLUE).
 *
 * Owns the Fusion / AirMouse / TiltNav / GesturePrimitive / RotationGesture
 * instances and tracks the latest samples from each motion-relevant sensor.
 *
 * Inputs are injected by the BoardModule:
 *   - feedImu()        — async i2cBus completion delivers calibrated IMU
 *   - feedMag()        — async i2cBus completion delivers calibrated mag
 *   - feedApdsGesture()— async APDS-9960 FIFO decode result
 *   - feedButtons()    — debounced button state from main loop
 *
 * Outputs are read by BoardModule handlers:
 *   - readSensor(id, ...) — fills latest value for handleReadSensor / streams
 *   - tick(now_us)        — drives rotation gesture FSM, APDS dispatch,
 *                            and stream sample emission pacing
 *   - startCalibration() / tickCalibration() — async calibration driver
 *
 * Designed so that host tests can fully exercise the logic by injecting
 * mock sensor data through the same feed* API used by firmware.
 */
#ifndef MOTION_MANAGER_H
#define MOTION_MANAGER_H

#include "motion_eval.h"
#include "fusion.h"
#include "air_mouse.h"
#include "tilt.h"
#include "gesture.h"
#include "rotation_gesture.h"

#include "../sensors/imu.h"
#include "../sensors/mag.h"
#include "../sensors/apds9960.h"

#ifdef BOARD_CLUE

#ifdef __cplusplus

#include <stdint.h>

class MotionManager {
public:
	MotionManager();

	void init();

	/* === Sensor feed (called by BoardModule on i2cBus completion) === */

	/* Feed a calibrated IMU sample. Runs fusion update at 104Hz.
	 * now_us is the sample timestamp (not the call timestamp). */
	void feedImu(const imu_sample_t *imu, uint64_t now_us);

	/* Feed a calibrated mag sample. Updates fusion health state.
	 * healthy=true if field-health check passed. */
	void feedMag(const mag_sample_t *mag, bool healthy, uint64_t now_us);

	/* Feed the latest APDS-9960 decoded gesture. Cached and consumed
	 * once by the next dispatchInput() call. */
	void feedApdsGesture(apds9960_gesture_t g);

	/* Configure mag availability (presence + calibration flags). */
	void setMagAvailable(bool present, bool calibrated);

	/* Feed debounced button state. */
	void feedButtons(bool button_a, bool button_b);

	/* === Periodic tick (called from Core::loop) === */

	/* Drives rotation-gesture FSM, APDS dispatch hook, and tilt events.
	 * Returns the event emitted by the rotation gesture, if any. */
	rotg_event_t tick(uint64_t now_us);

	/* === Outputs === */

	/* Status flag for board_SensorSample.status */
	enum SampleStatus {
		STALE = 0,
		FRESH = 1,
	};

	/* Read up to 10 int32 values for the given sensor ID.
	 * Returns the number of values written (0 if sensor not owned
	 * by MotionManager, e.g. environment sensors).
	 * Sets *out_status to FRESH if a sample has been fed for this
	 * sensor since the last read, STALE otherwise. */
	uint32_t readSensor(uint32_t sensor_id,
	                    int32_t out_values[10],
	                    SampleStatus *out_status);

	/* Read latest decoded APDS gesture (consumed: clears after read). */
	apds9960_gesture_t consumeApdsGesture();

	/* Latest button state. */
	bool getButtonA() const { return m_buttonA; }
	bool getButtonB() const { return m_buttonB; }

	/* === Calibration driver (async) === */

	enum CalibTarget {
		CALIB_NONE  = 0,
		CALIB_IMU   = 1,
		CALIB_MAG   = 2,
	};

	/* Begin calibration for the given target. Returns false if a
	 * calibration is already in progress. */
	bool startCalibration(CalibTarget target);

	/* Advance calibration one step. Returns true if calibration
	 * just completed (caller emits the terminal event).
	 *   imu_sample/mag_sample : latest sample (NULL if not yet available)
	 *   out_progress_pct      : 0..100 progress for periodic events */
	enum CalibResult {
		CALIB_RESULT_CONTINUE = 0,
		CALIB_RESULT_DONE_OK  = 1,
		CALIB_RESULT_FAILED   = 2,
	};
	CalibResult tickCalibration(const imu_sample_t *imu_sample,
	                            const mag_sample_t *mag_sample,
	                            uint8_t *out_progress_pct);

	bool isCalibrationBusy() const { return m_calibTarget != CALIB_NONE; }
	CalibTarget getCalibrationTarget() const { return m_calibTarget; }

	/* Access owned subsystems (read-only). */
	const Fusion &fusion() const { return m_fusion; }
	const AirMouse &airMouse() const { return m_airMouse; }
	const TiltNav &tilt() const { return m_tilt; }
	const RotationGesture &rotationGesture() const { return m_rotation; }

	/* Wire rotation-gesture switch callback. */
	void setRotationSwitchCallback(rotg_switch_fn fn, void *user) {
		m_rotation.setSwitchCallback(fn, user);
	}

private:
	Fusion          m_fusion;
	AirMouse        m_airMouse;
	TiltNav         m_tilt;
	GesturePrimitive m_gravity;
	RotationGesture m_rotation;

	/* Latest cached samples */
	imu_sample_t     m_lastImu;
	mag_sample_t     m_lastMag;
	bool             m_imuFresh;
	bool             m_magFresh;

	bool             m_buttonA;
	bool             m_buttonB;

	apds9960_gesture_t m_pendingApds;
	bool               m_apdsPending;

	/* Calibration driver */
	CalibTarget     m_calibTarget;
	imu_calib_t     m_imuCalib;
	mag_calib_t     m_magCalib;

	uint64_t        m_lastImuUs;       /* for air-mouse dt */
};

#endif /* __cplusplus */
#endif /* BOARD_CLUE */
#endif /* MOTION_MANAGER_H */
