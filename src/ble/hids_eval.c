/*
 * hids_eval.c - Pure-C HIDS evaluation implementations.
 *
 * No SDK dependencies. Compiled on both host (tests) and device (firmware).
 */
#include "hids_eval.h"
#include <string.h>

/* ---- HID Report Map (byte-exact composite descriptor) --------------- *
 *                                                                     *
 * Layout (169 bytes total):                                           *
 *                                                                     *
 * Mouse collection (75 bytes, Report ID 1):                           *
 *   8 buttons (1 byte bitmask), X/Y int16 relative, wheel/pan int8    *
 *                                                                     *
 * Keyboard collection (69 bytes, Report ID 2):                        *
 *   8 modifiers (1 byte), 1 reserved byte, 6 keycodes, LED output     *
 *                                                                     *
 * Consumer collection (25 bytes, Report ID 3):                        *
 *   Single 16-bit usage (array), Usage Min 0 Max 0x02FF               *
 */
const uint8_t hids_report_map[HIDS_REPORT_MAP_SIZE] = {

	/* ===== Mouse (Report ID 1) — 75 bytes ===== */
	0x05, 0x01,             /* Usage Page (Generic Desktop) */
	0x09, 0x02,             /* Usage (Mouse) */
	0xA1, 0x01,             /* Collection (Application) */
	0x85, 0x01,             /*   Report ID (1) */
	0x09, 0x01,             /*   Usage (Pointer) */
	0xA1, 0x00,             /*   Collection (Physical) */

	/* Buttons 1-8 */
	0x05, 0x09,             /*     Usage Page (Button) */
	0x19, 0x01,             /*     Usage Minimum (1) */
	0x29, 0x08,             /*     Usage Maximum (8) */
	0x15, 0x00,             /*     Logical Minimum (0) */
	0x25, 0x01,             /*     Logical Maximum (1) */
	0x75, 0x01,             /*     Report Size (1) */
	0x95, 0x08,             /*     Report Count (8) */
	0x81, 0x02,             /*     Input (Data,Var,Abs) */

	/* X, Y (relative int16) */
	0x05, 0x01,             /*     Usage Page (Generic Desktop) */
	0x09, 0x30,             /*     Usage (X) */
	0x09, 0x31,             /*     Usage (Y) */
	0x16, 0x00, 0x80,       /*     Logical Minimum (-32768) */
	0x26, 0xFF, 0x7F,       /*     Logical Maximum (32767) */
	0x75, 0x10,             /*     Report Size (16) */
	0x95, 0x02,             /*     Report Count (2) */
	0x81, 0x06,             /*     Input (Data,Var,Rel) */

	/* Wheel (relative int8) */
	0x09, 0x38,             /*     Usage (Wheel) */
	0x15, 0x81,             /*     Logical Minimum (-127) */
	0x25, 0x7F,             /*     Logical Maximum (127) */
	0x75, 0x08,             /*     Report Size (8) */
	0x95, 0x01,             /*     Report Count (1) */
	0x81, 0x06,             /*     Input (Data,Var,Rel) */

	/* AC Pan / horizontal wheel (relative int8) */
	0x05, 0x0C,             /*     Usage Page (Consumer) */
	0x0A, 0x38, 0x02,       /*     Usage (AC Pan) */
	0x15, 0x81,             /*     Logical Minimum (-127) */
	0x25, 0x7F,             /*     Logical Maximum (127) */
	0x75, 0x08,             /*     Report Size (8) */
	0x95, 0x01,             /*     Report Count (1) */
	0x81, 0x06,             /*     Input (Data,Var,Rel) */

	0xC0,                   /*   End Collection (Physical) */
	0xC0,                   /* End Collection (Application) */

	/* ===== Keyboard (Report ID 2) — 69 bytes ===== */
	0x05, 0x01,             /* Usage Page (Generic Desktop) */
	0x09, 0x06,             /* Usage (Keyboard) */
	0xA1, 0x01,             /* Collection (Application) */
	0x85, 0x02,             /*   Report ID (2) */

	/* Modifiers (Left Ctrl through Right GUI) */
	0x05, 0x07,             /*   Usage Page (Keyboard/Keypad) */
	0x19, 0xE0,             /*   Usage Minimum (Left Control) */
	0x29, 0xE7,             /*   Usage Maximum (Right GUI) */
	0x15, 0x00,             /*   Logical Minimum (0) */
	0x25, 0x01,             /*   Logical Maximum (1) */
	0x75, 0x01,             /*   Report Size (1) */
	0x95, 0x08,             /*   Report Count (8) */
	0x81, 0x02,             /*   Input (Data,Var,Abs) */

	/* Reserved byte */
	0x75, 0x08,             /*   Report Size (8) */
	0x95, 0x01,             /*   Report Count (1) */
	0x81, 0x01,             /*   Input (Cnst,Arr,Abs) */

	/* Keycodes (6 keys, array) */
	0x05, 0x07,             /*   Usage Page (Keyboard/Keypad) */
	0x19, 0x00,             /*   Usage Minimum (0) */
	0x29, 0x65,             /*   Usage Maximum (101) */
	0x15, 0x00,             /*   Logical Minimum (0) */
	0x25, 0x65,             /*   Logical Maximum (101) */
	0x75, 0x08,             /*   Report Size (8) */
	0x95, 0x06,             /*   Report Count (6) */
	0x81, 0x00,             /*   Input (Data,Arr,Abs) */

	/* LED output report (5 bits + 3 padding) */
	0x05, 0x08,             /*   Usage Page (LEDs) */
	0x19, 0x01,             /*   Usage Minimum (Num Lock) */
	0x29, 0x05,             /*   Usage Maximum (Kana) */
	0x15, 0x00,             /*   Logical Minimum (0) */
	0x25, 0x01,             /*   Logical Maximum (1) */
	0x75, 0x01,             /*   Report Size (1) */
	0x95, 0x05,             /*   Report Count (5) */
	0x91, 0x02,             /*   Output (Data,Var,Abs) */
	0x75, 0x03,             /*   Report Size (3) */
	0x95, 0x01,             /*   Report Count (1) */
	0x91, 0x01,             /*   Output (Cnst,Arr,Abs) */

	0xC0,                   /* End Collection */

	/* ===== Consumer (Report ID 3) — 25 bytes ===== */
	0x05, 0x0C,             /* Usage Page (Consumer) */
	0x09, 0x01,             /* Usage (Consumer Control) */
	0xA1, 0x01,             /* Collection (Application) */
	0x85, 0x03,             /*   Report ID (3) */
	0x15, 0x00,             /*   Logical Minimum (0) */
	0x26, 0xFF, 0x02,       /*   Logical Maximum (0x02FF) */
	0x19, 0x00,             /*   Usage Minimum (0) */
	0x2A, 0xFF, 0x02,       /*   Usage Maximum (0x02FF) */
	0x75, 0x10,             /*   Report Size (16) */
	0x95, 0x01,             /*   Report Count (1) */
	0x81, 0x00,             /*   Input (Data,Arr,Abs) */
	0xC0,                   /* End Collection */
};

