/*
 * runtime.cpp - dual-runtime selection, state machine, and switching.
 *
 * Pure logic: no SDK deps. Backend injected at init. Compiles on host.
 *
 * Exception: runtime_request_switch() must drain the WHAD USB TX ring
 * before pulling reset, so the device-side build (BOARD_CLUE) pulls in
 * the transport + CDC ACM headers behind an #ifdef. Host test builds
 * compile the drain out and exercise only the state-machine contract.
 */
#include "runtime.h"
#include <string.h>

#ifdef BOARD_CLUE
extern "C" {
#include "transport.h"
}
#include "serial.h"
#endif

/* ---- Static state ---------------------------------------------------- */

static runtime_backend_t s_backend;
static bool s_initialized = false;

static runtime_mode_t   s_selected_mode   = RUNTIME_RAW_WHAD;
static runtime_state_t  s_state           = RUNTIME_STATE_INIT;
static int              s_last_source     = 0; /* 0=default, 1=store, 2=oneshot */

static runtime_caps_t s_caps[RUNTIME_MODE_COUNT];
static bool           s_caps_registered[RUNTIME_MODE_COUNT];

/* ---- Lifecycle ------------------------------------------------------- */

void runtime_init(const runtime_backend_t *backend)
{
	if (backend != NULL) {
		memcpy(&s_backend, backend, sizeof(s_backend));
	} else {
		memset(&s_backend, 0, sizeof(s_backend));
	}
	s_initialized = (backend != NULL);
	s_selected_mode = RUNTIME_RAW_WHAD;
	s_state = RUNTIME_STATE_INIT;
	s_last_source = 0;
	memset(s_caps_registered, 0, sizeof(s_caps_registered));
	memset(s_caps, 0, sizeof(s_caps));
}

/* ---- Validation helpers --------------------------------------------- */

bool runtime_mode_is_valid(runtime_mode_t mode)
{
	return (mode == RUNTIME_RAW_WHAD || mode == RUNTIME_BLE_HID);
}

bool runtime_gpregret2_is_valid(uint32_t value)
{
	return (value == RUNTIME_GPREGRET2_RAW_WHAD ||
	        value == RUNTIME_GPREGRET2_BLE_HID);
}

bool runtime_gpregret2_to_mode(uint32_t value, runtime_mode_t *out)
{
	if (!runtime_gpregret2_is_valid(value) || out == NULL) {
		return false;
	}
	*out = (value == RUNTIME_GPREGRET2_RAW_WHAD)
	           ? RUNTIME_RAW_WHAD
	           : RUNTIME_BLE_HID;
	return true;
}

uint8_t runtime_mode_to_gpregret2(runtime_mode_t mode)
{
	switch (mode) {
	case RUNTIME_RAW_WHAD: return RUNTIME_GPREGRET2_RAW_WHAD;
	case RUNTIME_BLE_HID:  return RUNTIME_GPREGRET2_BLE_HID;
	default:               return 0;
	}
}

/* ---- Selection ------------------------------------------------------- */

runtime_mode_t runtime_select(int *out_source)
{
	int source = 0;
	runtime_mode_t mode = RUNTIME_RAW_WHAD;

	if (!s_initialized) {
		s_selected_mode = mode;
		s_last_source = source;
		if (out_source != NULL) *out_source = source;
		return mode;
	}

	/* 1. GPREGRET2 one-shot (highest precedence). */
	if (s_backend.gpregret2_read != NULL) {
		uint32_t raw = s_backend.gpregret2_read();
		if (runtime_gpregret2_is_valid(raw)) {
			runtime_mode_t onesot;
			if (runtime_gpregret2_to_mode(raw, &onesot)) {
				mode = onesot;
				source = 2;
				/* Consume the one-shot. */
				if (s_backend.gpregret2_clear != NULL) {
					s_backend.gpregret2_clear();
				}
				s_selected_mode = mode;
				s_last_source = source;
				s_state = RUNTIME_STATE_INIT;
				if (out_source != NULL) *out_source = source;
				return mode;
			}
		} else if (raw != 0) {
			/* Invalid one-shot present — clear it. */
			if (s_backend.gpregret2_clear != NULL) {
				s_backend.gpregret2_clear();
			}
		}
	}

	/* 2. RuntimeConfigStore persisted mode. */
	if (s_backend.store_read != NULL) {
		runtime_mode_t stored;
		runtime_store_result_t sr = s_backend.store_read(&stored);
		if (sr == RUNTIME_STORE_OK && runtime_mode_is_valid(stored)) {
			mode = stored;
			source = 1;
		}
		/* UNAVAILABLE / INVALID / IO_ERROR → fall through to default. */
	}

	/* 3. Raw-WHAD default. */
	s_selected_mode = mode;
	s_last_source = source;
	s_state = RUNTIME_STATE_INIT;
	if (out_source != NULL) *out_source = source;
	return mode;
}

void runtime_set_selected(runtime_mode_t mode)
{
	if (runtime_mode_is_valid(mode)) {
		s_selected_mode = mode;
	}
}

runtime_mode_t runtime_get_selected(void)
{
	return s_selected_mode;
}

/* ---- State machine --------------------------------------------------- */

runtime_state_t runtime_get_state(void)
{
	return s_state;
}

void runtime_set_state(runtime_state_t state)
{
	s_state = state;
}

/* ---- Capability tables ----------------------------------------------- */

const runtime_caps_t *runtime_get_caps(runtime_mode_t mode)
{
	if (!runtime_mode_is_valid(mode)) return NULL;
	if (!s_caps_registered[mode]) return NULL;
	return &s_caps[mode];
}

