/*
 * security_eval.c - Pure-C security evaluation implementations.
 *
 * No SDK dependencies. Compiled on both host (tests) and device (firmware).
 */
#include "security_eval.h"

/* Absolute BLE minimum key size (7 octets = 56 bits). */
#define SEC_EVAL_ABSOLUTE_MIN_KEY_SIZE  7u

bool security_eval_validate_sec_params(uint8_t bond, uint8_t mitm,
                                       uint8_t lesc, uint8_t io_caps,
                                       uint8_t min_key_size,
                                       uint8_t max_key_size) {
	/* bond is either 0 or 1 — no constraint beyond type width. */

	/* LESC requires 16-byte key (BLE_SEC_MIN_KEY_SIZE). */
	if (lesc && min_key_size < BLE_SEC_MIN_KEY_SIZE) {
		return false;
	}

	/* Absolute BLE floor: key must be at least 7 octets. */
	if (min_key_size < SEC_EVAL_ABSOLUTE_MIN_KEY_SIZE) {
		return false;
	}

	/* Range: min <= max. */
	if (min_key_size > max_key_size) {
		return false;
	}

	/* MITM requires an I/O capability (NONE = no MITM possible). */
	if (mitm && io_caps == BLE_SEC_IO_CAPS) {
		return false;
	}

	return true;
}