/* ---- Initialization --------------------------------------------------- */

void hids_state_init(hids_state_t *state) {
	if (state == NULL) return;
	memset(state, 0, sizeof(*state));
	state->hvx.credits = HIDS_HVX_CREDITS_MAX;
}

/* ---- Modifier refcount ----------------------------------------------- */

bool hids_modifier_press(hids_modifier_state_t *mods, uint8_t mod_bit, hids_src_t src) {
	if (mods == NULL || mod_bit > 7u || src >= HIDS_MAX_SOURCES) return false;
	uint8_t old = mods->mods[mod_bit].source_bits;
	mods->mods[mod_bit].source_bits |= HIDS_SRC_BIT(src);
	return mods->mods[mod_bit].source_bits != old;
}

bool hids_modifier_release(hids_modifier_state_t *mods, uint8_t mod_bit, hids_src_t src) {
	if (mods == NULL || mod_bit > 7u || src >= HIDS_MAX_SOURCES) return false;
	uint8_t old = mods->mods[mod_bit].source_bits;
	mods->mods[mod_bit].source_bits &= (uint8_t)~HIDS_SRC_BIT(src);
	return mods->mods[mod_bit].source_bits != old;
}

uint8_t hids_modifier_build(const hids_modifier_state_t *mods) {
	if (mods == NULL) return 0;
	uint8_t byte = 0;
	for (uint8_t i = 0; i < 8u; i++) {
		if (mods->mods[i].source_bits != 0) {
			byte |= (1u << i);
		}
	}
	return byte;
}

