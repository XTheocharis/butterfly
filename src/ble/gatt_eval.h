/*
 * gatt_eval.h - Pure-C GATT server evaluation layer (host-testable, no SDK deps).
 *
 * Contains GATT-specific constants and validators NOT already in ble_eval.h:
 *   - HID Generic Remote appearance value (962 per HID over GATT profile spec)
 *   - Connection-handle invalid sentinel (mirrors SDK BLE_CONN_HANDLE_INVALID)
 *   - PPCP parameter builder + range validator
 *
 * The firmware C++ wrapper gatt.cpp wraps these evaluations with SDK
 * sd_ble_gap_* calls. Compiled on BOTH host (tests) and device (firmware).
 */
#ifndef GATT_EVAL_H
#define GATT_EVAL_H

#include <stdint.h>
#include <stdbool.h>

#include "ble_eval.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- GATT constants (not in ble_eval.h) ------------------------------ */

/* HID Generic Remote appearance (per HID over GATT profile spec).
 * Mirrors gatt.cpp BLE_APPEARANCE_HID_REMOTE. */
#define GATT_APPEARANCE_HID_REMOTE   962u

/* Invalid connection handle sentinel (mirrors SDK BLE_CONN_HANDLE_INVALID).
 * Used by gatt.cpp m_connHandle initial value. */
#define GATT_CONN_HANDLE_INVALID     0xFFFFu

/* ---- Connection-handle predicate -------------------------------------- */

/* Returns true if conn_handle represents an active connection.
 * Mirrors GattServer::isConnected() logic. */
bool gatt_eval_is_connected(uint16_t conn_handle);

/* ---- PPCP builder ----------------------------------------------------- */

/* Populate GAP PPCP fields from ble_eval.h conversion functions.
 * Caller passes pointers to all four ble_gap_conn_params_t fields.
 * Values: min=8ms (GAP unit 6), max=15ms (GAP unit 12),
 *         latency=BLE_LATENCY_ACTIVE (0), timeout=4s (unit 400). */
void gatt_eval_build_ppcp_params(uint16_t *min, uint16_t *max,
                                 uint16_t *latency, uint16_t *timeout);

/* ---- PPCP validator --------------------------------------------------- */

/* Validate PPCP fields per BLE spec constraints.
 *   min_u, max_u in ms (will be range-checked against [7.5, 4000] ms).
 *   latency in number of connection events [0, 499].
 *   timeout_u in ms [100, 32000].
 * Returns false if:
 *   - min > max
 *   - timeout < (1 + latency) * max * 2  (BLE Vol 3, Part C, Section 12.5)
 *   - any value outside its allowed range */
bool gatt_eval_validate_ppcp(uint16_t min_u, uint16_t max_u,
                             uint16_t latency, uint16_t timeout_u);

#ifdef __cplusplus
}
#endif

#endif /* GATT_EVAL_H */
