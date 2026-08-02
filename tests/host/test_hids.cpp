/*
 * test_hids.cpp - Host tests for HIDS eval layer (Todo 21).
 *
 * Tests:
 *   - Report map byte-exact match against golden vectors
 *   - Report sizes (mouse=7, keyboard=8, LED=1, consumer=2)
 *   - Report ID placement at expected offsets in the map
 *   - HID Info, permissions, UUID constants
 *   - Consumer usage constants (Menu Pick 0x41, AC Home 0x223, etc.)
 *   - Modifier bitmask values
 *   - Refcount: same key from 3 sources, release in reverse/same order
 *   - 7-key rejection (7th keycode rejected)
 *   - Mouse button refcount from multiple sources
 *   - Consumer usage refcount
 *   - HVX backpressure: credit=0 → dirty stays, credit restored → flush
 *   - HVX priority: consumer > keyboard > mouse
 *   - Guaranteed release under backpressure
 *   - Report builders produce correct bytes
 */
#include "test_framework.h"
#include "hids_eval.h"
#include <string.h>

/* ---- Report map golden vectors --------------------------------------- */

/* Verify specific bytes at known offsets. */
static void test_report_map_total_size(void) {
	TEST_ASSERT_EQ_INT(HIDS_REPORT_MAP_SIZE, hids_report_map_size());
	TEST_ASSERT_EQ_INT(169, (int)HIDS_REPORT_MAP_SIZE);
}

static void test_report_map_mouse_section_header(void) {
	/* Bytes 0-7: Usage Page GD, Usage Mouse, Collection App, Report ID 1 */
	TEST_ASSERT_EQ_INT(0x05, (int)hids_report_map[0]);  /* Usage Page */
	TEST_ASSERT_EQ_INT(0x01, (int)hids_report_map[1]);  /* Generic Desktop */
	TEST_ASSERT_EQ_INT(0x09, (int)hids_report_map[2]);  /* Usage */
	TEST_ASSERT_EQ_INT(0x02, (int)hids_report_map[3]);  /* Mouse */
	TEST_ASSERT_EQ_INT(0xA1, (int)hids_report_map[4]);  /* Collection */
	TEST_ASSERT_EQ_INT(0x01, (int)hids_report_map[5]);  /* Application */
	TEST_ASSERT_EQ_INT(0x85, (int)hids_report_map[6]);  /* Report ID */
	TEST_ASSERT_EQ_INT(0x01, (int)hids_report_map[7]);  /* ID = 1 */
}

static void test_report_map_mouse_buttons_8(void) {
	/* Buttons at offset 12-27: Usage Page Button, Min 1, Max 8, size 1, count 8 */
	TEST_ASSERT_EQ_INT(0x05, (int)hids_report_map[12]); /* Usage Page */
	TEST_ASSERT_EQ_INT(0x09, (int)hids_report_map[13]); /* Button */
	TEST_ASSERT_EQ_INT(0x19, (int)hids_report_map[14]); /* Usage Min */
	TEST_ASSERT_EQ_INT(0x01, (int)hids_report_map[15]); /* 1 */
	TEST_ASSERT_EQ_INT(0x29, (int)hids_report_map[16]); /* Usage Max */
	TEST_ASSERT_EQ_INT(0x08, (int)hids_report_map[17]); /* 8 */
}


static void test_report_map_mouse_wheel_and_pan(void) {
	/* Find the wheel usage (0x09 0x38) within the mouse section.
	 * Mouse section is bytes 0-74. Scan for it. */
	bool found_wheel = false;
	bool found_ac_pan = false;
	for (int i = 0; i < 75; i++) {
		if (hids_report_map[i] == 0x09 && hids_report_map[i+1] == 0x38) {
			found_wheel = true;
		}
		/* AC Pan is Usage extended: 0x0A, 0x38, 0x02 */
		if (hids_report_map[i] == 0x0A && hids_report_map[i+1] == 0x38
		    && hids_report_map[i+2] == 0x02) {
			found_ac_pan = true;
		}
	}
	TEST_ASSERT(found_wheel, "mouse wheel usage (0x09 0x38) must be present");
	TEST_ASSERT(found_ac_pan, "AC Pan usage (0x0A 0x38 0x02) must be present");
}

static void test_report_map_mouse_end_collections(void) {
	/* Mouse ends at byte 73-74: two End Collection (0xC0). */
	TEST_ASSERT_EQ_INT(0xC0, (int)hids_report_map[73]);
	TEST_ASSERT_EQ_INT(0xC0, (int)hids_report_map[74]);
}

static void test_report_map_keyboard_section_header(void) {
	/* Keyboard starts at byte 75. */
	TEST_ASSERT_EQ_INT(0x05, (int)hids_report_map[75]); /* Usage Page */
	TEST_ASSERT_EQ_INT(0x01, (int)hids_report_map[76]); /* Generic Desktop */
	TEST_ASSERT_EQ_INT(0x09, (int)hids_report_map[77]); /* Usage */
	TEST_ASSERT_EQ_INT(0x06, (int)hids_report_map[78]); /* Keyboard */
	TEST_ASSERT_EQ_INT(0xA1, (int)hids_report_map[79]); /* Collection */
	TEST_ASSERT_EQ_INT(0x01, (int)hids_report_map[80]); /* Application */
	TEST_ASSERT_EQ_INT(0x85, (int)hids_report_map[81]); /* Report ID */
	TEST_ASSERT_EQ_INT(0x02, (int)hids_report_map[82]); /* ID = 2 */
}

static void test_report_map_keyboard_modifiers(void) {
	/* Modifiers at offset 83-98: Keyboard/Keypad page, Min E0, Max E7 */
	TEST_ASSERT_EQ_INT(0x05, (int)hids_report_map[83]); /* Usage Page */
	TEST_ASSERT_EQ_INT(0x07, (int)hids_report_map[84]); /* Keyboard/Keypad */
	TEST_ASSERT_EQ_INT(0x19, (int)hids_report_map[85]); /* Usage Min */
	TEST_ASSERT_EQ_INT(0xE0, (int)hids_report_map[86]); /* Left Control */
	TEST_ASSERT_EQ_INT(0x29, (int)hids_report_map[87]); /* Usage Max */
	TEST_ASSERT_EQ_INT(0xE7, (int)hids_report_map[88]); /* Right GUI */
}