/* ---- Keyboard keycode refcount --------------------------------------- */

bool hids_keyboard_press(hids_keyboard_state_t *kb, uint8_t keycode, hids_src_t src) {
	if (kb == NULL || keycode == 0u || src >= HIDS_MAX_SOURCES) return false;

	/* If this keycode is already pressed, just increment its refcount. */
	for (uint8_t i = 0; i < HIDS_MAX_KB_KEYS; i++) {
		if (kb->slots[i].keycode == keycode) {
			uint8_t old = kb->slots[i].source_bits;
			kb->slots[i].source_bits |= HIDS_SRC_BIT(src);
			return kb->slots[i].source_bits != old;
		}
	}

	/* Find an empty slot. */
	for (uint8_t i = 0; i < HIDS_MAX_KB_KEYS; i++) {
		if (kb->slots[i].keycode == 0u) {
			kb->slots[i].keycode = keycode;
			kb->slots[i].source_bits = HIDS_SRC_BIT(src);
			return true;
		}
	}

	/* All 6 slots full with different keycodes — 7th is rejected. */
	kb->rejected_count++;
	return false;
}

bool hids_keyboard_release(hids_keyboard_state_t *kb, uint8_t keycode, hids_src_t src) {
	if (kb == NULL || keycode == 0u || src >= HIDS_MAX_SOURCES) return false;

	for (uint8_t i = 0; i < HIDS_MAX_KB_KEYS; i++) {
		if (kb->slots[i].keycode == keycode) {
			uint8_t old = kb->slots[i].source_bits;
			kb->slots[i].source_bits &= (uint8_t)~HIDS_SRC_BIT(src);
			if (kb->slots[i].source_bits == 0u) {
				/* Fully released — clear the slot. */
				kb->slots[i].keycode = 0u;
			}
			return kb->slots[i].source_bits != old;
		}
	}
	return false;
}

uint8_t hids_keyboard_build_keys(const hids_keyboard_state_t *kb, uint8_t out_keys[6]) {
	if (kb == NULL || out_keys == NULL) return 0;
	uint8_t count = 0;
	memset(out_keys, 0, 6);
	for (uint8_t i = 0; i < HIDS_MAX_KB_KEYS; i++) {
		if (kb->slots[i].keycode != 0u) {
			out_keys[count] = kb->slots[i].keycode;
			count++;
		}
	}
	return count;
}

uint8_t hids_keyboard_active_count(const hids_keyboard_state_t *kb) {
	if (kb == NULL) return 0;
	uint8_t count = 0;
	for (uint8_t i = 0; i < HIDS_MAX_KB_KEYS; i++) {
		if (kb->slots[i].keycode != 0u) count++;
	}
	return count;
}

/* ---- Mouse button refcount ------------------------------------------- */

bool hids_mouse_button_press(hids_mouse_button_state_t *btn, uint8_t button_idx, hids_src_t src) {
	/* button_idx is 1-8 (HID button numbering). */
	if (btn == NULL || button_idx < 1u || button_idx > 8u || src >= HIDS_MAX_SOURCES) return false;
	uint8_t idx = (uint8_t)(button_idx - 1u);
	uint8_t old = btn->source_bits[idx];
	btn->source_bits[idx] |= HIDS_SRC_BIT(src);
	return btn->source_bits[idx] != old;
}

