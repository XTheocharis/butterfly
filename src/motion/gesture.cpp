/*
 * gesture.cpp - Runtime gesture detection primitive firmware wrapper.
 */
#include "gesture.h"

GesturePrimitive::GesturePrimitive()
	: m_ref{0.0f, 0.0f, 1.0f}
	, m_hasRef(false)
{
}

motion_result_t GesturePrimitive::computeGravity(const motion_quat_t *q,
                                                  float g_out[3]) {
	return motion_gravity_from_quat(q, g_out);
}

float GesturePrimitive::dotWithReference(const float current[3],
                                         const float reference[3]) {
	return motion_vec3_dot(current, reference);
}

void GesturePrimitive::storeReference(const float g[3]) {
	m_ref[0] = g[0];
	m_ref[1] = g[1];
	m_ref[2] = g[2];
	m_hasRef = true;
}
