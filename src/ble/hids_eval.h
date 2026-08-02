/*
 * hids_eval.h - Pure-C HID Service evaluation layer (host-testable, no SDK deps).
 *
 * Defines the composite HID Report Map (mouse + keyboard + consumer),
 * report sizes, refcount-based press/release state machines, HVX
 * pipeline credit management, and all HIDS protocol constants.
 *
 * The firmware C++ class (hids.h/cpp) wraps these evaluations with
 * SDK 15.3 ble_hids calls.
 *
 * Compiled on BOTH host (for unit tests) and device (linked into firmware).
 */
#ifndef HIDS_EVAL_H
#define HIDS_EVAL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Report IDs ------------------------------------------------------ */

#define HIDS_REPORT_ID_MOUSE       1u
#define HIDS_REPORT_ID_KEYBOARD    2u
#define HIDS_REPORT_ID_CONSUMER    3u

/* Report indices used by SDK ble_hids (zero-based input report order). */
#define HIDS_REPORT_IDX_MOUSE      0u
#define HIDS_REPORT_IDX_KEYBOARD   1u
#define HIDS_REPORT_IDX_CONSUMER   2u
/* Output report index (keyboard LEDs). */
#define HIDS_REPORT_IDX_KB_LED     0u

/* ---- Report payload sizes (bytes, excluding Report ID prefix) --------- */

#define HIDS_MOUSE_REPORT_SIZE     7u   /* buttons(1) X(2) Y(2) wheel(1) pan(1) */
#define HIDS_KB_INPUT_REPORT_SIZE  8u   /* modifier(1) reserved(1) keys(6) */
#define HIDS_KB_LED_REPORT_SIZE    1u   /* 5 LED bits + 3 padding */
#define HIDS_CONSUMER_REPORT_SIZE  2u   /* one 16-bit usage */

/* ---- HID Information characteristic ----------------------------------- */

#define HIDS_BCD_HID               0x0101u  /* HID spec v1.1 */
#define HIDS_INFO_FLAGS_NORMALLY_CONNECTABLE  0x02u
#define HIDS_INFO_FLAGS_REMOTE_WAKE           0x04u
/* CLUE has no HID wake source wired (no capacitive wake, button wake is
 * consumed by SystemOff entry, not the HID suspend protocol). Advertising
 * REMOTE_WAKE would lie to the host: it would issue SUSPEND/EXIT_SUSPEND
 * expecting the device to wake the host on HID activity, which we cannot
 * do. Keep only NORMALLY_CONNECTABLE. */
#define HIDS_INFO_FLAGS  (HIDS_INFO_FLAGS_NORMALLY_CONNECTABLE)

/* ---- HID Service UUID and Report Reference --------------------------- */

#define HIDS_SERVICE_UUID          0x1812u

/* Report Reference descriptor values: (reportID, type). */
#define HIDS_REPORT_TYPE_INPUT     1u
#define HIDS_REPORT_TYPE_OUTPUT    2u
#define HIDS_REPORT_TYPE_FEATURE   3u

/* ---- Permissions (match BT_HIDS_DEFAULT_PERM_RW_ENCRYPT) ------------- */
/*
 * SEC_MODE_1_ENC_NO_MITM = { .sm = 1, .lv = 3 }
 * Read and write both require encryption (post-bonding).
 */
#define HIDS_PERM_SM               1u
#define HIDS_PERM_LV               3u  /* LE Secure Connections, no MITM */

/* ---- Keyboard modifier bitmask --------------------------------------- */

#define HIDS_MOD_LCTRL   0x01u
#define HIDS_MOD_LSHIFT  0x02u
#define HIDS_MOD_LALT    0x04u
#define HIDS_MOD_LGUI    0x08u
#define HIDS_MOD_RCTRL   0x10u
#define HIDS_MOD_RSHIFT  0x20u
#define HIDS_MOD_RALT    0x40u
#define HIDS_MOD_RGUI    0x80u