bool hids_mouse_button_release(hids_mouse_button_state_t *btn, uint8_t button_idx, hids_src_t src) {
	if (btn == NULL || button_idx < 1u || button_idx > 8u || src >= HIDS_MAX_SOURCES) return false;
	uint8_t idx = (uint8_t)(button_idx - 1u);
	uint8_t old = btn->source_bits[idx];
	btn->source_bits[idx] &= (uint8_t)~HIDS_SRC_BIT(src);
	return btn->source_bits[idx] != old;
}

uint8_t hids_mouse_button_build(const hids_mouse_button_state_t *btn) {
	if (btn == NULL) return 0;
	uint8_t byte = 0;
	for (uint8_t i = 0; i < 8u; i++) {
		if (btn->source_bits[i] != 0) {
			byte |= (1u << i);
		}
	}
	return byte;
}

/* ---- Consumer usage refcount ----------------------------------------- */

bool hids_consumer_press(hids_consumer_state_t *con, uint16_t usage, hids_src_t src) {
	if (con == NULL || src >= HIDS_MAX_SOURCES) return false;

	/* If usage == 0 (release), treat as release of whatever this source has. */
	if (usage == HIDS_CONSUMER_NONE) {
		return false;
	}

	/* Check if this usage is already tracked. */
	for (uint8_t i = 0; i < con->active_count; i++) {
		if (con->refs[i].usage == usage) {
			uint8_t old = con->refs[i].source_bits;
			con->refs[i].source_bits |= HIDS_SRC_BIT(src);
			return con->refs[i].source_bits != old;
		}
	}

	/* New usage. Check if we have room. */
	if (con->active_count >= HIDS_MAX_CONSUMER_REFS) {
		return false;
	}

	con->refs[con->active_count].usage = usage;
	con->refs[con->active_count].source_bits = HIDS_SRC_BIT(src);
	con->active_count++;
	return true;
}

bool hids_consumer_release(hids_consumer_state_t *con, uint16_t usage, hids_src_t src) {
	if (con == NULL || src >= HIDS_MAX_SOURCES) return false;

	for (uint8_t i = 0; i < con->active_count; i++) {
		if (con->refs[i].usage == usage) {
			con->refs[i].source_bits &= (uint8_t)~HIDS_SRC_BIT(src);
			if (con->refs[i].source_bits == 0u) {
				/* Fully released — remove from active list (swap with last). */
				con->active_count--;
				if (i < con->active_count) {
					con->refs[i] = con->refs[con->active_count];
				}
				con->refs[con->active_count].usage = 0u;
				con->refs[con->active_count].source_bits = 0u;
			}
			return true;
		}
	}
	return false;
}

uint16_t hids_consumer_build(const hids_consumer_state_t *con) {
	if (con == NULL || con->active_count == 0u) return HIDS_CONSUMER_NONE;
	/* Return the first active usage. */
	return con->refs[0].usage;
}

/* ---- Mouse movement --------------------------------------------------- */

void hids_mouse_set_move(hids_state_t *state, int16_t x, int16_t y, int8_t wheel, int8_t pan) {
	if (state == NULL) return;
	state->mouse_move.x = x;
	state->mouse_move.y = y;
	state->mouse_move.wheel = wheel;
	state->mouse_move.pan = pan;
	hids_hvx_mark_dirty(&state->hvx, HIDS_REPORT_ID_MOUSE);
}

/* ---- Report building -------------------------------------------------- */

void hids_build_mouse_report(const hids_state_t *state, uint8_t *out) {
	if (state == NULL || out == NULL) return;
	out[0] = hids_mouse_button_build(&state->mouse_buttons);
	/* X (int16 little-endian) */
	out[1] = (uint8_t)(state->mouse_move.x & 0xFF);
	out[2] = (uint8_t)((state->mouse_move.x >> 8) & 0xFF);
	/* Y (int16 little-endian) */
	out[3] = (uint8_t)(state->mouse_move.y & 0xFF);
	out[4] = (uint8_t)((state->mouse_move.y >> 8) & 0xFF);
	/* Wheel (int8) */
	out[5] = (uint8_t)state->mouse_move.wheel;
	/* Pan (int8) */
	out[6] = (uint8_t)state->mouse_move.pan;
}