static void test_report_map_keyboard_6_keys(void) {
	/* 6-key array: find Report Count (0x95) = 6 in keyboard section (75-143). */
	bool found_6 = false;
	for (int i = 75; i < 144; i++) {
		if (hids_report_map[i] == 0x95 && hids_report_map[i+1] == 0x06) {
			found_6 = true;
		}
	}
	TEST_ASSERT(found_6, "keyboard must have Report Count = 6 for keycodes");
}

static void test_report_map_keyboard_led_output(void) {
	/* LED output: Usage Page LEDs (0x05 0x08) and Output (0x91) in keyboard section. */
	bool found_led_page = false;
	bool found_output = false;
	for (int i = 75; i < 144; i++) {
		if (hids_report_map[i] == 0x05 && hids_report_map[i+1] == 0x08) {
			found_led_page = true;
		}
		if (hids_report_map[i] == 0x91) {
			found_output = true;
		}
	}
	TEST_ASSERT(found_led_page, "LED usage page must be present in keyboard section");
	TEST_ASSERT(found_output, "Output item must be present in keyboard section");
}

static void test_report_map_consumer_section_header(void) {
	/* Consumer starts at byte 144 (75 + 69). */
	TEST_ASSERT_EQ_INT(0x05, (int)hids_report_map[144]); /* Usage Page */
	TEST_ASSERT_EQ_INT(0x0C, (int)hids_report_map[145]); /* Consumer */
	TEST_ASSERT_EQ_INT(0x09, (int)hids_report_map[146]); /* Usage */
	TEST_ASSERT_EQ_INT(0x01, (int)hids_report_map[147]); /* Consumer Control */
	TEST_ASSERT_EQ_INT(0xA1, (int)hids_report_map[148]); /* Collection */
	TEST_ASSERT_EQ_INT(0x01, (int)hids_report_map[149]); /* Application */
	TEST_ASSERT_EQ_INT(0x85, (int)hids_report_map[150]); /* Report ID */
	TEST_ASSERT_EQ_INT(0x03, (int)hids_report_map[151]); /* ID = 3 */
}

static void test_report_map_consumer_16bit(void) {
	/* Consumer: Report Size 16 (0x75 0x10), Report Count 1 (0x95 0x01). */
	bool found_size_16 = false;
	bool found_count_1 = false;
	for (int i = 144; i < 169; i++) {
		if (hids_report_map[i] == 0x75 && hids_report_map[i+1] == 0x10) {
			found_size_16 = true;
		}
		if (hids_report_map[i] == 0x95 && hids_report_map[i+1] == 0x01) {
			found_count_1 = true;
		}
	}
	TEST_ASSERT(found_size_16, "consumer must have Report Size = 16");
	TEST_ASSERT(found_count_1, "consumer must have Report Count = 1");
}

static void test_report_map_last_byte_is_end_collection(void) {
	TEST_ASSERT_EQ_INT(0xC0, (int)hids_report_map[168]);
}

static void test_report_map_validate_self(void) {
	TEST_ASSERT(hids_report_map_validate(hids_report_map, HIDS_REPORT_MAP_SIZE),
	            "report map must validate against itself");
}

static void test_report_map_validate_rejects_null(void) {
	TEST_ASSERT(!hids_report_map_validate(NULL, HIDS_REPORT_MAP_SIZE),
	            "null map must fail validation");
}

static void test_report_map_validate_rejects_wrong_length(void) {
	TEST_ASSERT(!hids_report_map_validate(hids_report_map, 100),
	            "wrong length must fail validation");
}

static void test_report_map_validate_rejects_corrupted(void) {
	uint8_t bad_map[HIDS_REPORT_MAP_SIZE];
	memcpy(bad_map, hids_report_map, HIDS_REPORT_MAP_SIZE);
	bad_map[0] = 0xFF;  /* corrupt first byte */
	TEST_ASSERT(!hids_report_map_validate(bad_map, HIDS_REPORT_MAP_SIZE),
	            "corrupted map must fail validation");
}

/* ---- Report sizes ---------------------------------------------------- */

static void test_report_sizes(void) {
	TEST_ASSERT_EQ_INT(7, (int)HIDS_MOUSE_REPORT_SIZE);
	TEST_ASSERT_EQ_INT(8, (int)HIDS_KB_INPUT_REPORT_SIZE);
	TEST_ASSERT_EQ_INT(1, (int)HIDS_KB_LED_REPORT_SIZE);
	TEST_ASSERT_EQ_INT(2, (int)HIDS_CONSUMER_REPORT_SIZE);
}

/* ---- HID Info and UUID constants ------------------------------------- */

static void test_hids_uuid(void) {
	TEST_ASSERT_EQ_INT(0x1812, (int)HIDS_SERVICE_UUID);
}

static void test_hids_info_bcd_version(void) {
	TEST_ASSERT_EQ_INT(0x0101, (int)HIDS_BCD_HID);
}

static void test_hids_info_flags_normally_connectable(void) {
	TEST_ASSERT(HIDS_INFO_FLAGS & HIDS_INFO_FLAGS_NORMALLY_CONNECTABLE,
	            "normally connectable flag must be set");
}

static void test_hids_info_flags_no_remote_wake(void) {
	TEST_ASSERT(!(HIDS_INFO_FLAGS & HIDS_INFO_FLAGS_REMOTE_WAKE),
	            "remote wake flag must NOT be set (not implemented)");
	TEST_ASSERT_EQ_INT(0x02, (int)HIDS_INFO_FLAGS);
}

/* ---- Permissions ----------------------------------------------------- */

static void test_hids_permissions_encrypted(void) {
	TEST_ASSERT_EQ_INT(1, (int)HIDS_PERM_SM);  /* SEC_MODE_1 */
	TEST_ASSERT_EQ_INT(3, (int)HIDS_PERM_LV);  /* ENC_NO_MITM */
}

/* ---- Report Reference types ------------------------------------------ */

static void test_report_ref_types(void) {
	TEST_ASSERT_EQ_INT(HIDS_REPORT_TYPE_INPUT, (int)hids_report_ref_type(HIDS_REPORT_ID_MOUSE));
	TEST_ASSERT_EQ_INT(HIDS_REPORT_TYPE_INPUT, (int)hids_report_ref_type(HIDS_REPORT_ID_KEYBOARD));
	TEST_ASSERT_EQ_INT(HIDS_REPORT_TYPE_INPUT, (int)hids_report_ref_type(HIDS_REPORT_ID_CONSUMER));
	TEST_ASSERT_EQ_INT(1, (int)HIDS_REPORT_TYPE_INPUT);
	TEST_ASSERT_EQ_INT(2, (int)HIDS_REPORT_TYPE_OUTPUT);
	TEST_ASSERT_EQ_INT(3, (int)HIDS_REPORT_TYPE_FEATURE);
}

