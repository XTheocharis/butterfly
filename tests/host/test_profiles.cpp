/*
 * test_profiles.cpp - host tests for HID profile evaluation logic.
 *
 * Tests profiles_eval.c (pure-C, no SDK deps) by including it directly.
 * Covers: built-in profile lookup, all 4 built-in mappings, desktop
 * air-mouse contextual A, custom profile validation, known-good
 * preservation, usage range checks, profile equality.
 */
#include "test_framework.h"
#include "../../src/ble/profiles_eval.h"
#include "../../src/ble/hids_eval.h"

#include <string.h>

/* ---- Built-in profile access ---- */

static void test_get_builtin_android_tv(void) {
	const prof_profile_t *p = profiles_get_builtin(PROF_ANDROID_TV);
	TEST_ASSERT(p != NULL, "android_tv should exist");
	TEST_ASSERT_EQ_INT(PROF_ANDROID_TV, p->id);
	TEST_ASSERT(strcmp(p->name, "android_tv") == 0, "name");
	TEST_ASSERT_EQ_INT(6, p->mappings_count);
}

static void test_get_builtin_fire_tv(void) {
	const prof_profile_t *p = profiles_get_builtin(PROF_FIRE_TV);
	TEST_ASSERT(p != NULL, "fire_tv should exist");
	TEST_ASSERT_EQ_INT(PROF_FIRE_TV, p->id);
	TEST_ASSERT(strcmp(p->name, "fire_tv") == 0, "name");
}

static void test_get_builtin_portal_tv(void) {
	const prof_profile_t *p = profiles_get_builtin(PROF_PORTAL_TV);
	TEST_ASSERT(p != NULL, "portal_tv should exist");
	TEST_ASSERT_EQ_INT(PROF_PORTAL_TV, p->id);
	TEST_ASSERT(strcmp(p->name, "portal_tv") == 0, "name");
}

static void test_get_builtin_desktop_airmouse(void) {
	const prof_profile_t *p = profiles_get_builtin(PROF_DESKTOP_AIRMOUSE);
	TEST_ASSERT(p != NULL, "desktop_airmouse should exist");
	TEST_ASSERT_EQ_INT(PROF_DESKTOP_AIRMOUSE, p->id);
	TEST_ASSERT(strcmp(p->name, "desktop_airmouse") == 0, "name");
	TEST_ASSERT_EQ_INT(2, p->mappings_count);
	TEST_ASSERT(p->pointer_mode, "pointer_mode");
	TEST_ASSERT(p->tilt_mode, "tilt_mode");
}

static void test_get_builtin_custom_returns_null(void) {
	const prof_profile_t *p = profiles_get_builtin(PROF_CUSTOM);
	TEST_ASSERT(p == NULL, "custom is not a builtin");
}

static void test_get_builtin_out_of_range_returns_null(void) {
	const prof_profile_t *p = profiles_get_builtin((profile_id_t)99);
	TEST_ASSERT(p == NULL, "out-of-range returns null");
}

static void test_is_builtin(void) {
	TEST_ASSERT(profiles_is_builtin(PROF_ANDROID_TV), "android_tv is builtin");
	TEST_ASSERT(profiles_is_builtin(PROF_DESKTOP_AIRMOUSE), "desktop is builtin");
	TEST_ASSERT(!profiles_is_builtin(PROF_CUSTOM), "custom is NOT builtin");
	TEST_ASSERT(!profiles_is_builtin((profile_id_t)99), "oob is NOT builtin");
}

/* ---- Mapping lookup for TV profiles ---- */

static void test_tv_profile_a_maps_to_menu_pick(void) {
	const prof_profile_t *p = profiles_get_builtin(PROF_ANDROID_TV);
	prof_hid_target_t t;
	prof_result_t r = profiles_lookup(p, PROF_SRC_BUTTON_A, &t);
	TEST_ASSERT_EQ_INT(PROF_OK, r);
	TEST_ASSERT_EQ_INT(HIDS_REPORT_ID_CONSUMER, t.report_id);
	TEST_ASSERT_EQ_INT(HIDS_CONSUMER_MENU_PICK, t.usage);
}

static void test_tv_profile_b_maps_to_menu_escape(void) {
	const prof_profile_t *p = profiles_get_builtin(PROF_FIRE_TV);
	prof_hid_target_t t;
	prof_result_t r = profiles_lookup(p, PROF_SRC_BUTTON_B, &t);
	TEST_ASSERT_EQ_INT(PROF_OK, r);
	TEST_ASSERT_EQ_INT(HIDS_CONSUMER_MENU_ESCAPE, t.usage);
}

