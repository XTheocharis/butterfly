/*
 * profiles_eval.c - Pure-logic HID profile evaluation.
 *
 * No SDK deps. Included from profiles.cpp (firmware) and test_profiles.cpp
 * (host test) for unit testing.
 */
#include "profiles_eval.h"
#include "hids_eval.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Built-in profile definitions ------------------------------------ */
/*
 * android_tv / fire_tv / portal_tv share the same consumer usages
 * (Amazon Fire TV uses the same Android TV consumer codes):
 *   A       = Menu Pick    0x0041
 *   B       = Menu Escape  0x0046
 *   swipe U = Menu Up      0x0042
 *   swipe D = Menu Down    0x0043
 *   swipe L = Menu Left    0x0044
 *   swipe R = Menu Right   0x0045
 */
#define TV_MAPPINGS_INIT { \
	{ HIDS_REPORT_ID_CONSUMER, PROF_USAGE_PAGE_CONSUMER, HIDS_CONSUMER_MENU_PICK   }, \
	{ HIDS_REPORT_ID_CONSUMER, PROF_USAGE_PAGE_CONSUMER, HIDS_CONSUMER_MENU_ESCAPE }, \
	{ HIDS_REPORT_ID_CONSUMER, PROF_USAGE_PAGE_CONSUMER, HIDS_CONSUMER_MENU_UP     }, \
	{ HIDS_REPORT_ID_CONSUMER, PROF_USAGE_PAGE_CONSUMER, HIDS_CONSUMER_MENU_DOWN   }, \
	{ HIDS_REPORT_ID_CONSUMER, PROF_USAGE_PAGE_CONSUMER, HIDS_CONSUMER_MENU_LEFT   }, \
	{ HIDS_REPORT_ID_CONSUMER, PROF_USAGE_PAGE_CONSUMER, HIDS_CONSUMER_MENU_RIGHT  }, \
}

/* Helper: build a TV profile (android_tv, fire_tv, portal_tv share mappings). */
static prof_profile_t make_tv_profile(profile_id_t id, const char *name)
{
	prof_profile_t p;
	memset(&p, 0, sizeof(p));
	p.id = id;
	strncpy(p.name, name, PROF_NAME_MAX - 1);
	p.sensitivity = 10;
	p.deadzone = 800;
	p.pointer_mode = false;
	p.tilt_mode = false;
	p.mappings_count = 6;
	const prof_hid_target_t tv[6] = TV_MAPPINGS_INIT;
	memcpy(p.mappings, tv, sizeof(tv));
	return p;
}

/* Helper: build the desktop_airmouse profile. */
static prof_profile_t make_desktop_airmouse(void)
{
	prof_profile_t p;
	memset(&p, 0, sizeof(p));
	p.id = PROF_DESKTOP_AIRMOUSE;
	strncpy(p.name, "desktop_airmouse", PROF_NAME_MAX - 1);
	p.sensitivity = 11;
	p.deadzone = 800;
	p.pointer_mode = true;
	p.tilt_mode = true;
	p.mappings_count = 2;
	/*
	 * A = keyboard Enter (default; firmware overrides to mouse btn 1
	 * via profiles_desktop_a_target() when motion is active).
	 * B = keyboard Escape (0x29).
	 */
	p.mappings[0].report_id  = HIDS_REPORT_ID_KEYBOARD;
	p.mappings[0].usage_page = PROF_USAGE_PAGE_KEYBOARD;
	p.mappings[0].usage      = PROF_KB_ENTER;
	p.mappings[1].report_id  = HIDS_REPORT_ID_KEYBOARD;
	p.mappings[1].usage_page = PROF_USAGE_PAGE_KEYBOARD;
	p.mappings[1].usage      = 0x29u; /* Escape */
	return p;
}

/* Lazy-initialized cache (built once on first access). */
static prof_profile_t s_builtins[PROF_COUNT];
static bool s_builtins_init = false;

static void ensure_builtins(void)
{
	if (s_builtins_init) {
		return;
	}
	s_builtins[PROF_ANDROID_TV]       = make_tv_profile(PROF_ANDROID_TV, "android_tv");
	s_builtins[PROF_FIRE_TV]          = make_tv_profile(PROF_FIRE_TV, "fire_tv");
	s_builtins[PROF_PORTAL_TV]        = make_tv_profile(PROF_PORTAL_TV, "portal_tv");
	s_builtins[PROF_DESKTOP_AIRMOUSE] = make_desktop_airmouse();
	s_builtins_init = true;
}

/* ---- Built-in profile access ----------------------------------------- */

const prof_profile_t *profiles_get_builtin(profile_id_t id)
{
	ensure_builtins();
	if (id >= PROF_COUNT || id == PROF_CUSTOM) {
		return NULL;
	}
	return &s_builtins[id];
}

