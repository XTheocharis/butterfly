/*
 * rotation_gesture.cpp - Runtime rotation gesture firmware wrapper.
 */
#include "rotation_gesture.h"
#include "gesture.h"

RotationGesture::RotationGesture()
	: m_switchFn(nullptr)
	, m_switchUser(nullptr)
{
	rotg_init(&m_sm);
}

void RotationGesture::init() {
	rotg_init(&m_sm);
	m_switchFn = nullptr;
	m_switchUser = nullptr;
}

void RotationGesture::setSwitchCallback(rotg_switch_fn fn, void *user) {
	m_switchFn = fn;
	m_switchUser = user;
}

rotg_event_t RotationGesture::tick(bool button_a, bool button_b,
                                   const motion_quat_t *q,
                                   uint64_t now_us)
{
	float gravity[3];
	const float *g_ptr = nullptr;

	if (q != nullptr) {
		/* Use the existing GesturePrimitive helper to compute body-frame
		 * gravity from the quaternion. We construct a transient
		 * primitive each call — it is stateless aside from the
		 * reference store, which we don't use here. */
		GesturePrimitive prim;
		if (prim.computeGravity(q, gravity) == MOTION_RESULT_OK) {
			g_ptr = gravity;
		}
	}

	rotg_event_t evt = ROTG_EVENT_NONE;
	rotg_tick(&m_sm, button_a, button_b, g_ptr, now_us, &evt);

	if (evt == ROTG_EVENT_SWITCH_CONFIRM && m_switchFn != nullptr) {
		m_switchFn(m_switchUser);
	}
	return evt;
}