/* ---- Consumer usage constants ---------------------------------------- */

static void test_consumer_usage_constants(void) {
	TEST_ASSERT_EQ_INT(0x0000, (int)HIDS_CONSUMER_NONE);
	TEST_ASSERT_EQ_INT(0x0041, (int)HIDS_CONSUMER_MENU_PICK);
	TEST_ASSERT_EQ_INT(0x0042, (int)HIDS_CONSUMER_MENU_UP);
	TEST_ASSERT_EQ_INT(0x0043, (int)HIDS_CONSUMER_MENU_DOWN);
	TEST_ASSERT_EQ_INT(0x0044, (int)HIDS_CONSUMER_MENU_LEFT);
	TEST_ASSERT_EQ_INT(0x0045, (int)HIDS_CONSUMER_MENU_RIGHT);
	TEST_ASSERT_EQ_INT(0x0046, (int)HIDS_CONSUMER_MENU_ESCAPE);
	TEST_ASSERT_EQ_INT(0x0221, (int)HIDS_CONSUMER_AC_SEARCH);
	TEST_ASSERT_EQ_INT(0x0223, (int)HIDS_CONSUMER_AC_HOME);
	TEST_ASSERT_EQ_INT(0x0224, (int)HIDS_CONSUMER_AC_BACK);
	TEST_ASSERT_EQ_INT(0x00E9, (int)HIDS_CONSUMER_VOL_UP);
	TEST_ASSERT_EQ_INT(0x00EA, (int)HIDS_CONSUMER_VOL_DOWN);
	TEST_ASSERT_EQ_INT(0x00E2, (int)HIDS_CONSUMER_MUTE);
	TEST_ASSERT_EQ_INT(0x00CD, (int)HIDS_CONSUMER_PLAY_PAUSE);
}

static void test_consumer_logical_max_covers_all_usages(void) {
	TEST_ASSERT(HIDS_CONSUMER_LOGICAL_MAX >= HIDS_CONSUMER_AC_BACK,
	            "logical max must cover AC Back (0x0224)");
	TEST_ASSERT(HIDS_CONSUMER_LOGICAL_MAX >= HIDS_CONSUMER_AC_SEARCH,
	            "logical max must cover AC Search (0x0221)");
	TEST_ASSERT_EQ_INT(0x02FF, (int)HIDS_CONSUMER_LOGICAL_MAX);
}

/* ---- Modifier bitmask constants -------------------------------------- */

static void test_modifier_bitmask_values(void) {
	TEST_ASSERT_EQ_INT(0x01, (int)HIDS_MOD_LCTRL);
	TEST_ASSERT_EQ_INT(0x02, (int)HIDS_MOD_LSHIFT);
	TEST_ASSERT_EQ_INT(0x04, (int)HIDS_MOD_LALT);
	TEST_ASSERT_EQ_INT(0x08, (int)HIDS_MOD_LGUI);
	TEST_ASSERT_EQ_INT(0x10, (int)HIDS_MOD_RCTRL);
	TEST_ASSERT_EQ_INT(0x20, (int)HIDS_MOD_RSHIFT);
	TEST_ASSERT_EQ_INT(0x40, (int)HIDS_MOD_RALT);
	TEST_ASSERT_EQ_INT(0x80, (int)HIDS_MOD_RGUI);
}

/* ---- Modifier refcount tests ----------------------------------------- */

static void test_modifier_press_single_source(void) {
	hids_modifier_state_t mods;
	memset(&mods, 0, sizeof(mods));

	bool changed = hids_modifier_press(&mods, 0, HIDS_SRC_BUTTON_A); /* LCTRL */
	TEST_ASSERT(changed, "first press must change state");
	TEST_ASSERT_EQ_INT(HIDS_SRC_BIT(HIDS_SRC_BUTTON_A), (int)mods.mods[0].source_bits);

	/* Same source pressing again: no change. */
	changed = hids_modifier_press(&mods, 0, HIDS_SRC_BUTTON_A);
	TEST_ASSERT(!changed, "same source pressing again must not change state");
}

static void test_modifier_release_single_source(void) {
	hids_modifier_state_t mods;
	memset(&mods, 0, sizeof(mods));

	hids_modifier_press(&mods, 0, HIDS_SRC_BUTTON_A);
	bool changed = hids_modifier_release(&mods, 0, HIDS_SRC_BUTTON_A);
	TEST_ASSERT(changed, "release must change state");
	TEST_ASSERT_EQ_INT(0, (int)mods.mods[0].source_bits);
	TEST_ASSERT_EQ_INT(0, (int)hids_modifier_build(&mods));
}

static void test_modifier_multi_source_release_order(void) {
	hids_modifier_state_t mods;
	memset(&mods, 0, sizeof(mods));

	/* Three sources press the same modifier. */
	hids_modifier_press(&mods, 0, HIDS_SRC_BUTTON_A);
	hids_modifier_press(&mods, 0, HIDS_SRC_BUTTON_B);
	hids_modifier_press(&mods, 0, HIDS_SRC_AIR_MOUSE);

	/* Modifier should be active (build returns bit 0). */
	TEST_ASSERT_EQ_INT(0x01, (int)hids_modifier_build(&mods));

	/* Release in REVERSE order: A first. */
	hids_modifier_release(&mods, 0, HIDS_SRC_BUTTON_A);
	TEST_ASSERT(hids_modifier_build(&mods) != 0, "modifier still active after 1/3 releases");

	/* Release B. */
	hids_modifier_release(&mods, 0, HIDS_SRC_BUTTON_B);
	TEST_ASSERT(hids_modifier_build(&mods) != 0, "modifier still active after 2/3 releases");

	/* Release APDS — last one. Now modifier should be gone. */
	hids_modifier_release(&mods, 0, HIDS_SRC_AIR_MOUSE);
	TEST_ASSERT(hids_modifier_build(&mods) == 0, "modifier released after all sources release");
}

