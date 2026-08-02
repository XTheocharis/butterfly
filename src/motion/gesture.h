/*
 * gesture.h - Runtime gesture detection primitive firmware wrapper.
 *
 * Provides normalized-gravity / dot-product primitives that Todo 23
 * uses to detect deliberate device orientation gestures (flip, twist,
 * shake) for runtime-mode switching and HID consumer-key actions.
 */
#ifndef MOTION_GESTURE_H
#define MOTION_GESTURE_H

#include "motion_eval.h"

#ifdef __cplusplus

class GesturePrimitive {
public:
	GesturePrimitive();

	/* Compute current gravity direction from quaternion.
	 * Gravity in body frame: R(q)^T * [0, 0, 1]. */
	motion_result_t computeGravity(const motion_quat_t *q, float g_out[3]);

	/* Compare current gravity against a stored reference direction.
	 * Returns dot product (1.0 = same direction, -1.0 = opposite).
	 * Used by Todo 23 for flip detection. */
	float dotWithReference(const float current[3], const float reference[3]);

	/* Store the current gravity as the reference for future comparisons. */
	void storeReference(const float g[3]);

	const float *getReference() const { return m_ref; }
	bool hasReference() const { return m_hasRef; }

private:
	float m_ref[3];
	bool m_hasRef;
};

#endif /* __cplusplus */
#endif /* MOTION_GESTURE_H */