static void test_tv_profile_swipe_up_maps_to_menu_up(void) {
	const prof_profile_t *p = profiles_get_builtin(PROF_PORTAL_TV);
	prof_hid_target_t t;
	prof_result_t r = profiles_lookup(p, PROF_SRC_APDS_UP, &t);
	TEST_ASSERT_EQ_INT(PROF_OK, r);
	TEST_ASSERT_EQ_INT(HIDS_CONSUMER_MENU_UP, t.usage);
}

static void test_tv_profile_swipe_down_maps_to_menu_down(void) {
	const prof_profile_t *p = profiles_get_builtin(PROF_ANDROID_TV);
	prof_hid_target_t t;
	prof_result_t r = profiles_lookup(p, PROF_SRC_APDS_DOWN, &t);
	TEST_ASSERT_EQ_INT(PROF_OK, r);
	TEST_ASSERT_EQ_INT(HIDS_CONSUMER_MENU_DOWN, t.usage);
}

static void test_tv_profile_swipe_left_right(void) {
	const prof_profile_t *p = profiles_get_builtin(PROF_ANDROID_TV);
	prof_hid_target_t t;
	TEST_ASSERT_EQ_INT(PROF_OK, profiles_lookup(p, PROF_SRC_APDS_LEFT, &t));
	TEST_ASSERT_EQ_INT(HIDS_CONSUMER_MENU_LEFT, t.usage);
	TEST_ASSERT_EQ_INT(PROF_OK, profiles_lookup(p, PROF_SRC_APDS_RIGHT, &t));
	TEST_ASSERT_EQ_INT(HIDS_CONSUMER_MENU_RIGHT, t.usage);
}

static void test_tv_profiles_share_same_mappings(void) {
	const prof_profile_t *a = profiles_get_builtin(PROF_ANDROID_TV);
	const prof_profile_t *f = profiles_get_builtin(PROF_FIRE_TV);
	const prof_profile_t *p = profiles_get_builtin(PROF_PORTAL_TV);
	TEST_ASSERT_EQ_INT(a->mappings_count, f->mappings_count);
	TEST_ASSERT_EQ_INT(a->mappings_count, p->mappings_count);
	for (uint8_t i = 0; i < a->mappings_count; i++) {
		TEST_ASSERT_EQ_INT(a->mappings[i].usage, f->mappings[i].usage);
		TEST_ASSERT_EQ_INT(a->mappings[i].usage, p->mappings[i].usage);
	}
}

/* ---- Desktop air-mouse contextual A ---- */

static void test_desktop_a_no_motion_maps_to_enter(void) {
	prof_hid_target_t t = profiles_desktop_a_target(false);
	TEST_ASSERT_EQ_INT(HIDS_REPORT_ID_KEYBOARD, t.report_id);
	TEST_ASSERT_EQ_INT(PROF_KB_ENTER, t.usage);
}

static void test_desktop_a_with_motion_maps_to_mouse_btn1(void) {
	prof_hid_target_t t = profiles_desktop_a_target(true);
	TEST_ASSERT_EQ_INT(HIDS_REPORT_ID_MOUSE, t.report_id);
	TEST_ASSERT_EQ_INT(1u, t.usage);
}

static void test_desktop_b_maps_to_escape(void) {
	const prof_profile_t *p = profiles_get_builtin(PROF_DESKTOP_AIRMOUSE);
	prof_hid_target_t t;
	prof_result_t r = profiles_lookup(p, PROF_SRC_BUTTON_B, &t);
	TEST_ASSERT_EQ_INT(PROF_OK, r);
	TEST_ASSERT_EQ_INT(HIDS_REPORT_ID_KEYBOARD, t.report_id);
	TEST_ASSERT_EQ_INT(0x29u, t.usage);
}

static void test_desktop_no_swipe_mappings(void) {
	const prof_profile_t *p = profiles_get_builtin(PROF_DESKTOP_AIRMOUSE);
	prof_hid_target_t t;
	TEST_ASSERT_EQ_INT(PROF_ERR_NO_MAPPING,
	                   profiles_lookup(p, PROF_SRC_APDS_UP, &t));
	TEST_ASSERT_EQ_INT(PROF_ERR_NO_MAPPING,
	                   profiles_lookup(p, PROF_SRC_APDS_DOWN, &t));
}

