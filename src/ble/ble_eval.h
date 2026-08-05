/*
 * ble_eval.h - Pure-C BLE-HID evaluation layer (host-testable, no SDK deps).
 *
 * Contains constants, unit conversions, SD header validation, advertising
 * state machine, connection-parameter latency FSM, and IRQ priority
 * assertions. The firmware C++ classes in ble_runtime/gatt/advertising/
 * security wrap these evaluations with real SDK 15.3 calls.
 *
 * Compiled on BOTH host (for unit tests) and device (linked into firmware).
 */
#ifndef BLE_EVAL_H
#define BLE_EVAL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- SoftDevice header validation (S140 6.1.1) ---------------------- */

/*
 * The Adafruit bootloader places the SoftDevice info struct at 0x3000
 * (MBR_SIZE + SOFTDEVICE_INFO_STRUCT_OFFSET). Field offsets from there:
 *   +0x00: (reserved)
 *   +0x04: magic word  0x51B1E5DB
 *   +0x08: SD size     0x26000 for S140 6.1.1
 *   +0x0C: FWID        0x00B6 for S140 6.1.1
 *
 * The old Butterfly main.cpp checked magic at 0x1000 — WRONG.
 */
#define BLE_SD_INFO_STRUCT_ADDR    0x3000u
#define BLE_SD_MAGIC_ADDR          (BLE_SD_INFO_STRUCT_ADDR + 0x04u)
#define BLE_SD_SIZE_ADDR           (BLE_SD_INFO_STRUCT_ADDR + 0x08u)
#define BLE_SD_FWID_ADDR           (BLE_SD_INFO_STRUCT_ADDR + 0x0Cu)

#define BLE_SD_MAGIC_EXPECTED      0x51B1E5DBu
#define BLE_SD_SIZE_EXPECTED       0x26000u
#define BLE_SD_FWID_EXPECTED       0x00B6u

typedef enum {
	BLE_SD_VALID          = 0,
	BLE_SD_MAGIC_MISMATCH = 1,
	BLE_SD_SIZE_MISMATCH  = 2,
	BLE_SD_FWID_MISMATCH  = 3,
} ble_sd_validation_t;

/* Backend: read a 32-bit word from an absolute address.
 * On device: *(volatile uint32_t *). On host: mock table. */
typedef uint32_t (*ble_sd_read32_fn)(uint32_t addr);

typedef struct {
	ble_sd_read32_fn read32;
} ble_sd_backend_t;

/* Validate SD header. Checks magic FIRST — short-circuits on mismatch
 * before trusting size/FWID. Returns BLE_SD_VALID only if all three match. */
ble_sd_validation_t ble_validate_sd(const ble_sd_backend_t *backend);

/* ---- RAM origin check ------------------------------------------------ */

/* S140 6.1.1 baseline RAM start (verified from Adafruit nrf52840_s140_v6.ld). */
#define BLE_RAM_ORIGIN_BASELINE   0x20006000u

typedef enum {
	BLE_RAM_OK          = 0,  /* linker origin >= SD requirement */
	BLE_RAM_INSUFFICIENT = 1, /* linker reserves less than SD needs → abort */
	BLE_RAM_SURPLUS      = 2, /* SD needs more than linker → raise origin + rebuild */
} ble_ram_check_t;

/* Compare linker-provided RAM start against SD-required start.
 * If SD requires higher → SURPLUS (documented reflash, not in-app fix).
 * If linker provides less than baseline → INSUFFICIENT. */
ble_ram_check_t ble_check_ram_origin(uint32_t linker_ram_start,
                                     uint32_t sd_required_ram_start);

/* ---- BLE configuration constants ------------------------------------- */

/* Link counts: exactly one peripheral, zero central (HID remote role). */
#define BLE_PERIPHERAL_LINK_COUNT   1u
#define BLE_CENTRAL_LINK_COUNT      0u
#define BLE_TOTAL_LINK_COUNT        1u

/* GATTS HVN TX queue: at least 2 for HID pipeline-of-2 (nRF Desktop pattern). */
#define BLE_HVN_TX_QUEUE_MIN        2u

/* Device name. */
#define BLE_DEVICE_NAME             "Butterfly CLUE Remote"
#define BLE_DEVICE_NAME_LEN         21u  /* strlen without NUL */

/* LFRC (CLUE has no LF crystal). */
#define BLE_LF_CLK_SRC              1u   /* CLOCK_LFCLKSRC_SRC_RC */