/* ---- Consumer page usages -------------------------------------------- */
/* Zero = release (no consumer key active). */

#define HIDS_CONSUMER_NONE          0x0000u
#define HIDS_CONSUMER_MENU_PICK     0x0041u
#define HIDS_CONSUMER_MENU_UP       0x0042u
#define HIDS_CONSUMER_MENU_DOWN     0x0043u
#define HIDS_CONSUMER_MENU_LEFT     0x0044u
#define HIDS_CONSUMER_MENU_RIGHT    0x0045u
#define HIDS_CONSUMER_MENU_ESCAPE   0x0046u
#define HIDS_CONSUMER_AC_SEARCH     0x0221u
#define HIDS_CONSUMER_AC_HOME       0x0223u
#define HIDS_CONSUMER_AC_BACK       0x0224u
#define HIDS_CONSUMER_VOL_UP        0x00E9u
#define HIDS_CONSUMER_VOL_DOWN      0x00EAu
#define HIDS_CONSUMER_MUTE          0x00E2u
#define HIDS_CONSUMER_PLAY_PAUSE    0x00CDu

/* Maximum consumer usage value in our report map (logical maximum). */
#define HIDS_CONSUMER_LOGICAL_MAX   0x02FFu

/* ---- Press sources for refcount tracking ----------------------------- */

#define HIDS_MAX_SOURCES  4u

typedef enum {
	HIDS_SRC_BUTTON_A   = 0,
	HIDS_SRC_BUTTON_B   = 1,
	HIDS_SRC_AIR_MOUSE  = 2,
	HIDS_SRC_APDS_SWIPE = 3,
} hids_src_t;

/* Source bitmask helper. */
#define HIDS_SRC_BIT(src)  (1u << (src))

/* ---- Modifier refcount state ----------------------------------------- */

/*
 * Each of the 8 keyboard modifiers has a source bitmask tracking which
 * sources are pressing it. The modifier is active in the report when
 * the bitmask is nonzero.
 */
typedef struct {
	uint8_t source_bits;  /* bitmask of HIDS_SRC_BIT values */
} hids_mod_ref_t;

typedef struct {
	hids_mod_ref_t mods[8];  /* one per modifier bit (LCTRL..RGUI) */
} hids_modifier_state_t;

/* ---- Keyboard keycode refcount state --------------------------------- */

#define HIDS_MAX_KB_KEYS  6u   /* HID spec: max 6 simultaneous keys */

typedef struct {
	uint8_t keycode;      /* 0 = empty slot */
	uint8_t source_bits;  /* bitmask of pressing sources */
} hids_key_ref_t;

typedef struct {
	hids_key_ref_t slots[HIDS_MAX_KB_KEYS];
	uint8_t rejected_count;  /* keys rejected by 7-key limit (overflow) */
} hids_keyboard_state_t;

/* ---- Mouse button refcount state ------------------------------------- */

typedef struct {
	uint8_t source_bits[8];  /* one per button (buttons 1-8) */
} hids_mouse_button_state_t;

/* ---- Consumer usage refcount state ----------------------------------- */
/*
 * Only one consumer usage active at a time (report = single 16-bit value).
 * The refcount tracks how many sources are pressing the current usage.
 * A different usage from a second source overwrites only when the first
 * is fully released. If a third source presses yet another usage while
 * two are active, it is rejected (count overflow).
 */
#define HIDS_MAX_CONSUMER_REFS  4u  /* max concurrent consumer presses tracked */

typedef struct {
	uint16_t usage;
	uint8_t  source_bits;
} hids_consumer_ref_t;

typedef struct {
	hids_consumer_ref_t refs[HIDS_MAX_CONSUMER_REFS];
	uint8_t active_count;
} hids_consumer_state_t;

/* ---- Mouse relative movement (not refcounted; snapshot value) -------- */

