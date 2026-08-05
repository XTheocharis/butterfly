/*
 * security_eval.h - Pure-C BLE security evaluation layer (host-testable, no SDK deps).
 *
 * Contains the security-parameter validator using the BLE_SEC_* constants
 * already defined in ble_eval.h. The firmware C++ class security.cpp wraps
 * these evaluations with SDK Peer Manager calls.
 *
 * Compiled on BOTH host (for unit tests) and device (linked into firmware).
 */
#ifndef SECURITY_EVAL_H
#define SECURITY_EVAL_H

#include <stdint.h>
#include <stdbool.h>

#include "ble_eval.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Key-distribution constants (from security.cpp:76-77) ------------ */

/* LTK/EDIV/RAND distribution: enc=1, all other keys=0.
 * Mirrors security.cpp kdist_own.enc / kdist_peer.enc. */
#define SEC_EVAL_KDIST_OWN_ENC    1u
#define SEC_EVAL_KDIST_PEER_ENC   1u

/* ---- Security parameter validator ------------------------------------- */

/* Validate a security-parameters set against LESC Just Works constraints.
 * Uses the BLE_SEC_* constants from ble_eval.h for policy checks.
 *
 * Rules (returns false on any violation):
 *   - lesc && min_key_size < BLE_SEC_MIN_KEY_SIZE (LESC requires 16-byte key)
 *   - min_key_size > max_key_size
 *   - mitm && io_caps == BLE_GAP_IO_CAPS_NONE (MITM needs I/O capability)
 *   - min_key_size < 7  (absolute BLE minimum)
 *
 * NOTE: Does NOT model a security state machine. security.cpp uses a simple
 * bool m_secured — this validator only checks parameter consistency. */
bool security_eval_validate_sec_params(uint8_t bond, uint8_t mitm,
                                       uint8_t lesc, uint8_t io_caps,
                                       uint8_t min_key_size,
                                       uint8_t max_key_size);

#ifdef __cplusplus
}
#endif

#endif /* SECURITY_EVAL_H */