static void test_modifier_multi_source_release_same_order(void) {
	hids_modifier_state_t mods;
	memset(&mods, 0, sizeof(mods));

	/* Press in order A, B, APDS. */
	hids_modifier_press(&mods, 3, HIDS_SRC_BUTTON_A); /* LGUI */
	hids_modifier_press(&mods, 3, HIDS_SRC_BUTTON_B);
	hids_modifier_press(&mods, 3, HIDS_SRC_AIR_MOUSE);

	/* Release in SAME order: A first. */
	hids_modifier_release(&mods, 3, HIDS_SRC_BUTTON_A);
	TEST_ASSERT(hids_modifier_build(&mods) != 0, "still active after 1/3");

	hids_modifier_release(&mods, 3, HIDS_SRC_BUTTON_B);
	TEST_ASSERT(hids_modifier_build(&mods) != 0, "still active after 2/3");

	hids_modifier_release(&mods, 3, HIDS_SRC_AIR_MOUSE);
	TEST_ASSERT(hids_modifier_build(&mods) == 0, "fully released");
}

static void test_modifier_build_multiple_modifiers(void) {
	hids_modifier_state_t mods;
	memset(&mods, 0, sizeof(mods));

	hids_modifier_press(&mods, 0, HIDS_SRC_BUTTON_A); /* LCTRL = bit 0 */
	hids_modifier_press(&mods, 1, HIDS_SRC_BUTTON_A); /* LSHIFT = bit 1 */
	hids_modifier_press(&mods, 5, HIDS_SRC_BUTTON_B); /* RSHIFT = bit 5 */

	uint8_t byte = hids_modifier_build(&mods);
	TEST_ASSERT_EQ_INT(0x01 | 0x02 | 0x20, (int)byte);
}

/* ---- Keyboard keycode refcount tests --------------------------------- */

static void test_keyboard_press_and_build(void) {
	hids_keyboard_state_t kb;
	memset(&kb, 0, sizeof(kb));

	bool ok = hids_keyboard_press(&kb, 0x04, HIDS_SRC_BUTTON_A); /* 'a' */
	TEST_ASSERT(ok, "press must succeed");
	TEST_ASSERT_EQ_INT(1, (int)hids_keyboard_active_count(&kb));

	uint8_t keys[6];
	uint8_t count = hids_keyboard_build_keys(&kb, keys);
	TEST_ASSERT_EQ_INT(1, (int)count);
	TEST_ASSERT_EQ_INT(0x04, (int)keys[0]);
	TEST_ASSERT_EQ_INT(0, (int)keys[1]); /* rest zero */
}

static void test_keyboard_refcount_same_key_3_sources(void) {
	hids_keyboard_state_t kb;
	memset(&kb, 0, sizeof(kb));

	/* Press 'a' from 3 different sources. */
	hids_keyboard_press(&kb, 0x04, HIDS_SRC_BUTTON_A);
	hids_keyboard_press(&kb, 0x04, HIDS_SRC_BUTTON_B);
	hids_keyboard_press(&kb, 0x04, HIDS_SRC_AIR_MOUSE);

	/* Should occupy only 1 slot. */
	TEST_ASSERT_EQ_INT(1, (int)hids_keyboard_active_count(&kb));

	/* Release from 1 source — still active. */
	hids_keyboard_release(&kb, 0x04, HIDS_SRC_BUTTON_A);
	TEST_ASSERT_EQ_INT(1, (int)hids_keyboard_active_count(&kb));

	/* Release from 2nd source — still active. */
	hids_keyboard_release(&kb, 0x04, HIDS_SRC_BUTTON_B);
	TEST_ASSERT_EQ_INT(1, (int)hids_keyboard_active_count(&kb));

	/* Release from 3rd source — now gone. */
	hids_keyboard_release(&kb, 0x04, HIDS_SRC_AIR_MOUSE);
	TEST_ASSERT_EQ_INT(0, (int)hids_keyboard_active_count(&kb));
}

static void test_keyboard_release_reverse_order(void) {
	hids_keyboard_state_t kb;
	memset(&kb, 0, sizeof(kb));

	hids_keyboard_press(&kb, 0x05, HIDS_SRC_BUTTON_A); /* 'b' */
	hids_keyboard_press(&kb, 0x05, HIDS_SRC_BUTTON_B);
	hids_keyboard_press(&kb, 0x05, HIDS_SRC_APDS_SWIPE);

	/* Release in reverse: APDS first, then B, then A. */
	hids_keyboard_release(&kb, 0x05, HIDS_SRC_APDS_SWIPE);
	TEST_ASSERT_EQ_INT(1, (int)hids_keyboard_active_count(&kb));
	hids_keyboard_release(&kb, 0x05, HIDS_SRC_BUTTON_B);
	TEST_ASSERT_EQ_INT(1, (int)hids_keyboard_active_count(&kb));
	hids_keyboard_release(&kb, 0x05, HIDS_SRC_BUTTON_A);
	TEST_ASSERT_EQ_INT(0, (int)hids_keyboard_active_count(&kb));
}

static void test_keyboard_7th_key_rejected(void) {
	hids_keyboard_state_t kb;
	memset(&kb, 0, sizeof(kb));

	/* Fill all 6 slots with different keys. */
	for (uint8_t i = 0; i < 6; i++) {
		bool ok = hids_keyboard_press(&kb, 0x04 + i, HIDS_SRC_BUTTON_A);
		TEST_ASSERT(ok, "first 6 keys must be accepted");
	}
	TEST_ASSERT_EQ_INT(6, (int)hids_keyboard_active_count(&kb));

	/* 7th key must be rejected. */
	bool ok = hids_keyboard_press(&kb, 0x0B, HIDS_SRC_BUTTON_A);
	TEST_ASSERT(!ok, "7th key must be rejected");
	TEST_ASSERT_EQ_INT(6, (int)hids_keyboard_active_count(&kb));
	TEST_ASSERT_EQ_INT(1, (int)kb.rejected_count);

	/* Report still has 6 keys. */
	uint8_t keys[6];
	uint8_t count = hids_keyboard_build_keys(&kb, keys);
	TEST_ASSERT_EQ_INT(6, (int)count);
}

static void test_keyboard_release_then_press_succeeds(void) {
	hids_keyboard_state_t kb;
	memset(&kb, 0, sizeof(kb));

	for (uint8_t i = 0; i < 6; i++) {
		hids_keyboard_press(&kb, 0x04 + i, HIDS_SRC_BUTTON_A);
	}
	/* 7th rejected. */
	TEST_ASSERT(!hids_keyboard_press(&kb, 0x0B, HIDS_SRC_BUTTON_A), "7th key must be rejected after release+repress");

	/* Release one. */
	hids_keyboard_release(&kb, 0x04, HIDS_SRC_BUTTON_A);
	TEST_ASSERT_EQ_INT(5, (int)hids_keyboard_active_count(&kb));

	/* Now can press again. */
	bool ok = hids_keyboard_press(&kb, 0x0B, HIDS_SRC_BUTTON_A);
	TEST_ASSERT(ok, "press after release must succeed");
	TEST_ASSERT_EQ_INT(6, (int)hids_keyboard_active_count(&kb));
}

