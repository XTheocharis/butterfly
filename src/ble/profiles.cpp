/*
 * profiles.cpp - Firmware HID profile manager implementation.
 *
 * Wraps profiles_eval with HIDS refcount state mutations.
 */
#include "profiles.h"

#ifdef BOARD_CLUE

/* Pull in the pure-logic eval functions. */
#include "profiles_eval.h"

#include <string.h>
#include "timebase.h"

/* board.pb.h for RemoteProfile conversion. */
#include "domains/board.h"
#include "board.pb.h"

/* ---- HIDS source mapping --------------------------------------------- */
/*
 * Map a prof_src_t to the HIDS refcount source identifier.
 * Button A/B use dedicated source slots; APDS swipes share the
 * APDS_SWIPE source since only one swipe is active at a time.
 */
static hids_src_t prof_src_to_hids(prof_src_t src)
{
	switch (src) {
	case PROF_SRC_BUTTON_A:   return HIDS_SRC_BUTTON_A;
	case PROF_SRC_BUTTON_B:   return HIDS_SRC_BUTTON_B;
	case PROF_SRC_APDS_UP:
	case PROF_SRC_APDS_DOWN:
	case PROF_SRC_APDS_LEFT:
	case PROF_SRC_APDS_RIGHT: return HIDS_SRC_APDS_SWIPE;
	default:                  return HIDS_SRC_BUTTON_A;
	}
}

/* ---- ProfileManager -------------------------------------------------- */

ProfileManager::ProfileManager()
	: m_activeId(PROF_ANDROID_TV)
	, m_hids(NULL)
	, m_lastMotionUs(0)
{
	memset(&m_custom, 0, sizeof(m_custom));
	m_custom.id = PROF_CUSTOM;
	strncpy(m_custom.name, "custom", PROF_NAME_MAX - 1);
}

void ProfileManager::setHidsState(hids_state_t *state)
{
	m_hids = state;
}

bool ProfileManager::selectProfile(profile_id_t id)
{
	if (id >= PROF_COUNT) {
		return false;
	}
	if (id == PROF_CUSTOM) {
		/* Only allow selecting custom if it has been configured. */
		if (m_custom.mappings_count == 0) {
			return false;
		}
	}
	m_activeId = id;
	return true;
}

const prof_profile_t *ProfileManager::getActiveProfileDesc(void) const
{
	if (m_activeId == PROF_CUSTOM) {
		return &m_custom;
	}
	return profiles_get_builtin(m_activeId);
}

const prof_profile_t *ProfileManager::getCustomProfile(void) const
{
	return &m_custom;
}

prof_result_t ProfileManager::applyTarget(bool pressed,
                                          const prof_hid_target_t *target,
                                          hids_src_t hidsSrc)
{
	if (m_hids == NULL || target == NULL) {
		return PROF_ERR_INVALID_ID;
	}
	switch (target->report_id) {
	case HIDS_REPORT_ID_MOUSE: {
		bool ok;
		if (pressed) {
			ok = hids_mouse_button_press(&m_hids->mouse_buttons,
			                             (uint8_t)target->usage, hidsSrc);
		} else {
			ok = hids_mouse_button_release(&m_hids->mouse_buttons,
			                               (uint8_t)target->usage, hidsSrc);
		}
		if (ok) {
			hids_hvx_mark_dirty(&m_hids->hvx, HIDS_REPORT_ID_MOUSE);
		}
		return PROF_OK;
	}
	case HIDS_REPORT_ID_KEYBOARD: {
		bool ok;
		if (pressed) {
			ok = hids_keyboard_press(&m_hids->keyboard,
			                         (uint8_t)target->usage, hidsSrc);
		} else {
			ok = hids_keyboard_release(&m_hids->keyboard,
			                           (uint8_t)target->usage, hidsSrc);
		}
		if (ok) {
			hids_hvx_mark_dirty(&m_hids->hvx, HIDS_REPORT_ID_KEYBOARD);
		}
		return PROF_OK;
	}
	case HIDS_REPORT_ID_CONSUMER: {
		bool ok;
		if (pressed) {
			ok = hids_consumer_press(&m_hids->consumer,
			                         target->usage, hidsSrc);
		} else {
			ok = hids_consumer_release(&m_hids->consumer,
			                           target->usage, hidsSrc);
		}
		if (ok) {
			hids_hvx_mark_dirty(&m_hids->hvx, HIDS_REPORT_ID_CONSUMER);
		}
		return PROF_OK;
	}
	default:
		return PROF_ERR_INVALID_USAGE;
	}
}

