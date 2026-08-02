/*
 * air_mouse.h - Air mouse pointer integration firmware wrapper.
 *
 * Wraps motion_eval air mouse logic with gyro input from IMU.
 * Output dx/dy feed into HIDS mouse reports (Todo 21).
 *
 * Nominal 60Hz update rate. Yaw (gyro Z) → horizontal, pitch (gyro X) → vertical.
 */
#ifndef MOTION_AIR_MOUSE_H
#define MOTION_AIR_MOUSE_H

#include "motion_eval.h"
#include "../sensors/imu.h"

#ifdef __cplusplus

class AirMouse {
public:
	AirMouse();

	void init();
	void enable();   /* recenter + activate */
	void disable();

	/* Update with gyro rates in mdps. Converts to dps internally.
	 * dt is measured seconds since last call. */
	motion_result_t update(int32_t gyro_x_mdps, int32_t gyro_z_mdps,
	                       float dt);

	int16_t getDx() const { return m_state.dx; }
	int16_t getDy() const { return m_state.dy; }
	bool isEnabled() const { return m_state.enabled; }

private:
	motion_air_mouse_state_t m_state;
};

#endif /* __cplusplus */
#endif /* MOTION_AIR_MOUSE_H */