static void test_keyboard_multiple_distinct_keys(void) {
	hids_keyboard_state_t kb;
	memset(&kb, 0, sizeof(kb));

	hids_keyboard_press(&kb, 0x04, HIDS_SRC_BUTTON_A); /* 'a' */
	hids_keyboard_press(&kb, 0x06, HIDS_SRC_BUTTON_B); /* 'c' */
	hids_keyboard_press(&kb, 0x08, HIDS_SRC_AIR_MOUSE); /* 'e' */

	uint8_t keys[6];
	uint8_t count = hids_keyboard_build_keys(&kb, keys);
	TEST_ASSERT_EQ_INT(3, (int)count);
	TEST_ASSERT_EQ_INT(0x04, (int)keys[0]);
	TEST_ASSERT_EQ_INT(0x06, (int)keys[1]);
	TEST_ASSERT_EQ_INT(0x08, (int)keys[2]);
}

/* ---- Mouse button refcount tests ------------------------------------- */

static void test_mouse_button_press_release(void) {
	hids_mouse_button_state_t btn;
	memset(&btn, 0, sizeof(btn));

	hids_mouse_button_press(&btn, 1, HIDS_SRC_BUTTON_A); /* left */
	TEST_ASSERT_EQ_INT(0x01, (int)hids_mouse_button_build(&btn));

	hids_mouse_button_press(&btn, 2, HIDS_SRC_BUTTON_B); /* right */
	TEST_ASSERT_EQ_INT(0x03, (int)hids_mouse_button_build(&btn));

	hids_mouse_button_release(&btn, 1, HIDS_SRC_BUTTON_A);
	TEST_ASSERT_EQ_INT(0x02, (int)hids_mouse_button_build(&btn));
}

static void test_mouse_button_multi_source(void) {
	hids_mouse_button_state_t btn;
	memset(&btn, 0, sizeof(btn));

	/* Both sources press button 1. */
	hids_mouse_button_press(&btn, 1, HIDS_SRC_BUTTON_A);
	hids_mouse_button_press(&btn, 1, HIDS_SRC_BUTTON_B);
	TEST_ASSERT_EQ_INT(0x01, (int)hids_mouse_button_build(&btn));

	/* Release from A — B still holding. */
	hids_mouse_button_release(&btn, 1, HIDS_SRC_BUTTON_A);
	TEST_ASSERT_EQ_INT(0x01, (int)hids_mouse_button_build(&btn));

	/* Release from B — now released. */
	hids_mouse_button_release(&btn, 1, HIDS_SRC_BUTTON_B);
	TEST_ASSERT_EQ_INT(0, (int)hids_mouse_button_build(&btn));
}

static void test_mouse_button_all_8(void) {
	hids_mouse_button_state_t btn;
	memset(&btn, 0, sizeof(btn));

	for (uint8_t b = 1; b <= 8; b++) {
		hids_mouse_button_press(&btn, b, HIDS_SRC_BUTTON_A);
	}
	TEST_ASSERT_EQ_INT(0xFF, (int)hids_mouse_button_build(&btn));
}

/* ---- Consumer usage refcount tests ----------------------------------- */

static void test_consumer_press_and_build(void) {
	hids_consumer_state_t con;
	memset(&con, 0, sizeof(con));

	bool ok = hids_consumer_press(&con, HIDS_CONSUMER_VOL_UP, HIDS_SRC_BUTTON_A);
	TEST_ASSERT(ok, "press must succeed");
	TEST_ASSERT_EQ_INT(HIDS_CONSUMER_VOL_UP, (int)hids_consumer_build(&con));
}

static void test_consumer_multi_source_same_usage(void) {
	hids_consumer_state_t con;
	memset(&con, 0, sizeof(con));

	hids_consumer_press(&con, HIDS_CONSUMER_PLAY_PAUSE, HIDS_SRC_BUTTON_A);
	hids_consumer_press(&con, HIDS_CONSUMER_PLAY_PAUSE, HIDS_SRC_BUTTON_B);
	TEST_ASSERT_EQ_INT(1, (int)con.active_count);

	/* Release from A — B still holding. */
	hids_consumer_release(&con, HIDS_CONSUMER_PLAY_PAUSE, HIDS_SRC_BUTTON_A);
	TEST_ASSERT_EQ_INT(1, (int)con.active_count);
	TEST_ASSERT_EQ_INT(HIDS_CONSUMER_PLAY_PAUSE, (int)hids_consumer_build(&con));

	/* Release from B — gone. */
	hids_consumer_release(&con, HIDS_CONSUMER_PLAY_PAUSE, HIDS_SRC_BUTTON_B);
	TEST_ASSERT_EQ_INT(0, (int)con.active_count);
	TEST_ASSERT_EQ_INT(HIDS_CONSUMER_NONE, (int)hids_consumer_build(&con));
}

static void test_consumer_release_nonexistent_usage(void) {
	hids_consumer_state_t con;
	memset(&con, 0, sizeof(con));

	/* Release a usage that was never pressed. */
	bool changed = hids_consumer_release(&con, HIDS_CONSUMER_MUTE, HIDS_SRC_BUTTON_A);
	TEST_ASSERT(!changed, "releasing nonexistent usage must return false");
}

/* ---- Report builder tests -------------------------------------------- */

static void test_build_mouse_report_zeros(void) {
	hids_state_t state;
	hids_state_init(&state);

	uint8_t report[HIDS_MOUSE_REPORT_SIZE];
	hids_build_mouse_report(&state, report);

	for (uint8_t i = 0; i < HIDS_MOUSE_REPORT_SIZE; i++) {
		TEST_ASSERT_EQ_INT(0, (int)report[i]);
	}
}

static void test_build_mouse_report_with_movement(void) {
	hids_state_t state;
	hids_state_init(&state);

	hids_mouse_button_press(&state.mouse_buttons, 1, HIDS_SRC_BUTTON_A);
	hids_mouse_set_move(&state, 100, -50, 3, -1);

	uint8_t report[HIDS_MOUSE_REPORT_SIZE];
	hids_build_mouse_report(&state, report);

	TEST_ASSERT_EQ_INT(0x01, (int)report[0]); /* button 1 */
	TEST_ASSERT_EQ_INT(100, (int)report[1]);  /* X low = 0x64 */
	TEST_ASSERT_EQ_INT(0, (int)report[2]);    /* X high = 0x00 */
	TEST_ASSERT_EQ_INT(0xCE, (int)(uint8_t)report[3]);  /* Y low = -50 = 0xCE */
	TEST_ASSERT_EQ_INT(0xFF, (int)(uint8_t)report[4]);  /* Y high */
	TEST_ASSERT_EQ_INT(3, (int)(int8_t)report[5]);      /* wheel */
	TEST_ASSERT_EQ_INT(-1, (int)(int8_t)report[6]);     /* pan */
}