prof_result_t ProfileManager::dispatchInput(bool pressed, prof_src_t src,
                                            bool motion_active)
{
	const prof_profile_t *prof = getActiveProfileDesc();
	if (prof == NULL) {
		return PROF_ERR_INVALID_ID;
	}

	prof_hid_target_t target;
	prof_result_t result;

	/*
	 * desktop_airmouse Button A is contextual: if motion happened
	 * within the dwell window, A maps to mouse button 1; otherwise
	 * A maps to keyboard Enter.
	 */
	if (m_activeId == PROF_DESKTOP_AIRMOUSE &&
	    src == PROF_SRC_BUTTON_A) {
		target = profiles_desktop_a_target(motion_active);
	} else {
		result = profiles_lookup(prof, src, &target);
		if (result != PROF_OK) {
			return result;
		}
	}

	hids_src_t hidsSrc = prof_src_to_hids(src);
	return applyTarget(pressed, &target, hidsSrc);
}

prof_result_t ProfileManager::applyCustomProfile(
	const prof_profile_t *candidate)
{
	if (candidate == NULL) {
		return PROF_ERR_INVALID_ID;
	}
	prof_result_t vr = profiles_validate_custom(candidate);
	if (vr != PROF_OK) {
		/* Preserve known-good: do NOT overwrite m_custom. */
		return vr;
	}
	m_custom = *candidate;
	m_custom.id = PROF_CUSTOM;
	return PROF_OK;
}

void ProfileManager::notifyMotion(void)
{
	m_lastMotionUs = timebase_now_us();
}

bool ProfileManager::isMotionActive(uint64_t now_us) const
{
	/* 3-second dwell window for desktop_airmouse contextual A. */
	const uint64_t MOTION_DWELL_US = 3000000ULL;
	return (now_us - m_lastMotionUs) < MOTION_DWELL_US;
}

/* ---- Protobuf conversion --------------------------------------------- */

bool ProfileManager::fillRemoteProfile(profile_id_t id,
                                       board_RemoteProfile *out) const
{
	if (out == NULL) {
		return false;
	}
	const prof_profile_t *src = NULL;
	if (id == PROF_CUSTOM) {
		src = &m_custom;
	} else {
		src = profiles_get_builtin(id);
	}
	if (src == NULL) {
		return false;
	}

	memset(out, 0, sizeof(*out));
	out->profile_id = (uint32_t)src->id;
	strncpy(out->name, src->name, sizeof(out->name) - 1);
	out->sensitivity = src->sensitivity;
	out->deadzone = src->deadzone;
	out->pointer_mode = src->pointer_mode;
	out->tilt_mode = src->tilt_mode;

	uint8_t count = src->mappings_count;
	if (count > 16) {
		count = 16;
	}
	out->mappings_count = count;
	for (uint8_t i = 0; i < count; i++) {
		board_ProfileMapping *pm = &out->mappings[i];
		pm->source = static_cast<board_InputSource>(i); /* mappings are in source order */
		pm->action = board_InputAction_INPUT_ACTION_PRESS;
		pm->hid_usage_page = src->mappings[i].usage_page;
		pm->hid_usage = src->mappings[i].usage;
		pm->value = 0;
	}
	return true;
}

prof_result_t ProfileManager::loadRemoteProfile(
	const board_RemoteProfile *in,
	prof_profile_t *out)
{
	if (in == NULL || out == NULL) {
		return PROF_ERR_INVALID_ID;
	}
	memset(out, 0, sizeof(*out));
	out->id = PROF_CUSTOM;
	strncpy(out->name, in->name, PROF_NAME_MAX - 1);
	out->name[PROF_NAME_MAX - 1] = '\0';
	out->sensitivity = in->sensitivity;
	out->deadzone = in->deadzone;
	out->pointer_mode = in->pointer_mode;
	out->tilt_mode = in->tilt_mode;

	uint8_t count = in->mappings_count;
	if (count > PROF_MAPPINGS_MAX) {
		count = PROF_MAPPINGS_MAX;
	}
	out->mappings_count = count;
	for (uint8_t i = 0; i < count; i++) {
		const board_ProfileMapping *pm = &in->mappings[i];
		out->mappings[i].report_id = 0;
		/* Derive report_id from usage_page. */
		switch (pm->hid_usage_page) {
		case PROF_USAGE_PAGE_GENERIC_DESKTOP:
			out->mappings[i].report_id = HIDS_REPORT_ID_MOUSE;
			break;
		case PROF_USAGE_PAGE_KEYBOARD:
			out->mappings[i].report_id = HIDS_REPORT_ID_KEYBOARD;
			break;
		case PROF_USAGE_PAGE_CONSUMER:
			out->mappings[i].report_id = HIDS_REPORT_ID_CONSUMER;
			break;
		default:
			return PROF_ERR_INVALID_USAGE;
		}
		out->mappings[i].usage_page = (uint8_t)pm->hid_usage_page;
		out->mappings[i].usage = (uint16_t)pm->hid_usage;
	}
	return profiles_validate_custom(out);
}

#endif /* BOARD_CLUE */
