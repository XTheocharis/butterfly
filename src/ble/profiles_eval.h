/*
 * profiles_eval.h - Pure-C HID profile evaluation (host-testable, no SDK deps).
 *
 * Defines the four built-in remote-control profiles (android_tv, fire_tv,
 * portal_tv, desktop_airmouse) plus a custom profile slot. Each profile maps
 * physical inputs (Button A, Button B, APDS swipes) to HID usages consumed
 * by hids_eval's refcount state machine.
 *
 * The firmware C++ class (profiles.h/cpp) wraps these evaluations with
 * runtime HIDS state mutations.
 *
 * Compiled on BOTH host (for unit tests) and device (linked into firmware).
 *
 * AC Home (0x0223) / AC Back (0x0224) are compatibility-sensitive: the
 * profiles store BOTH the preferred consumer usage AND a keyboard fallback
 * usage. Callers that detect host incompatibility may switch to the
 * fallback. See profiles_desktop_a_target() for the contextual pattern.
 */
#ifndef PROFILES_EVAL_H
#define PROFILES_EVAL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Profile IDs ----------------------------------------------------- */

typedef enum {
	PROF_ANDROID_TV       = 0,
	PROF_FIRE_TV          = 1,
	PROF_PORTAL_TV        = 2,
	PROF_DESKTOP_AIRMOUSE = 3,
	PROF_CUSTOM           = 4,
	PROF_COUNT,
	PROF_INVALID          = 0xFF,
} profile_id_t;

/* ---- Input sources (mirror board_InputSource but smaller for firmware) -- */

typedef enum {
	PROF_SRC_BUTTON_A = 0,
	PROF_SRC_BUTTON_B = 1,
	PROF_SRC_APDS_UP    = 2,
	PROF_SRC_APDS_DOWN  = 3,
	PROF_SRC_APDS_LEFT  = 4,
	PROF_SRC_APDS_RIGHT = 5,
	PROF_SRC_COUNT,
} prof_src_t;

/* ---- HID target descriptor ------------------------------------------- */
/*
 * Describes where a press goes in the HIDS report map.
 * report_id: 1=mouse, 2=keyboard, 3=consumer (matches HIDS_REPORT_ID_*).
 * usage_page: HID usage page (1=Generic Desktop, 7=Keyboard, 12=Consumer).
 * usage: 16-bit usage value within the page.
 *
 * For mouse reports, usage=button index (1-8), movement is separate.
 * For keyboard, usage=keycode (0x04-0xE7).
 * For consumer, usage=16-bit consumer page usage.
 *
 * A press with usage=0 means "release-only / no-op" (used for air-mouse
 * fallback when motion is in 3s window).
 */
typedef struct {
	uint8_t  report_id;   /* HIDS_REPORT_ID_MOUSE/KEYBOARD/CONSUMER */
	uint8_t  usage_page;  /* HID usage page */
	uint16_t usage;       /* usage value within page */
} prof_hid_target_t;

/* ---- Profile descriptor ---------------------------------------------- */

#define PROF_NAME_MAX 32
#define PROF_MAPPINGS_MAX 16

typedef struct {
	profile_id_t id;
	char name[PROF_NAME_MAX];
	uint32_t sensitivity;  /* air-mouse px/degree scaling hint */
	uint32_t deadzone;     /* air-mouse dead-zone hint (micro-degrees/sec) */
	bool pointer_mode;     /* true = APDS/air-mouse moves cursor */
	bool tilt_mode;        /* true = tilt gestures navigate */
	uint8_t mappings_count;
	prof_hid_target_t mappings[PROF_MAPPINGS_MAX];
} prof_profile_t;

/* ---- Result codes ---------------------------------------------------- */

typedef enum {
	PROF_OK                 = 0,
	PROF_ERR_INVALID_ID     = 1,
	PROF_ERR_INVALID_SOURCE = 2,
	PROF_ERR_NO_MAPPING     = 3,
	PROF_ERR_INVALID_USAGE  = 4,
	PROF_ERR_PROFILE_FULL   = 5,
	PROF_ERR_IMMUTABLE      = 6,
} prof_result_t;

/* ---- Built-in profile access ----------------------------------------- */

/* Returns the built-in profile for the given ID.
 * Returns NULL for PROF_CUSTOM or out-of-range IDs. */
const prof_profile_t *profiles_get_builtin(profile_id_t id);

/* Returns the human-readable name for a profile ID. */
const char *profiles_name(profile_id_t id);

/* Returns true if the profile ID is a built-in (not custom). */
bool profiles_is_builtin(profile_id_t id);

/* ---- Mapping lookup -------------------------------------------------- */

/*
 * Look up the HID target for a given source in a profile.
 * Returns PROF_OK and fills *out_target on success.
 * Returns PROF_ERR_NO_MAPPING if the source has no mapping.
 * Returns PROF_ERR_INVALID_SOURCE if the source is out of range.
 */
prof_result_t profiles_lookup(const prof_profile_t *prof,
                              prof_src_t src,
                              prof_hid_target_t *out_target);

/* ---- Desktop air-mouse contextual mapping ---------------------------- */
/*
 * desktop_airmouse Button A is contextual: if motion happened in the last
 * 3 seconds, A = mouse button 1; otherwise A = keyboard Enter (0x28).
 * profiles_desktop_a_target() implements this contextual selection.
 *
 * motion_active: true if air-mouse motion occurred within the dwell window.
 */
prof_hid_target_t profiles_desktop_a_target(bool motion_active);

/* ---- Custom profile validation --------------------------------------- */
/*
 * Validate a candidate custom profile before applying it.
 * Checks:
 *   - mappings_count <= PROF_MAPPINGS_MAX
 *   - each mapping's report_id is 1 (mouse), 2 (keyboard), or 3 (consumer)
 *   - each mapping's usage is within the declared report's logical range
 *   - name is null-terminated within PROF_NAME_MAX
 *
 * The current known-good profile is preserved if validation fails —
 * callers must call this BEFORE replacing the stored custom profile.
 */
prof_result_t profiles_validate_custom(const prof_profile_t *candidate);

/* ---- Profile comparison (for "preserve known-good" semantics) -------- */

/* Returns true if two profiles are byte-identical. */
bool profiles_equal(const prof_profile_t *a, const prof_profile_t *b);

/* ---- Usage range validation helpers ---------------------------------- */

/* Returns true if a consumer usage is in our report map's logical range. */
bool profiles_consumer_usage_valid(uint16_t usage);

/* Returns true if a keyboard keycode is in the report map's range. */
bool profiles_keyboard_usage_valid(uint16_t usage);

/* Returns true if a mouse button index (1-8) is valid. */
bool profiles_mouse_button_valid(uint16_t button);

/* ---- HID usage page constants (mirror USB HID spec) ------------------ */

#define PROF_USAGE_PAGE_GENERIC_DESKTOP 1u
#define PROF_USAGE_PAGE_KEYBOARD        7u
#define PROF_USAGE_PAGE_CONSUMER       12u

/* Keyboard Enter (used by desktop_airmouse fallback). */
#define PROF_KB_ENTER  0x28u

#ifdef __cplusplus
}
#endif

#endif /* PROFILES_EVAL_H */