static void test_build_keyboard_report_zeros(void) {
	hids_state_t state;
	hids_state_init(&state);

	uint8_t report[HIDS_KB_INPUT_REPORT_SIZE];
	hids_build_keyboard_report(&state, report);

	TEST_ASSERT_EQ_INT(0, (int)report[0]); /* modifier */
	TEST_ASSERT_EQ_INT(0, (int)report[1]); /* reserved */
	for (uint8_t i = 2; i < 8; i++) {
		TEST_ASSERT_EQ_INT(0, (int)report[i]); /* no keys */
	}
}

static void test_build_keyboard_report_with_mods_and_keys(void) {
	hids_state_t state;
	hids_state_init(&state);

	hids_modifier_press(&state.modifiers, 0, HIDS_SRC_BUTTON_A); /* LCTRL */
	hids_keyboard_press(&state.keyboard, 0x04, HIDS_SRC_BUTTON_A); /* 'a' */

	uint8_t report[HIDS_KB_INPUT_REPORT_SIZE];
	hids_build_keyboard_report(&state, report);

	TEST_ASSERT_EQ_INT(HIDS_MOD_LCTRL, (int)report[0]); /* modifier = 0x01 */
	TEST_ASSERT_EQ_INT(0, (int)report[1]); /* reserved */
	TEST_ASSERT_EQ_INT(0x04, (int)report[2]); /* 'a' */
	TEST_ASSERT_EQ_INT(0, (int)report[3]); /* empty */
}

static void test_build_consumer_report_release(void) {
	hids_state_t state;
	hids_state_init(&state);

	uint8_t report[HIDS_CONSUMER_REPORT_SIZE];
	hids_build_consumer_report(&state, report);

	TEST_ASSERT_EQ_INT(0, (int)report[0]); /* usage low */
	TEST_ASSERT_EQ_INT(0, (int)report[1]); /* usage high */
}

static void test_build_consumer_report_with_usage(void) {
	hids_state_t state;
	hids_state_init(&state);

	hids_consumer_press(&state.consumer, HIDS_CONSUMER_AC_HOME, HIDS_SRC_BUTTON_A);

	uint8_t report[HIDS_CONSUMER_REPORT_SIZE];
	hids_build_consumer_report(&state, report);

	TEST_ASSERT_EQ_INT(0x23, (int)report[0]); /* AC Home low byte */
	TEST_ASSERT_EQ_INT(0x02, (int)report[1]); /* AC Home high byte */
}

/* ---- HVX pipeline tests ---------------------------------------------- */

static void test_hvx_init_full_credits(void) {
	hids_hvx_state_t hvx;
	hids_hvx_init(&hvx);

	TEST_ASSERT_EQ_INT(HIDS_HVX_CREDITS_MAX, (int)hvx.credits);
	TEST_ASSERT_EQ_INT(0, (int)hvx.dirty_mask);
	TEST_ASSERT(!hids_hvx_has_pending(&hvx), "nothing pending after init");
}

static void test_hvx_mark_dirty(void) {
	hids_hvx_state_t hvx;
	hids_hvx_init(&hvx);

	hids_hvx_mark_dirty(&hvx, HIDS_REPORT_ID_CONSUMER);
	TEST_ASSERT(hvx.dirty_mask & HIDS_DIRTY_BIT_CONSUMER, "consumer dirty bit set");
	TEST_ASSERT(hids_hvx_has_pending(&hvx), "has pending after mark dirty");
}

static void test_hvx_send_consumes_credit(void) {
	hids_hvx_state_t hvx;
	hids_hvx_init(&hvx);

	hids_hvx_mark_dirty(&hvx, HIDS_REPORT_ID_MOUSE);
	uint8_t rid = hids_hvx_next_send(&hvx);
	TEST_ASSERT_EQ_INT(HIDS_REPORT_ID_MOUSE, (int)rid);

	hids_hvx_on_sent(&hvx, rid);
	TEST_ASSERT_EQ_INT(1, (int)hvx.credits); /* consumed one */
	TEST_ASSERT(!(hvx.dirty_mask & HIDS_DIRTY_BIT_MOUSE), "mouse dirty cleared after send");
}

static void test_hvx_credit_zero_blocks_send(void) {
	hids_hvx_state_t hvx;
	hids_hvx_init(&hvx);

	/* Consume both credits. */
	hids_hvx_mark_dirty(&hvx, HIDS_REPORT_ID_MOUSE);
	hids_hvx_on_sent(&hvx, hids_hvx_next_send(&hvx));
	hids_hvx_mark_dirty(&hvx, HIDS_REPORT_ID_KEYBOARD);
	hids_hvx_on_sent(&hvx, hids_hvx_next_send(&hvx));

	TEST_ASSERT_EQ_INT(0, (int)hvx.credits);

	/* New report dirty — should be blocked. */
	hids_hvx_mark_dirty(&hvx, HIDS_REPORT_ID_CONSUMER);
	TEST_ASSERT(hids_hvx_is_backpressured(&hvx), "must be backpressured at credit 0");
	TEST_ASSERT_EQ_INT(0, (int)hids_hvx_next_send(&hvx));
}

static void test_hvx_tx_complete_restores_credit(void) {
	hids_hvx_state_t hvx;
	hids_hvx_init(&hvx);

	/* Consume both credits. */
	hids_hvx_mark_dirty(&hvx, HIDS_REPORT_ID_MOUSE);
	hids_hvx_on_sent(&hvx, hids_hvx_next_send(&hvx));
	hids_hvx_mark_dirty(&hvx, HIDS_REPORT_ID_KEYBOARD);
	hids_hvx_on_sent(&hvx, hids_hvx_next_send(&hvx));
	TEST_ASSERT_EQ_INT(0, (int)hvx.credits);

	/* Mark consumer dirty while backpressured. */
	hids_hvx_mark_dirty(&hvx, HIDS_REPORT_ID_CONSUMER);

	/* TX complete restores credit and there's pending work. */
	bool pending = hids_hvx_on_tx_complete(&hvx);
	TEST_ASSERT(pending, "pending work after tx complete");
	TEST_ASSERT_EQ_INT(1, (int)hvx.credits);

	/* Now we can send consumer. */
	uint8_t rid = hids_hvx_next_send(&hvx);
	TEST_ASSERT_EQ_INT(HIDS_REPORT_ID_CONSUMER, (int)rid);
}

