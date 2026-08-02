/*
 * profiles.h - Firmware HID profile manager for BLE-HID runtime.
 *
 * Wraps the pure-C profiles_eval layer with HIDS state mutations.
 * Owns the active profile selection and the custom profile slot.
 *
 * Responsibilities:
 *   - Track the active profile (one of 4 built-ins or custom)
 *   - On physical input (button press, APDS swipe): look up the HID
 *     target in the active profile and drive hids_eval's refcount state
 *   - Desktop air-mouse contextual A mapping (motion-active check)
 *   - Accept/reject custom profile updates (validate before applying,
 *     preserve known-good on failure)
 *   - Convert board_RemoteProfile protobuf to/from prof_profile_t
 *
 * Must be called AFTER HidsService::init() (Todo 21).
 */
#ifndef BLE_PROFILES_H
#define BLE_PROFILES_H

#include "profiles_eval.h"
#include "hids_eval.h"

#ifdef __cplusplus

#include <stdint.h>

#include "board.pb.h"

/* board_RemoteProfile / board_ProfileMapping are nanopb typedefs
 * (typedef struct _board_X {...} board_X;) — anonymous struct tag,
 * cannot be forward declared as `struct board_RemoteProfile`. */

class ProfileManager {
public:
	ProfileManager();

	/* Set the HIDS state pointer (from HidsService::getState()).
	 * Must be called before dispatchInput(). */
	void setHidsState(hids_state_t *state);

	/* Select the active profile. Returns false for invalid IDs. */
	bool selectProfile(profile_id_t id);

	/* Returns the active profile ID. */
	profile_id_t getActiveProfile(void) const { return m_activeId; }

	/* Returns a pointer to the active profile descriptor.
	 * For built-ins, returns the immutable const def.
	 * For custom, returns the mutable custom slot. */
	const prof_profile_t *getActiveProfileDesc(void) const;

	/* Returns a pointer to the custom profile slot (mutable, for
	 * RemoteProfileSet handling). */
	const prof_profile_t *getCustomProfile(void) const;

	/* Dispatch a physical input event to the HIDS refcount state.
	 * pressed: true=press, false=release.
	 * src: which physical source (button A/B, APDS swipe).
	 * motion_active: only used for desktop_airmouse Button A contextual
	 *   mapping (mouse btn 1 vs keyboard Enter).
	 * Returns PROF_OK on success, error code on failure. */
	prof_result_t dispatchInput(bool pressed, prof_src_t src,
	                            bool motion_active);

	/* --- Custom profile management --- */

	/* Apply a candidate custom profile. Validates first; on failure
	 * preserves the existing known-good custom slot and returns the
	 * error. On success, replaces the custom slot and (if active)
	 * switches to it. */
	prof_result_t applyCustomProfile(const prof_profile_t *candidate);

	/* Fill a board_RemoteProfile protobuf from the given profile ID.
	 * For built-ins, reads the const def. For custom, reads the slot.
	 * Returns false if the profile doesn't exist. */
	bool fillRemoteProfile(profile_id_t id,
	                       board_RemoteProfile *out) const;

	/* Load a prof_profile_t from a board_RemoteProfile protobuf.
	 * Performs validation; returns error on invalid data. */
	static prof_result_t loadRemoteProfile(
		const board_RemoteProfile *in,
		prof_profile_t *out);

	/* Motion activity tracking for desktop_airmouse contextual A.
	 * Called by the air-mouse module when motion is produced. */
	void notifyMotion(void);
	bool isMotionActive(uint64_t now_us) const;

private:
	profile_id_t    m_activeId;
	hids_state_t   *m_hids;
	prof_profile_t  m_custom;
	uint64_t        m_lastMotionUs;

	/* Map a prof_hid_target_t to the appropriate HIDS refcount call. */
	prof_result_t applyTarget(bool pressed,
	                          const prof_hid_target_t *target,
	                          hids_src_t hidsSrc);
};

#endif /* __cplusplus */

#endif /* BLE_PROFILES_H */