typedef struct {
	int16_t x;
	int16_t y;
	int8_t  wheel;
	int8_t  pan;
} hids_mouse_move_t;

/* ---- HVX pipeline credit management ---------------------------------- */

#define HIDS_HVX_CREDITS_MAX  2u

typedef struct {
	uint8_t credits;     /* available credits (0 to HIDS_HVX_CREDITS_MAX) */
	uint8_t dirty_mask;  /* bitmask: bit(report_id-1) set = needs flush */
} hids_hvx_state_t;

/* Report ID → dirty bit mapping. */
#define HIDS_DIRTY_BIT_MOUSE     0x01u  /* bit 0 for report ID 1 */
#define HIDS_DIRTY_BIT_KEYBOARD  0x02u  /* bit 1 for report ID 2 */
#define HIDS_DIRTY_BIT_CONSUMER  0x04u  /* bit 2 for report ID 3 */

#define HIDS_DIRTY_BIT(report_id)  (1u << ((report_id) - 1u))

/* ---- Aggregate HIDS input state -------------------------------------- */

typedef struct {
	hids_modifier_state_t   modifiers;
	hids_keyboard_state_t   keyboard;
	hids_mouse_button_state_t mouse_buttons;
	hids_consumer_state_t   consumer;
	hids_mouse_move_t       mouse_move;
	hids_hvx_state_t        hvx;
} hids_state_t;

/* ---- Report Map (compile-time constant) ------------------------------ */

/* Total size of the composite HID Report Map in bytes. */
#define HIDS_REPORT_MAP_SIZE  169u

/* The byte-exact HID Report Map. See hids_eval.c for the array definition. */
extern const uint8_t hids_report_map[HIDS_REPORT_MAP_SIZE];

/* ---- Initialization --------------------------------------------------- */

/* Reset all HIDS state to zero (no presses, full credits). */
void hids_state_init(hids_state_t *state);

/* ---- Modifier refcount API ------------------------------------------- */

/* Press a modifier from a source. Returns true if the modifier state changed. */
bool hids_modifier_press(hids_modifier_state_t *mods, uint8_t mod_bit, hids_src_t src);

/* Release a modifier from a source. Returns true if the modifier state changed. */
bool hids_modifier_release(hids_modifier_state_t *mods, uint8_t mod_bit, hids_src_t src);

/* Build the modifier byte from current state. */
uint8_t hids_modifier_build(const hids_modifier_state_t *mods);

/* ---- Keyboard keycode refcount API ----------------------------------- */

/*
 * Press a keycode from a source. Returns:
 *   true  = keycode accepted (added or refcount incremented)
 *   false = rejected (7th simultaneous keycode, no room)
 */
bool hids_keyboard_press(hids_keyboard_state_t *kb, uint8_t keycode, hids_src_t src);

/* Release a keycode from a source. Returns true if state changed. */
bool hids_keyboard_release(hids_keyboard_state_t *kb, uint8_t keycode, hids_src_t src);

/* Build the 6-key array from current state. Writes 6 bytes to out_keys.
 * Empty slots are zero. Returns the number of active keys (0-6). */
uint8_t hids_keyboard_build_keys(const hids_keyboard_state_t *kb, uint8_t out_keys[6]);

/* Returns the number of distinct active keycodes (may exceed 6). */
uint8_t hids_keyboard_active_count(const hids_keyboard_state_t *kb);

/* ---- Mouse button refcount API --------------------------------------- */

/* Press a mouse button (1-8) from a source. Returns true if changed. */
bool hids_mouse_button_press(hids_mouse_button_state_t *btn, uint8_t button_idx, hids_src_t src);

/* Release a mouse button from a source. Returns true if changed. */
bool hids_mouse_button_release(hids_mouse_button_state_t *btn, uint8_t button_idx, hids_src_t src);