/* ---- Interval unit conversions --------------------------------------- */

/*
 * GAP connection interval: units of 1.25 ms.
 *   7.5 ms → 6,  15 ms → 12
 * Formula: interval_units = (ms * 4) / 5
 */
uint16_t ble_gap_interval_from_ms(uint32_t ms);

/*
 * Advertising interval: units of 0.625 ms.
 *   30 ms → 48,  250 ms → 400
 * Formula: adv_units = (ms * 8) / 5
 */
uint16_t ble_adv_interval_from_ms(uint32_t ms);

/*
 * Supervision timeout: units of 10 ms.
 *   4000 ms → 400
 */
uint16_t ble_sup_timeout_from_ms(uint32_t ms);

/* Advertising timeout: units of 10 ms. 0 = unlimited. */
#define BLE_ADV_TIMEOUT_FROM_MS(ms)  ((uint16_t)((ms) / 10u))

/* ---- Connection parameters ------------------------------------------- */

/* Active: 7.5–15 ms, latency 0. Idle: latency 4 after 5 s inactivity.
 * Supervision timeout: 4 s.
 * Note: 7.5 ms cannot be represented as an integer ms value. Use the
 * GAP unit constant directly (6 units = 7.5 ms). */
#define BLE_CONN_MIN_INTERVAL_MS     8u    /* ceil(7.5) — converts to GAP unit 6 */
#define BLE_CONN_MAX_INTERVAL_MS     15u
#define BLE_CONN_TIMEOUT_MS          4000u
#define BLE_LATENCY_ACTIVE           0u
#define BLE_LATENCY_IDLE             4u
#define BLE_IDLE_THRESHOLD_MS        5000u

/* ---- Advertising state machine --------------------------------------- */

typedef enum {
	BLE_ADV_STATE_IDLE      = 0,
	BLE_ADV_STATE_FAST      = 1,  /* 30 ms interval, 30 s timeout */
	BLE_ADV_STATE_SLOW      = 2,  /* 250 ms interval, unlimited */
	BLE_ADV_STATE_DIRECTED  = 3,  /* low-duty directed to bonded peer */
	BLE_ADV_STATE_STOPPED   = 4,
} ble_adv_state_t;

typedef enum {
	BLE_ADV_EVT_START            = 0,
	BLE_ADV_EVT_FAST_TIMEOUT     = 1,
	BLE_ADV_EVT_CONNECTED        = 2,
	BLE_ADV_EVT_DISCONNECTED     = 3,
	BLE_ADV_EVT_BONDED_PEER      = 4,
	BLE_ADV_EVT_IDLE_ACTIVITY    = 5,  /* user activity → restart fast */
	BLE_ADV_EVT_STOP             = 6,
} ble_adv_event_t;

/* Fast advertising parameters. */
#define BLE_ADV_FAST_INTERVAL_MS    30u
#define BLE_ADV_FAST_TIMEOUT_MS     30000u

/* Slow advertising parameters. */
#define BLE_ADV_SLOW_INTERVAL_MS    250u
#define BLE_ADV_SLOW_TIMEOUT_UNLIM  0u

/* Directed advertising fallback timeout (timed open). */
#define BLE_ADV_DIRECTED_TIMEOUT_MS  5000u

/* Advertising FSM. Returns next state.
 *   START from IDLE → FAST
 *   FAST_TIMEOUT → SLOW
 *   CONNECTED → STOPPED
 *   DISCONNECTED + bonded → DIRECTED
 *   DISCONNECTED + not bonded → SLOW (idle-event restart goes to FAST)
 *   IDLE_ACTIVITY from SLOW → FAST (restart)
 *   IDLE_ACTIVITY from DIRECTED → FAST
 *   STOP → STOPPED */
ble_adv_state_t ble_adv_fsm(ble_adv_state_t current,
                            ble_adv_event_t event,
                            bool has_bond);

/* ---- Latency FSM ----------------------------------------------------- */

typedef enum {
	BLE_LAT_STATE_ACTIVE = 0,
	BLE_LAT_STATE_IDLE   = 1,
	BLE_LAT_STATE_BACKOFF = 2,  /* host rejected update, waiting */
} ble_latency_state_t;