/* ---- Lookup edge cases ---- */

static void test_lookup_invalid_source(void) {
	const prof_profile_t *p = profiles_get_builtin(PROF_ANDROID_TV);
	prof_hid_target_t t;
	prof_result_t r = profiles_lookup(p, (prof_src_t)PROF_SRC_COUNT, &t);
	TEST_ASSERT_EQ_INT(PROF_ERR_INVALID_SOURCE, r);
}

static void test_lookup_null_profile(void) {
	prof_hid_target_t t;
	prof_result_t r = profiles_lookup(NULL, PROF_SRC_BUTTON_A, &t);
	TEST_ASSERT(r != PROF_OK, "null profile should fail");
}

/* ---- Usage validation ---- */

static void test_consumer_usage_valid(void) {
	TEST_ASSERT(profiles_consumer_usage_valid(0x0041), "menu pick");
	TEST_ASSERT(profiles_consumer_usage_valid(0x0223), "AC home");
	TEST_ASSERT(profiles_consumer_usage_valid(0x02FF), "max");
	TEST_ASSERT(!profiles_consumer_usage_valid(0x0300), "over max");
}

static void test_keyboard_usage_valid(void) {
	TEST_ASSERT(!profiles_keyboard_usage_valid(0x03), "below min");
	TEST_ASSERT(profiles_keyboard_usage_valid(0x04), "min A");
	TEST_ASSERT(profiles_keyboard_usage_valid(0x28), "enter");
	TEST_ASSERT(profiles_keyboard_usage_valid(0xE7), "max RGUI");
	TEST_ASSERT(!profiles_keyboard_usage_valid(0xE8), "over max");
}

static void test_mouse_button_valid(void) {
	TEST_ASSERT(!profiles_mouse_button_valid(0), "zero invalid");
	TEST_ASSERT(profiles_mouse_button_valid(1), "btn 1");
	TEST_ASSERT(profiles_mouse_button_valid(8), "btn 8");
	TEST_ASSERT(!profiles_mouse_button_valid(9), "btn 9 invalid");
}

/* ---- Custom profile validation ---- */

static void test_validate_valid_custom(void) {
	prof_profile_t c = {};
	c.id = PROF_CUSTOM;
	c.mappings_count = 1;
	c.mappings[0].report_id = HIDS_REPORT_ID_CONSUMER;
	c.mappings[0].usage_page = PROF_USAGE_PAGE_CONSUMER;
	c.mappings[0].usage = HIDS_CONSUMER_AC_HOME;
	strncpy(c.name, "test", PROF_NAME_MAX - 1);
	TEST_ASSERT_EQ_INT(PROF_OK, profiles_validate_custom(&c));
}

static void test_validate_too_many_mappings(void) {
	prof_profile_t c = {};
	c.mappings_count = PROF_MAPPINGS_MAX + 1;
	TEST_ASSERT_EQ_INT(PROF_ERR_PROFILE_FULL,
	                   profiles_validate_custom(&c));
}

static void test_validate_invalid_report_id(void) {
	prof_profile_t c = {};
	c.mappings_count = 1;
	c.mappings[0].report_id = 99;
	c.mappings[0].usage = 0x41;
	TEST_ASSERT_EQ_INT(PROF_ERR_INVALID_USAGE,
	                   profiles_validate_custom(&c));
}

static void test_validate_invalid_consumer_usage(void) {
	prof_profile_t c = {};
	c.mappings_count = 1;
	c.mappings[0].report_id = HIDS_REPORT_ID_CONSUMER;
	c.mappings[0].usage_page = PROF_USAGE_PAGE_CONSUMER;
	c.mappings[0].usage = 0xFFFF; /* over logical max */
	TEST_ASSERT_EQ_INT(PROF_ERR_INVALID_USAGE,
	                   profiles_validate_custom(&c));
}

static void test_validate_invalid_keyboard_usage(void) {
	prof_profile_t c = {};
	c.mappings_count = 1;
	c.mappings[0].report_id = HIDS_REPORT_ID_KEYBOARD;
	c.mappings[0].usage_page = PROF_USAGE_PAGE_KEYBOARD;
	c.mappings[0].usage = 0x03; /* below keycode min */
	TEST_ASSERT_EQ_INT(PROF_ERR_INVALID_USAGE,
	                   profiles_validate_custom(&c));
}