/* Build the mouse button byte from current state (button_idx 1-8 → bits 0-7). */
uint8_t hids_mouse_button_build(const hids_mouse_button_state_t *btn);

/* ---- Consumer usage refcount API ------------------------------------- */

/*
 * Press a consumer usage from a source. Returns:
 *   true  = accepted (new usage or refcount incremented)
 *   false = rejected (too many concurrent different usages)
 */
bool hids_consumer_press(hids_consumer_state_t *con, uint16_t usage, hids_src_t src);

/* Release a consumer usage from a source. Returns true if state changed. */
bool hids_consumer_release(hids_consumer_state_t *con, uint16_t usage, hids_src_t src);

/*
 * Build the consumer report value. Returns the active usage (0 if none).
 * When multiple usages are active, returns the first one.
 */
uint16_t hids_consumer_build(const hids_consumer_state_t *con);

/* ---- Mouse movement API ----------------------------------------------- */

/* Set mouse relative movement. Marks the mouse report dirty. */
void hids_mouse_set_move(hids_state_t *state, int16_t x, int16_t y, int8_t wheel, int8_t pan);

/* ---- Report building (full report bytes) ------------------------------ */

/* Build a mouse report (7 bytes). Writes to out (must be >= HIDS_MOUSE_REPORT_SIZE). */
void hids_build_mouse_report(const hids_state_t *state, uint8_t *out);

/* Build a keyboard input report (8 bytes). */
void hids_build_keyboard_report(const hids_state_t *state, uint8_t *out);

/* Build a consumer report (2 bytes, little-endian usage). */
void hids_build_consumer_report(const hids_state_t *state, uint8_t *out);

/* ---- HVX pipeline API ------------------------------------------------- */

/* Initialize HVX state with full credits and no dirty reports. */
void hids_hvx_init(hids_hvx_state_t *hvx);

/* Mark a report dirty (state changed, needs flush). */
void hids_hvx_mark_dirty(hids_hvx_state_t *hvx, uint8_t report_id);

/*
 * Determine which report to send next. Returns:
 *   0  = no report ready (no credits or nothing dirty)
 *   1  = mouse report
 *   2  = keyboard report
 *   3  = consumer report
 *
 * Priority: consumer > keyboard > mouse (releases first to avoid stuck keys).
 * Consumer (bit 2) is checked before keyboard (bit 1) before mouse (bit 0).
 */
uint8_t hids_hvx_next_send(const hids_hvx_state_t *hvx);

/* Called after successfully sending a report: consume a credit, clear dirty bit. */
void hids_hvx_on_sent(hids_hvx_state_t *hvx, uint8_t report_id);

/* Called on TX-complete: restore a credit. Returns true if there is pending work. */
bool hids_hvx_on_tx_complete(hids_hvx_state_t *hvx);

/* Returns true if any report is pending (dirty with credits available). */
bool hids_hvx_has_pending(const hids_hvx_state_t *hvx);

/* Returns true if any report is dirty but blocked by no credits (backpressure). */
bool hids_hvx_is_backpressured(const hids_hvx_state_t *hvx);

/* ---- Report Map validation ------------------------------------------- */

/* Returns the size of the report map (HIDS_REPORT_MAP_SIZE). */
uint16_t hids_report_map_size(void);

/* Returns true if the report map matches the expected golden bytes. */
bool hids_report_map_validate(const uint8_t *map, uint16_t len);

/* Returns the Report Reference descriptor value for a given report ID. */
/* type: HIDS_REPORT_TYPE_INPUT or HIDS_REPORT_TYPE_OUTPUT. */
uint8_t hids_report_ref_type(uint8_t report_id);

/* ---- Control Point ---------------------------------------------------- */

typedef enum {
	HIDS_CTRL_SUSPEND  = 0u,
	HIDS_CTRL_EXIT_SUSP = 1u,
} hids_control_point_t;

#ifdef __cplusplus
}
#endif

#endif /* HIDS_EVAL_H */