void runtime_register_caps(runtime_mode_t mode, const runtime_caps_t *caps)
{
	if (!runtime_mode_is_valid(mode) || caps == NULL) return;
	s_caps[mode] = *caps;
	s_caps_registered[mode] = true;
}

/* ---- Controlled switching -------------------------------------------- */

#ifdef BOARD_CLUE
/* Drain the WHAD TX ring and wait for the in-flight CDC ACM TX to
 * finish so that a controlled mode switch does not yank USB mid-frame
 * (the host would see a truncated WHAD message followed by a bus
 * reset). Two bounded phases:
 *   1. Pump whad_transport_send_pending until the TX ring is empty
 *      (<= 100 ms wall-clock via the injected now_us clock).
 *   2. Wait for the CDC ACM driver to clear txInProgress from its
 *      TX-complete ISR (<= 50 ms fallback).
 * Returns true if both phases completed; false on timeout. The caller
 * proceeds either way -- the WDT fallback (armed next) catches a
 * genuinely stuck USB stack. */
static bool runtime_drain_usb(const runtime_backend_t *backend)
{
	if (backend == NULL || backend->now_us == NULL) {
		return true;
	}

	const uint64_t drain_deadline_us = backend->now_us() + 100000ULL;
	while (whad_transport_get_txbuf_size() > 0) {
		if (SerialComm::instance != NULL &&
		    !SerialComm::instance->txInProgress) {
			(void)whad_transport_send_pending();
		}
		app_usbd_event_queue_process();
		if (backend->now_us() >= drain_deadline_us) {
			return false;
		}
	}

	const uint64_t tx_done_deadline_us = backend->now_us() + 50000ULL;
	while (SerialComm::instance != NULL &&
	       SerialComm::instance->txInProgress) {
		app_usbd_event_queue_process();
		if (backend->now_us() >= tx_done_deadline_us) {
			return false;
		}
	}
	return true;
}
#endif /* BOARD_CLUE */

runtime_switch_result_t runtime_request_switch(runtime_mode_t target)
{
	if (!s_initialized) {
		return RUNTIME_SWITCH_NOT_INIT;
	}
	if (!runtime_mode_is_valid(target)) {
		return RUNTIME_SWITCH_INVALID_MODE;
	}
	if (target == s_selected_mode) {
		/* Already in the target mode — no-op. */
		return RUNTIME_SWITCH_OK;
	}
	if (s_state == RUNTIME_STATE_SWITCHING) {
		return RUNTIME_SWITCH_ALREADY_SW;
	}

	/* Enter SWITCHING state — reject new streams/storage from here on. */
	s_state = RUNTIME_STATE_SWITCHING;

	/* Clear GPREGRET2 before setting the exact one-shot value
	 * (avoids OR-ing stale bits). */
	if (s_backend.gpregret2_clear != NULL) {
		s_backend.gpregret2_clear();
	}
	if (s_backend.gpregret2_set != NULL) {
		s_backend.gpregret2_set(runtime_mode_to_gpregret2(target));
	}

	/* Drain the WHAD USB TX ring before WDT+reset. GPREGRET2 is
	 * already armed so a stall here still reboots into the target
	 * mode. The drain is best-effort: on timeout we proceed and let
	 * the WDT (armed next) handle a stuck USB stack. Host test builds
	 * compile this out (no BOARD_CLUE) and exercise only the
	 * state-machine ordering via the mock backend. */
#ifdef BOARD_CLUE
	(void)runtime_drain_usb(&s_backend);
#endif

	/* Arm the 2 s WDT fallback before resetting. If the reset
	 * path stalls, the WDT will force a reboot. On reboot,
	 * runtime_select() reads the GPREGRET2 one-shot and boots
	 * into the target mode. */
	if (s_backend.wdt_arm != NULL) {
		s_backend.wdt_arm();
	}

	/* System reset. This does not return. */
	if (s_backend.system_reset != NULL) {
		s_backend.system_reset();
	}

	/* If system_reset returns (test harness / no backend), restore
	 * state for the caller to inspect. */
	s_state = RUNTIME_STATE_RUNNING;
	return RUNTIME_SWITCH_OK;
}

/* ---- CDC close policy ------------------------------------------------ */

bool runtime_cdc_close_should_reset(void)
{
	/* Raw-WHAD preserves legacy CDC-close-reset behavior so the host
	 * can force a device reset by closing and reopening the serial
	 * port. BLE-HID must NOT reset on CDC close — an active HID
	 * connection must survive the host closing the serial monitor. */
	return (s_selected_mode == RUNTIME_RAW_WHAD);
}

/* ---- Adopted-QSPI store validation (Todo 30) ------------------------- */

/* When the QSPI store backend (calib_runtime_store_read from calib.cpp)
 * returns a mode, this helper validates it before runtime_select()
 * applies it. An invalid stored mode (corrupt flash, partial write)
 * is silently rejected — the caller falls through to the default.
 *
 * The actual QSPI read is behind the injected backend function pointer.
 * This helper only adds the validation layer that the stub in Todo 11
 * lacked. The precedence chain in runtime_select() is:
 *   1. GPREGRET2 one-shot (consumed on read)
 *   2. Adopted-QSPI persisted mode (validated here before apply)
 *   3. Raw-WHAD default
 *
 * Non-adopted storage: store_read returns UNAVAILABLE, so step 2
 * is skipped entirely and selection falls to the default. */
bool runtime_store_mode_is_applicable(runtime_store_result_t result,
				      runtime_mode_t mode)
{
	if (result != RUNTIME_STORE_OK) {
		return false;
	}
	return runtime_mode_is_valid(mode);
}