const char *profiles_name(profile_id_t id)
{
	ensure_builtins();
	if (id < PROF_CUSTOM) {
		return s_builtins[id].name;
	}
	return "custom";
}

bool profiles_is_builtin(profile_id_t id)
{
	return id < PROF_CUSTOM;
}

/* ---- Mapping lookup -------------------------------------------------- */

prof_result_t profiles_lookup(const prof_profile_t *prof,
                              prof_src_t src,
                              prof_hid_target_t *out_target)
{
	if (prof == NULL || out_target == NULL) {
		return PROF_ERR_INVALID_ID;
	}
	if (src >= PROF_SRC_COUNT) {
		return PROF_ERR_INVALID_SOURCE;
	}
	for (uint8_t i = 0; i < prof->mappings_count; i++) {
		/*
		 * Mappings are stored in source order:
		 * index 0 = BUTTON_A, 1 = BUTTON_B, 2 = APDS_UP, etc.
		 * If mappings_count < (src+1), there is no mapping for this source.
		 */
		if (i == (uint8_t)src) {
			*out_target = prof->mappings[i];
			return PROF_OK;
		}
	}
	return PROF_ERR_NO_MAPPING;
}

/* ---- Desktop air-mouse contextual mapping ---------------------------- */

prof_hid_target_t profiles_desktop_a_target(bool motion_active)
{
	if (motion_active) {
		prof_hid_target_t t = {
			HIDS_REPORT_ID_MOUSE,
			PROF_USAGE_PAGE_GENERIC_DESKTOP,
			1u,  /* mouse button 1 */
		};
		return t;
	}
	prof_hid_target_t t = {
		HIDS_REPORT_ID_KEYBOARD,
		PROF_USAGE_PAGE_KEYBOARD,
		PROF_KB_ENTER,
	};
	return t;
}

/* ---- Usage range validation helpers ---------------------------------- */

bool profiles_consumer_usage_valid(uint16_t usage)
{
	/* Our consumer report map has logical max 0x02FF. */
	return usage <= HIDS_CONSUMER_LOGICAL_MAX;
}

bool profiles_keyboard_usage_valid(uint16_t usage)
{
	/* HID keycodes: 0x04 (A) through 0xE7 (Right GUI). */
	return usage >= 0x04u && usage <= 0xE7u;
}

bool profiles_mouse_button_valid(uint16_t button)
{
	return button >= 1u && button <= 8u;
}

/* ---- Custom profile validation --------------------------------------- */

prof_result_t profiles_validate_custom(const prof_profile_t *candidate)
{
	if (candidate == NULL) {
		return PROF_ERR_INVALID_ID;
	}
	if (candidate->mappings_count > PROF_MAPPINGS_MAX) {
		return PROF_ERR_PROFILE_FULL;
	}
	/* Name must be null-terminated within PROF_NAME_MAX. The struct is
	 * zero-initialized by callers, so we just check the last byte. */
	if (candidate->name[PROF_NAME_MAX - 1] != '\0') {
		return PROF_ERR_INVALID_USAGE;
	}
	for (uint8_t i = 0; i < candidate->mappings_count; i++) {
		const prof_hid_target_t *m = &candidate->mappings[i];
		switch (m->report_id) {
		case HIDS_REPORT_ID_MOUSE:
			if (!profiles_mouse_button_valid(m->usage)) {
				return PROF_ERR_INVALID_USAGE;
			}
			break;
		case HIDS_REPORT_ID_KEYBOARD:
			if (!profiles_keyboard_usage_valid(m->usage)) {
				return PROF_ERR_INVALID_USAGE;
			}
			break;
		case HIDS_REPORT_ID_CONSUMER:
			if (!profiles_consumer_usage_valid(m->usage)) {
				return PROF_ERR_INVALID_USAGE;
			}
			break;
		default:
			return PROF_ERR_INVALID_USAGE;
		}
	}
	return PROF_OK;
}

/* ---- Profile comparison ---------------------------------------------- */

bool profiles_equal(const prof_profile_t *a, const prof_profile_t *b)
{
	if (a == NULL || b == NULL) {
		return a == b;
	}
	if (a->id != b->id ||
	    a->sensitivity != b->sensitivity ||
	    a->deadzone != b->deadzone ||
	    a->pointer_mode != b->pointer_mode ||
	    a->tilt_mode != b->tilt_mode ||
	    a->mappings_count != b->mappings_count) {
		return false;
	}
	for (uint8_t i = 0; i < a->mappings_count; i++) {
		if (a->mappings[i].report_id  != b->mappings[i].report_id ||
		    a->mappings[i].usage_page != b->mappings[i].usage_page ||
		    a->mappings[i].usage      != b->mappings[i].usage) {
			return false;
		}
	}
	for (uint8_t i = 0; i < PROF_NAME_MAX; i++) {
		if (a->name[i] != b->name[i]) {
			return false;
		}
	}
	return true;
}

#ifdef __cplusplus
}
#endif