static void test_hvx_priority_consumer_over_keyboard(void) {
	hids_hvx_state_t hvx;
	hids_hvx_init(&hvx);

	/* Both consumer and keyboard dirty. Consumer has higher priority. */
	hids_hvx_mark_dirty(&hvx, HIDS_REPORT_ID_KEYBOARD);
	hids_hvx_mark_dirty(&hvx, HIDS_REPORT_ID_CONSUMER);

	uint8_t rid = hids_hvx_next_send(&hvx);
	TEST_ASSERT_EQ_INT(HIDS_REPORT_ID_CONSUMER, (int)rid);
}

static void test_hvx_priority_keyboard_over_mouse(void) {
	hids_hvx_state_t hvx;
	hids_hvx_init(&hvx);

	hids_hvx_mark_dirty(&hvx, HIDS_REPORT_ID_MOUSE);
	hids_hvx_mark_dirty(&hvx, HIDS_REPORT_ID_KEYBOARD);

	uint8_t rid = hids_hvx_next_send(&hvx);
	TEST_ASSERT_EQ_INT(HIDS_REPORT_ID_KEYBOARD, (int)rid);
}

static void test_hvx_full_pipeline_cycle(void) {
	hids_hvx_state_t hvx;
	hids_hvx_init(&hvx);

	/* Send report 1 (credit 2→1). */
	hids_hvx_mark_dirty(&hvx, HIDS_REPORT_ID_MOUSE);
	hids_hvx_on_sent(&hvx, hids_hvx_next_send(&hvx));
	TEST_ASSERT_EQ_INT(1, (int)hvx.credits);

	/* Send report 2 (credit 1→0). */
	hids_hvx_mark_dirty(&hvx, HIDS_REPORT_ID_KEYBOARD);
	hids_hvx_on_sent(&hvx, hids_hvx_next_send(&hvx));
	TEST_ASSERT_EQ_INT(0, (int)hvx.credits);

	/* Nothing to send. */
	TEST_ASSERT_EQ_INT(0, (int)hids_hvx_next_send(&hvx));

	/* TX complete for report 1 (credit 0→1). */
	hids_hvx_on_tx_complete(&hvx);
	TEST_ASSERT_EQ_INT(1, (int)hvx.credits);

	/* TX complete for report 2 (credit 1→2). */
	hids_hvx_on_tx_complete(&hvx);
	TEST_ASSERT_EQ_INT(2, (int)hvx.credits);
}

/* ---- Guaranteed release under backpressure --------------------------- */

static void test_guaranteed_release_under_backpressure(void) {
	hids_state_t state;
	hids_state_init(&state);

	/* Consume both credits. */
	hids_hvx_mark_dirty(&state.hvx, HIDS_REPORT_ID_MOUSE);
	hids_hvx_on_sent(&state.hvx, hids_hvx_next_send(&state.hvx));
	hids_hvx_mark_dirty(&state.hvx, HIDS_REPORT_ID_KEYBOARD);
	hids_hvx_on_sent(&state.hvx, hids_hvx_next_send(&state.hvx));

	/* Now backpressured (credit=0). Press and release consumer key. */
	hids_consumer_press(&state.consumer, HIDS_CONSUMER_VOL_UP, HIDS_SRC_BUTTON_A);
	hids_hvx_mark_dirty(&state.hvx, HIDS_REPORT_ID_CONSUMER);

	/* Release the consumer key while still backpressured. */
	hids_consumer_release(&state.consumer, HIDS_CONSUMER_VOL_UP, HIDS_SRC_BUTTON_A);

	/* The dirty bit is still set (was marked when pressed). */
	TEST_ASSERT(state.hvx.dirty_mask & HIDS_DIRTY_BIT_CONSUMER,
	            "consumer dirty must persist under backpressure");

	/* TX complete restores credit. */
	hids_hvx_on_tx_complete(&state.hvx);

	/* Now we can flush — the report will show RELEASED state (0x0000). */
	uint8_t rid = hids_hvx_next_send(&state.hvx);
	TEST_ASSERT_EQ_INT(HIDS_REPORT_ID_CONSUMER, (int)rid);

	uint8_t report[HIDS_CONSUMER_REPORT_SIZE];
	hids_build_consumer_report(&state, report);
	TEST_ASSERT_EQ_INT(0, (int)report[0]); /* usage = 0 (released) */
	TEST_ASSERT_EQ_INT(0, (int)report[1]);

	/* After send, dirty is cleared. */
	hids_hvx_on_sent(&state.hvx, rid);
	TEST_ASSERT(!(state.hvx.dirty_mask & HIDS_DIRTY_BIT_CONSUMER),
	            "dirty cleared after send");
}

static void test_guaranteed_release_keyboard_under_backpressure(void) {
	hids_state_t state;
	hids_state_init(&state);

	/* Fill credits. */
	hids_hvx_mark_dirty(&state.hvx, HIDS_REPORT_ID_MOUSE);
	hids_hvx_on_sent(&state.hvx, hids_hvx_next_send(&state.hvx));
	hids_hvx_mark_dirty(&state.hvx, HIDS_REPORT_ID_CONSUMER);
	hids_hvx_on_sent(&state.hvx, hids_hvx_next_send(&state.hvx));
	TEST_ASSERT_EQ_INT(0, (int)state.hvx.credits);

	/* Press 'a' while backpressured. */
	hids_keyboard_press(&state.keyboard, 0x04, HIDS_SRC_BUTTON_A);
	hids_hvx_mark_dirty(&state.hvx, HIDS_REPORT_ID_KEYBOARD);

	/* Release 'a' while still backpressured. */
	hids_keyboard_release(&state.keyboard, 0x04, HIDS_SRC_BUTTON_A);

	/* Dirty persists. */
	TEST_ASSERT(state.hvx.dirty_mask & HIDS_DIRTY_BIT_KEYBOARD, "keyboard dirty persists under backpressure");

	/* TX complete. */
	hids_hvx_on_tx_complete(&state.hvx);
	uint8_t rid = hids_hvx_next_send(&state.hvx);
	TEST_ASSERT_EQ_INT(HIDS_REPORT_ID_KEYBOARD, (int)rid);

	/* Report shows released state (no keys). */
	uint8_t report[HIDS_KB_INPUT_REPORT_SIZE];
	hids_build_keyboard_report(&state, report);
	TEST_ASSERT_EQ_INT(0, (int)report[2]); /* no key in first slot */
}