static void test_validate_null_candidate(void) {
	TEST_ASSERT_EQ_INT(PROF_ERR_INVALID_ID,
	                   profiles_validate_custom(NULL));
}

/* ---- Profile equality ---- */

static void test_profiles_equal_same(void) {
	const prof_profile_t *a = profiles_get_builtin(PROF_ANDROID_TV);
	TEST_ASSERT(profiles_equal(a, a), "same pointer equal");
}

static void test_profiles_equal_different(void) {
	const prof_profile_t *a = profiles_get_builtin(PROF_ANDROID_TV);
	const prof_profile_t *b = profiles_get_builtin(PROF_DESKTOP_AIRMOUSE);
	TEST_ASSERT(!profiles_equal(a, b), "different profiles");
}

static void test_profiles_equal_null(void) {
	TEST_ASSERT(profiles_equal(NULL, NULL), "both null");
	TEST_ASSERT(!profiles_equal(NULL, profiles_get_builtin(PROF_ANDROID_TV)),
	            "null vs non-null");
}

/* ---- AC Home / Back compatibility usages ---- */

static void test_ac_home_in_range(void) {
	TEST_ASSERT(profiles_consumer_usage_valid(HIDS_CONSUMER_AC_HOME),
	            "AC Home 0x0223 in range");
	TEST_ASSERT(profiles_consumer_usage_valid(HIDS_CONSUMER_AC_BACK),
	            "AC Back 0x0224 in range");
}

/* ---- Names ---- */

static void test_profile_names(void) {
	TEST_ASSERT(strcmp(profiles_name(PROF_ANDROID_TV), "android_tv") == 0, "android_tv name");
	TEST_ASSERT(strcmp(profiles_name(PROF_FIRE_TV), "fire_tv") == 0, "fire_tv name");
	TEST_ASSERT(strcmp(profiles_name(PROF_PORTAL_TV), "portal_tv") == 0, "portal_tv name");
	TEST_ASSERT(strcmp(profiles_name(PROF_DESKTOP_AIRMOUSE),
	                    "desktop_airmouse") == 0, "desktop name");
	TEST_ASSERT(strcmp(profiles_name(PROF_CUSTOM), "custom") == 0, "custom name");
}

/* ---- Test runner ---- */

int main(void) {
	test_framework_init();

	RUN_TEST(test_get_builtin_android_tv);
	RUN_TEST(test_get_builtin_fire_tv);
	RUN_TEST(test_get_builtin_portal_tv);
	RUN_TEST(test_get_builtin_desktop_airmouse);
	RUN_TEST(test_get_builtin_custom_returns_null);
	RUN_TEST(test_get_builtin_out_of_range_returns_null);
	RUN_TEST(test_is_builtin);

	RUN_TEST(test_tv_profile_a_maps_to_menu_pick);
	RUN_TEST(test_tv_profile_b_maps_to_menu_escape);
	RUN_TEST(test_tv_profile_swipe_up_maps_to_menu_up);
	RUN_TEST(test_tv_profile_swipe_down_maps_to_menu_down);
	RUN_TEST(test_tv_profile_swipe_left_right);
	RUN_TEST(test_tv_profiles_share_same_mappings);

	RUN_TEST(test_desktop_a_no_motion_maps_to_enter);
	RUN_TEST(test_desktop_a_with_motion_maps_to_mouse_btn1);
	RUN_TEST(test_desktop_b_maps_to_escape);
	RUN_TEST(test_desktop_no_swipe_mappings);

	RUN_TEST(test_lookup_invalid_source);
	RUN_TEST(test_lookup_null_profile);

	RUN_TEST(test_consumer_usage_valid);
	RUN_TEST(test_keyboard_usage_valid);
	RUN_TEST(test_mouse_button_valid);

	RUN_TEST(test_validate_valid_custom);
	RUN_TEST(test_validate_too_many_mappings);
	RUN_TEST(test_validate_invalid_report_id);
	RUN_TEST(test_validate_invalid_consumer_usage);
	RUN_TEST(test_validate_invalid_keyboard_usage);
	RUN_TEST(test_validate_null_candidate);

	RUN_TEST(test_profiles_equal_same);
	RUN_TEST(test_profiles_equal_different);
	RUN_TEST(test_profiles_equal_null);

	RUN_TEST(test_ac_home_in_range);
	RUN_TEST(test_profile_names);

	return test_framework_finish();
}
