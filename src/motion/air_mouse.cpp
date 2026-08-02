/*
 * air_mouse.cpp - Air mouse pointer integration firmware wrapper.
 */
#include "air_mouse.h"

AirMouse::AirMouse() {
	motion_air_mouse_init(&m_state);
}

void AirMouse::init() {
	motion_air_mouse_init(&m_state);
}

void AirMouse::enable() {
	motion_air_mouse_enable(&m_state);
}

void AirMouse::disable() {
	motion_air_mouse_disable(&m_state);
}

motion_result_t AirMouse::update(int32_t gyro_x_mdps, int32_t gyro_z_mdps,
                                 float dt) {
	/* Pitch rate (gyro X) → vertical; yaw rate (gyro Z) → horizontal */
	float rate_v_dps = (float)gyro_x_mdps * 0.001f;  /* mdps → dps */
	float rate_h_dps = (float)gyro_z_mdps * 0.001f;
	return motion_air_mouse_update(&m_state, rate_h_dps, rate_v_dps, dt);
}