typedef enum {
	BLE_LAT_EVT_ACTIVITY     = 0,  /* pointer/key/consumer activity */
	BLE_LAT_EVT_IDLE_5S      = 1,  /* 5 s elapsed without activity */
	BLE_LAT_EVT_HOST_REJECT  = 2,  /* host rejected latency update */
	BLE_LAT_EVT_BACKOFF_ELAPSED = 3, /* 30 s backoff elapsed */
} ble_latency_event_t;

#define BLE_LATENCY_BACKOFF_MS  30000u  /* 30 s between rejected updates */

/* Latency FSM. Returns next state.
 *   ACTIVITY from IDLE/BACKOFF → ACTIVE (request latency 0)
 *   IDLE_5S from ACTIVE → IDLE (request latency 4)
 *   HOST_REJECT from ACTIVE/IDLE → BACKOFF
 *   BACKOFF_ELAPSED from BACKOFF → IDLE (retry latency 4) */
ble_latency_state_t ble_latency_fsm(ble_latency_state_t current,
                                    ble_latency_event_t event);

/* Returns the latency value for the given state. */
uint16_t ble_latency_value(ble_latency_state_t state);

/* ---- IRQ priority assertions ----------------------------------------- */

/*
 * S140 reserves priority 0 for: TIMER0, RTC0, RADIO_IRQn, SWI/EGU.
 * Raw-WHAD uses priority 1 for TIMER3/TIMER4 (NOT SD-reserved).
 * BLE-HID must NOT start TIMER3/TIMER4.
 * Application Board drivers use priority 6.
 */
#define BLE_IRQ_PRIO_SD_RESERVED   0u
#define BLE_IRQ_PRIO_RAW_TIMER     1u
#define BLE_IRQ_PRIO_APP_DEFAULT   6u

/* Returns true if priority 0 (SD-reserved). */
bool ble_irq_priority_is_sd_reserved(uint32_t priority);

/* Returns true if priority >= 2 and <= 6 (app-safe). */
bool ble_irq_priority_is_app_safe(uint32_t priority);

/* Assert BLE mode never starts raw timers.
 * Returns true if neither timer is started (safe for BLE). */
bool ble_ble_mode_raw_timer_check(bool timer3_started, bool timer4_started);

/* ---- Security parameters --------------------------------------------- */

/* LESC Just Works: bonding, no MITM, 16-byte key. */
#define BLE_SEC_BOND          1u
#define BLE_SEC_MITM          0u
#define BLE_SEC_LESC          1u
#define BLE_SEC_KEYPRESS      0u
#define BLE_SEC_IO_CAPS       3u   /* BLE_GAP_IO_CAPS_NONE */
#define BLE_SEC_MIN_KEY_SIZE  16u  /* 16 octets = 128 bits */
#define BLE_SEC_MAX_KEY_SIZE  16u

/* ---- Advertising-mode mapping + param-update classifier --------------- */

/*
 * The advertising.cpp SDK wrapper maps ble_adv_state_t → ble_adv_mode_t
 * (BLE_ADV_MODE_FAST/SLOW/DIRECTED/IDLE). This returns that mapping as an
 * integer so host tests can validate it without including SDK headers.
 * Returns: IDLE/STOPPED→0, FAST→1, SLOW→2, DIRECTED→3.
 */
int ble_eval_state_to_adv_mode(ble_adv_state_t state);

/* SDK error codes mirrored for host-side classifier (no SDK header needed).
 * These are the two codes advertising.cpp:208-213 actually branches on. */
#define BLE_EVAL_ERR_SUCCESS          0x0000u
#define BLE_EVAL_ERR_INVALID_STATE    0x0003u

typedef enum {
	BLE_EVAL_PARAM_UPDATE_OK            = 0,  /* sd call succeeded */
	BLE_EVAL_PARAM_UPDATE_RATE_LIMITED  = 1,  /* INVALID_STATE — host negotiating */
	BLE_EVAL_PARAM_UPDATE_BACKOFF       = 2,  /* INVALID_STATE + no connection */
	BLE_EVAL_PARAM_UPDATE_NO_CONNECTION = 3,  /* conn_handle invalid */
} ble_param_update_result_t;

/* Classify sd_ble_gap_conn_param_update() result.
 * Mirrors advertising.cpp:186-216 decision logic without SDK deps. */
ble_param_update_result_t ble_eval_classify_param_update_error(uint32_t err_code,
                                                               bool connected);

#ifdef __cplusplus
}
#endif

#endif /* BLE_EVAL_H */