/* ---- State init test ------------------------------------------------- */

static void test_state_init(void) {
	hids_state_t state;
	hids_state_init(&state);

	TEST_ASSERT_EQ_INT(HIDS_HVX_CREDITS_MAX, (int)state.hvx.credits);
	TEST_ASSERT_EQ_INT(0, (int)hids_modifier_build(&state.modifiers));
	TEST_ASSERT_EQ_INT(0, (int)hids_keyboard_active_count(&state.keyboard));
	TEST_ASSERT_EQ_INT(0, (int)hids_mouse_button_build(&state.mouse_buttons));
	TEST_ASSERT_EQ_INT(HIDS_CONSUMER_NONE, (int)hids_consumer_build(&state.consumer));
}

/* ---- Control Point constants ----------------------------------------- */

static void test_control_point_values(void) {
	TEST_ASSERT_EQ_INT(0, (int)HIDS_CTRL_SUSPEND);
	TEST_ASSERT_EQ_INT(1, (int)HIDS_CTRL_EXIT_SUSP);
}

/* ---- Source bitmask -------------------------------------------------- */

static void test_source_bit_values(void) {
	TEST_ASSERT_EQ_INT(0x01, (int)HIDS_SRC_BIT(HIDS_SRC_BUTTON_A));
	TEST_ASSERT_EQ_INT(0x02, (int)HIDS_SRC_BIT(HIDS_SRC_BUTTON_B));
	TEST_ASSERT_EQ_INT(0x04, (int)HIDS_SRC_BIT(HIDS_SRC_AIR_MOUSE));
	TEST_ASSERT_EQ_INT(0x08, (int)HIDS_SRC_BIT(HIDS_SRC_APDS_SWIPE));
	TEST_ASSERT_EQ_INT(4, (int)HIDS_MAX_SOURCES);
}

/* ---- Main ------------------------------------------------------------ */

int main(void) {
	test_framework_init();

	/* Report map structure */
	RUN_TEST(test_report_map_total_size);
	RUN_TEST(test_report_map_mouse_section_header);
	RUN_TEST(test_report_map_mouse_buttons_8);
	RUN_TEST(test_report_map_mouse_wheel_and_pan);
	RUN_TEST(test_report_map_mouse_end_collections);
	RUN_TEST(test_report_map_keyboard_section_header);
	RUN_TEST(test_report_map_keyboard_modifiers);
	RUN_TEST(test_report_map_keyboard_6_keys);
	RUN_TEST(test_report_map_keyboard_led_output);
	RUN_TEST(test_report_map_consumer_section_header);
	RUN_TEST(test_report_map_consumer_16bit);
	RUN_TEST(test_report_map_last_byte_is_end_collection);
	RUN_TEST(test_report_map_validate_self);
	RUN_TEST(test_report_map_validate_rejects_null);
	RUN_TEST(test_report_map_validate_rejects_wrong_length);
	RUN_TEST(test_report_map_validate_rejects_corrupted);

	/* Report sizes */
	RUN_TEST(test_report_sizes);

	/* HID Info + UUID */
	RUN_TEST(test_hids_uuid);
	RUN_TEST(test_hids_info_bcd_version);
	RUN_TEST(test_hids_info_flags_normally_connectable);
	RUN_TEST(test_hids_info_flags_no_remote_wake);

	/* Permissions */
	RUN_TEST(test_hids_permissions_encrypted);

	/* Report Reference */
	RUN_TEST(test_report_ref_types);

	/* Consumer usage constants */
	RUN_TEST(test_consumer_usage_constants);
	RUN_TEST(test_consumer_logical_max_covers_all_usages);

	/* Modifier bitmask */
	RUN_TEST(test_modifier_bitmask_values);

	/* Modifier refcount */
	RUN_TEST(test_modifier_press_single_source);
	RUN_TEST(test_modifier_release_single_source);
	RUN_TEST(test_modifier_multi_source_release_order);
	RUN_TEST(test_modifier_multi_source_release_same_order);
	RUN_TEST(test_modifier_build_multiple_modifiers);

	/* Keyboard keycode refcount */
	RUN_TEST(test_keyboard_press_and_build);
	RUN_TEST(test_keyboard_refcount_same_key_3_sources);
	RUN_TEST(test_keyboard_release_reverse_order);
	RUN_TEST(test_keyboard_7th_key_rejected);
	RUN_TEST(test_keyboard_release_then_press_succeeds);
	RUN_TEST(test_keyboard_multiple_distinct_keys);

	/* Mouse button refcount */
	RUN_TEST(test_mouse_button_press_release);
	RUN_TEST(test_mouse_button_multi_source);
	RUN_TEST(test_mouse_button_all_8);

	/* Consumer usage refcount */
	RUN_TEST(test_consumer_press_and_build);
	RUN_TEST(test_consumer_multi_source_same_usage);
	RUN_TEST(test_consumer_release_nonexistent_usage);

	/* Report builders */
	RUN_TEST(test_build_mouse_report_zeros);
	RUN_TEST(test_build_mouse_report_with_movement);
	RUN_TEST(test_build_keyboard_report_zeros);
	RUN_TEST(test_build_keyboard_report_with_mods_and_keys);
	RUN_TEST(test_build_consumer_report_release);
	RUN_TEST(test_build_consumer_report_with_usage);

	/* HVX pipeline */
	RUN_TEST(test_hvx_init_full_credits);
	RUN_TEST(test_hvx_mark_dirty);
	RUN_TEST(test_hvx_send_consumes_credit);
	RUN_TEST(test_hvx_credit_zero_blocks_send);
	RUN_TEST(test_hvx_tx_complete_restores_credit);
	RUN_TEST(test_hvx_priority_consumer_over_keyboard);
	RUN_TEST(test_hvx_priority_keyboard_over_mouse);
	RUN_TEST(test_hvx_full_pipeline_cycle);

	/* Guaranteed release */
	RUN_TEST(test_guaranteed_release_under_backpressure);
	RUN_TEST(test_guaranteed_release_keyboard_under_backpressure);

	/* State init + misc */
	RUN_TEST(test_state_init);
	RUN_TEST(test_control_point_values);
	RUN_TEST(test_source_bit_values);

	return test_framework_finish();
}
