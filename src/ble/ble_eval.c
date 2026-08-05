/*
 * ble_eval.c - Pure-C BLE evaluation implementations.
 *
 * No SDK dependencies. Compiled on both host (tests) and device (firmware).
 */
#include "ble_eval.h"

/* ---- SD header validation -------------------------------------------- */

ble_sd_validation_t ble_validate_sd(const ble_sd_backend_t *backend) {
	if (backend == 0 || backend->read32 == 0) {
		return BLE_SD_MAGIC_MISMATCH;
	}

	/* Check magic FIRST — short-circuit before trusting later fields. */
	uint32_t magic = backend->read32(BLE_SD_MAGIC_ADDR);
	if (magic != BLE_SD_MAGIC_EXPECTED) {
		return BLE_SD_MAGIC_MISMATCH;
	}

	uint32_t size = backend->read32(BLE_SD_SIZE_ADDR);
	if (size != BLE_SD_SIZE_EXPECTED) {
		return BLE_SD_SIZE_MISMATCH;
	}

	uint32_t fwid = backend->read32(BLE_SD_FWID_ADDR);
	if (fwid != BLE_SD_FWID_EXPECTED) {
		return BLE_SD_FWID_MISMATCH;
	}

	return BLE_SD_VALID;
}

/* ---- RAM origin check ------------------------------------------------ */

ble_ram_check_t ble_check_ram_origin(uint32_t linker_ram_start,
                                     uint32_t sd_required_ram_start) {
	if (sd_required_ram_start > linker_ram_start) {
		return BLE_RAM_SURPLUS;
	}
	if (linker_ram_start < BLE_RAM_ORIGIN_BASELINE) {
		return BLE_RAM_INSUFFICIENT;
	}
	return BLE_RAM_OK;
}

/* ---- Interval unit conversions --------------------------------------- */

uint16_t ble_gap_interval_from_ms(uint32_t ms) {
	/* GAP conn interval: units of 1.25 ms = ms * 4 / 5 */
	return (uint16_t)((ms * 4u) / 5u);
}

uint16_t ble_adv_interval_from_ms(uint32_t ms) {
	/* Adv interval: units of 0.625 ms = ms * 8 / 5 */
	return (uint16_t)((ms * 8u) / 5u);
}

uint16_t ble_sup_timeout_from_ms(uint32_t ms) {
	/* Supervision timeout: units of 10 ms */
	return (uint16_t)(ms / 10u);
}

/* ---- Advertising FSM ------------------------------------------------- */

ble_adv_state_t ble_adv_fsm(ble_adv_state_t current,
                            ble_adv_event_t event,
                            bool has_bond) {
	switch (current) {
	case BLE_ADV_STATE_IDLE:
		if (event == BLE_ADV_EVT_START) {
			return BLE_ADV_STATE_FAST;
		}
		return current;

	case BLE_ADV_STATE_FAST:
		if (event == BLE_ADV_EVT_FAST_TIMEOUT) {
			return BLE_ADV_STATE_SLOW;
		}
		if (event == BLE_ADV_EVT_CONNECTED) {
			return BLE_ADV_STATE_STOPPED;
		}
		if (event == BLE_ADV_EVT_STOP) {
			return BLE_ADV_STATE_STOPPED;
		}
		return current;

	case BLE_ADV_STATE_SLOW:
		if (event == BLE_ADV_EVT_CONNECTED) {
			return BLE_ADV_STATE_STOPPED;
		}
		if (event == BLE_ADV_EVT_IDLE_ACTIVITY) {
			return BLE_ADV_STATE_FAST;
		}
		if (event == BLE_ADV_EVT_STOP) {
			return BLE_ADV_STATE_STOPPED;
		}
		return current;

	case BLE_ADV_STATE_DIRECTED:
		if (event == BLE_ADV_EVT_CONNECTED) {
			return BLE_ADV_STATE_STOPPED;
		}
		if (event == BLE_ADV_EVT_IDLE_ACTIVITY) {
			return BLE_ADV_STATE_FAST;
		}
		if (event == BLE_ADV_EVT_STOP) {
			return BLE_ADV_STATE_STOPPED;
		}
		return current;

	case BLE_ADV_STATE_STOPPED:
		if (event == BLE_ADV_EVT_DISCONNECTED) {
			if (has_bond) {
				return BLE_ADV_STATE_DIRECTED;
			}
			return BLE_ADV_STATE_FAST;
		}
		if (event == BLE_ADV_EVT_START) {
			return BLE_ADV_STATE_FAST;
		}
		return current;

	default:
		return current;
	}
}

/* ---- Latency FSM ----------------------------------------------------- */

ble_latency_state_t ble_latency_fsm(ble_latency_state_t current,
                                    ble_latency_event_t event) {
	switch (current) {
	case BLE_LAT_STATE_ACTIVE:
		if (event == BLE_LAT_EVT_IDLE_5S) {
			return BLE_LAT_STATE_IDLE;
		}
		if (event == BLE_LAT_EVT_HOST_REJECT) {
			return BLE_LAT_STATE_BACKOFF;
		}
		return current;

	case BLE_LAT_STATE_IDLE:
		if (event == BLE_LAT_EVT_ACTIVITY) {
			return BLE_LAT_STATE_ACTIVE;
		}
		if (event == BLE_LAT_EVT_HOST_REJECT) {
			return BLE_LAT_STATE_BACKOFF;
		}
		return current;

	case BLE_LAT_STATE_BACKOFF:
		if (event == BLE_LAT_EVT_ACTIVITY) {
			return BLE_LAT_STATE_ACTIVE;
		}
		if (event == BLE_LAT_EVT_BACKOFF_ELAPSED) {
			return BLE_LAT_STATE_IDLE;
		}
		return current;

	default:
		return current;
	}
}

uint16_t ble_latency_value(ble_latency_state_t state) {
	if (state == BLE_LAT_STATE_ACTIVE) {
		return BLE_LATENCY_ACTIVE;
	}
	/* IDLE and BACKOFF both request latency 4. */
	return BLE_LATENCY_IDLE;
}

/* ---- IRQ priority assertions ----------------------------------------- */

bool ble_irq_priority_is_sd_reserved(uint32_t priority) {
	return priority == BLE_IRQ_PRIO_SD_RESERVED;
}

bool ble_irq_priority_is_app_safe(uint32_t priority) {
	return priority >= 2u && priority <= BLE_IRQ_PRIO_APP_DEFAULT;
}

bool ble_ble_mode_raw_timer_check(bool timer3_started, bool timer4_started) {
	return !timer3_started && !timer4_started;
}

/* ---- Advertising-mode mapping + param-update classifier --------------- */

int ble_eval_state_to_adv_mode(ble_adv_state_t state) {
	switch (state) {
	case BLE_ADV_STATE_FAST:     return 1;
	case BLE_ADV_STATE_SLOW:     return 2;
	case BLE_ADV_STATE_DIRECTED: return 3;
	default:                     return 0;
	}
}

ble_param_update_result_t ble_eval_classify_param_update_error(uint32_t err_code,
                                                               bool connected) {
	if (err_code == BLE_EVAL_ERR_SUCCESS) {
		return BLE_EVAL_PARAM_UPDATE_OK;
	}
	if (!connected) {
		return BLE_EVAL_PARAM_UPDATE_NO_CONNECTION;
	}
	if (err_code == BLE_EVAL_ERR_INVALID_STATE) {
		return BLE_EVAL_PARAM_UPDATE_RATE_LIMITED;
	}
	return BLE_EVAL_PARAM_UPDATE_BACKOFF;
}
