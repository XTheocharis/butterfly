/*
 * runtime.h - dual-runtime architecture for CLUE (raw-WHAD vs BLE-HID).
 *
 * Two exclusive runtimes share one nRF52840. The active runtime is
 * selected BEFORE Core construction and cannot change without a reset.
 *
 * Selection precedence (highest first):
 *   1. GPREGRET2 one-shot (0xC1 raw, 0xC2 BLE) — consumed on read
 *   2. RuntimeConfigStore persisted mode — UNAVAILABLE in Wave 1
 *   3. Raw-WHAD default
 *
 * Controlled switching (runtime_request_switch) enters SWITCHING,
 * persists the target via GPREGRET2, drains the correlated USB
 * response, arms a 2 s WDT fallback, and resets. The WDT fires if
 * the reset path stalls. No in-place SoftDevice teardown.
 *
 * Pure logic: no SDK deps. Caller injects a backend at init for
 * GPREGRET2 access, store read/write, reset, time, and WDT.
 * Compiles on host for unit testing.
 */
#ifndef RUNTIME_H
#define RUNTIME_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Runtime modes --------------------------------------------------- */

typedef enum {
	RUNTIME_RAW_WHAD = 0,
	RUNTIME_BLE_HID  = 1,
} runtime_mode_t;

#define RUNTIME_MODE_COUNT 2

/* GPREGRET2 one-shot values — deliberately distinct from bootloader
 * GPREGRET commands (0xB1, 0xA8, 0x4E, 0x57, 0x6D). */
#define RUNTIME_GPREGRET2_RAW_WHAD  0xC1u
#define RUNTIME_GPREGRET2_BLE_HID   0xC2u

/* ---- RuntimeConfigStore result --------------------------------------- */

typedef enum {
	RUNTIME_STORE_OK           = 0,
	RUNTIME_STORE_UNAVAILABLE  = 1,
	RUNTIME_STORE_INVALID      = 2,
	RUNTIME_STORE_IO_ERROR     = 3,
} runtime_store_result_t;

/* ---- Switching state machine ----------------------------------------- */

typedef enum {
	RUNTIME_STATE_INIT     = 0,
	RUNTIME_STATE_RUNNING  = 1,
	RUNTIME_STATE_SWITCHING = 2,
} runtime_state_t;

/* ---- Switch result --------------------------------------------------- */

typedef enum {
	RUNTIME_SWITCH_OK            = 0,
	RUNTIME_SWITCH_ALREADY_SW    = 1,  /* already in SWITCHING */
	RUNTIME_SWITCH_INVALID_MODE  = 2,
	RUNTIME_SWITCH_QUIESCE_FAIL  = 3,  /* fallible step failed */
	RUNTIME_SWITCH_NOT_INIT      = 4,
} runtime_switch_result_t;

/* ---- Backend (injected at init for testability) ---------------------- */

typedef struct {
	/* GPREGRET2 access — SD-safe on device, mocked on host. */
	uint32_t (*gpregret2_read)(void);
	void     (*gpregret2_clear)(void);
	void     (*gpregret2_set)(uint8_t value);

	/* RuntimeConfigStore — Wave 1 backend returns UNAVAILABLE. */
	runtime_store_result_t (*store_read)(runtime_mode_t *out_mode);
	runtime_store_result_t (*store_write)(runtime_mode_t mode);

	/* System reset for controlled switching. */
	void     (*system_reset)(void);

	/* Monotonic microseconds for bounded timeouts. */
	uint64_t (*now_us)(void);

	/* WDT arm (2 s) and feed for switch fallback. */
	void     (*wdt_arm)(void);
	void     (*wdt_feed)(void);
} runtime_backend_t;

/* ---- Lifecycle ------------------------------------------------------- */

/* Inject the backend. Must be called before runtime_select(). */
void runtime_init(const runtime_backend_t *backend);

/* ---- Selection (call BEFORE Core construction) ----------------------- */

/* Read GPREGRET2 one-shot, query store, apply precedence.
 * Clears a valid one-shot after reading. Clears an invalid one-shot.
 * Returns the selected mode and sets *out_source to the winning source
 * (0=raw default, 1=store, 2=oneshot). */
runtime_mode_t runtime_select(int *out_source);

/* Direct mode set (test harness or forced override). Does not perform
 * switching — only sets the static selection state. */
void runtime_set_selected(runtime_mode_t mode);

/* Get the currently selected mode. */
runtime_mode_t runtime_get_selected(void);

/* ---- State machine --------------------------------------------------- */

runtime_state_t runtime_get_state(void);
void runtime_set_state(runtime_state_t state);

/* ---- Capability tables ----------------------------------------------- */

/* Capability table descriptor — populated by the build (capabilities.h).
 * Raw-WHAD: Board + 5 radio domains (filled by Todo 14).
 * BLE-HID: Board only (filled by Todo 14). */
typedef struct {
	const void *capabilities;        /* whad_domain_desc_t[] or NULL */
	const void *frequency_ranges;    /* whad_phy_frequency_range_t[] or NULL */
} runtime_caps_t;

/* Returns the capability table for the given mode.
 * Returns NULL if mode is invalid or table not yet populated. */
const runtime_caps_t *runtime_get_caps(runtime_mode_t mode);

/* Register a capability table for a mode (called from Core init). */
void runtime_register_caps(runtime_mode_t mode, const runtime_caps_t *caps);

/* ---- Controlled switching -------------------------------------------- */

/* Returns true if mode is a valid runtime mode. */
bool runtime_mode_is_valid(runtime_mode_t mode);

/* Returns true if the given GPREGRET2 value is a valid one-shot. */
bool runtime_gpregret2_is_valid(uint32_t value);

/* Convert a GPREGRET2 value to a runtime mode. Returns false if invalid. */
bool runtime_gpregret2_to_mode(uint32_t value, runtime_mode_t *out);

/* Convert a runtime mode to its GPREGRET2 one-shot value. */
uint8_t runtime_mode_to_gpregret2(runtime_mode_t mode);

/* Initiate a controlled switch to the target mode.
 *
 * Steps:
 *   1. Reject if already SWITCHING or mode invalid
 *   2. Enter SWITCHING state
 *   3. Set GPREGRET2 to target mode one-shot
 *   4. (future: quiesce BLE/QSPI/HID — stubs in Wave 1)
 *   5. Arm 2 s WDT
 *   6. System reset
 *
 * On any failure before reset: restore RUNNING, return error. */
runtime_switch_result_t runtime_request_switch(runtime_mode_t target);

/* ---- CDC close policy ------------------------------------------------ */

/* Returns true if a CDC port-close event should trigger a system reset.
 * Raw-WHAD: yes (legacy behavior). BLE-HID: no (host closing the serial
 * port must not kill an active BLE HID connection). */
bool runtime_cdc_close_should_reset(void);

/* Validate that a store result+mode is applicable for runtime selection.
 * Returns false for UNAVAILABLE/INVALID/IO_ERROR or invalid modes. */
bool runtime_store_mode_is_applicable(runtime_store_result_t result,
				      runtime_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* RUNTIME_H */