void hids_build_keyboard_report(const hids_state_t *state, uint8_t *out) {
	if (state == NULL || out == NULL) return;
	out[0] = hids_modifier_build(&state->modifiers);
	out[1] = 0x00;  /* reserved */
	hids_keyboard_build_keys(&state->keyboard, &out[2]);
}

void hids_build_consumer_report(const hids_state_t *state, uint8_t *out) {
	if (state == NULL || out == NULL) return;
	uint16_t usage = hids_consumer_build(&state->consumer);
	out[0] = (uint8_t)(usage & 0xFF);
	out[1] = (uint8_t)((usage >> 8) & 0xFF);
}

/* ---- HVX pipeline ----------------------------------------------------- */

void hids_hvx_init(hids_hvx_state_t *hvx) {
	if (hvx == NULL) return;
	hvx->credits = HIDS_HVX_CREDITS_MAX;
	hvx->dirty_mask = 0u;
}

void hids_hvx_mark_dirty(hids_hvx_state_t *hvx, uint8_t report_id) {
	if (hvx == NULL) return;
	hvx->dirty_mask |= HIDS_DIRTY_BIT(report_id);
}

uint8_t hids_hvx_next_send(const hids_hvx_state_t *hvx) {
	if (hvx == NULL || hvx->credits == 0u || hvx->dirty_mask == 0u) {
		return 0u;
	}
	/* Priority: consumer (bit 2) > keyboard (bit 1) > mouse (bit 0). */
	if (hvx->dirty_mask & HIDS_DIRTY_BIT_CONSUMER) return HIDS_REPORT_ID_CONSUMER;
	if (hvx->dirty_mask & HIDS_DIRTY_BIT_KEYBOARD) return HIDS_REPORT_ID_KEYBOARD;
	if (hvx->dirty_mask & HIDS_DIRTY_BIT_MOUSE) return HIDS_REPORT_ID_MOUSE;
	return 0u;
}

void hids_hvx_on_sent(hids_hvx_state_t *hvx, uint8_t report_id) {
	if (hvx == NULL || report_id == 0u) return;
	if (hvx->credits > 0u) {
		hvx->credits--;
	}
	hvx->dirty_mask &= (uint8_t)~HIDS_DIRTY_BIT(report_id);
}

bool hids_hvx_on_tx_complete(hids_hvx_state_t *hvx) {
	if (hvx == NULL) return false;
	if (hvx->credits < HIDS_HVX_CREDITS_MAX) {
		hvx->credits++;
	}
	return hids_hvx_has_pending(hvx);
}

bool hids_hvx_has_pending(const hids_hvx_state_t *hvx) {
	if (hvx == NULL) return false;
	return hvx->credits > 0u && hvx->dirty_mask != 0u;
}

bool hids_hvx_is_backpressured(const hids_hvx_state_t *hvx) {
	if (hvx == NULL) return false;
	return hvx->credits == 0u && hvx->dirty_mask != 0u;
}

/* ---- Report Map validation ------------------------------------------- */

uint16_t hids_report_map_size(void) {
	return HIDS_REPORT_MAP_SIZE;
}

bool hids_report_map_validate(const uint8_t *map, uint16_t len) {
	if (map == NULL || len != HIDS_REPORT_MAP_SIZE) return false;
	return memcmp(map, hids_report_map, HIDS_REPORT_MAP_SIZE) == 0;
}

uint8_t hids_report_ref_type(uint8_t report_id) {
	switch (report_id) {
	case HIDS_REPORT_ID_MOUSE:
	case HIDS_REPORT_ID_CONSUMER:
		return HIDS_REPORT_TYPE_INPUT;
	case HIDS_REPORT_ID_KEYBOARD:
		return HIDS_REPORT_TYPE_INPUT;
	/* Keyboard LED is an output report but shares Report ID 2.
	 * The Report Reference descriptor for the LED output uses the
	 * same Report ID (2) with type OUTPUT. */
	default:
		return HIDS_REPORT_TYPE_INPUT;
	}
}
