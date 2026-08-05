/*
 * gatt_eval.c - Pure-C GATT evaluation implementations.
 *
 * No SDK dependencies. Compiled on both host (tests) and device (firmware).
 */
#include "gatt_eval.h"

/* ---- Connection-handle predicate -------------------------------------- */

bool gatt_eval_is_connected(uint16_t conn_handle) {
	return conn_handle != GATT_CONN_HANDLE_INVALID;
}

/* ---- PPCP builder ----------------------------------------------------- */

void gatt_eval_build_ppcp_params(uint16_t *min, uint16_t *max,
                                 uint16_t *latency, uint16_t *timeout) {
	if (min != 0) {
		*min = ble_gap_interval_from_ms(BLE_CONN_MIN_INTERVAL_MS);
	}
	if (max != 0) {
		*max = ble_gap_interval_from_ms(BLE_CONN_MAX_INTERVAL_MS);
	}
	if (latency != 0) {
		*latency = BLE_LATENCY_ACTIVE;
	}
	if (timeout != 0) {
		*timeout = ble_sup_timeout_from_ms(BLE_CONN_TIMEOUT_MS);
	}
}

/* ---- PPCP validator --------------------------------------------------- */

/* BLE spec ranges (Vol 3, Part C, Section 12.3 / 12.4):
 *   interval: 7.5 ms - 4000 ms  (GAP units 6 - 3200)
 *   latency:  0 - 499 connection events
 *   timeout:  100 ms - 32000 ms  (units 10 - 3200) */
#define GATT_PPCP_MIN_INTERVAL_MS     8u    /* ceil(7.5) as integer ms */
#define GATT_PPCP_MAX_INTERVAL_MS     4000u
#define GATT_PPCP_MAX_LATENCY         499u
#define GATT_PPCP_MIN_TIMEOUT_MS      100u
#define GATT_PPCP_MAX_TIMEOUT_MS      32000u

bool gatt_eval_validate_ppcp(uint16_t min_u, uint16_t max_u,
                             uint16_t latency, uint16_t timeout_u) {
	/* Range checks. min/max are in ms; BLE interval min is 7.5 but
	 * integer ms input can only be >= 8 (ceil). */
	if (min_u < GATT_PPCP_MIN_INTERVAL_MS || min_u > GATT_PPCP_MAX_INTERVAL_MS) {
		return false;
	}
	if (max_u < GATT_PPCP_MIN_INTERVAL_MS || max_u > GATT_PPCP_MAX_INTERVAL_MS) {
		return false;
	}
	if (latency > GATT_PPCP_MAX_LATENCY) {
		return false;
	}
	if (timeout_u < GATT_PPCP_MIN_TIMEOUT_MS || timeout_u > GATT_PPCP_MAX_TIMEOUT_MS) {
		return false;
	}

	/* Ordering: min <= max. */
	if (min_u > max_u) {
		return false;
	}

	/* BLE Vol 3 Part C Section 12.5: supervision timeout must be greater
	 * than (1 + latency) * interval_max * 2 (in ms). Use integer math. */
	uint32_t required_timeout_ms = (uint32_t)(1u + latency) *
	                               (uint32_t)max_u * 2u;
	if (timeout_u <= required_timeout_ms) {
		return false;
	}

	return true;
}
