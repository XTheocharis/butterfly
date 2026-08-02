/*
 * security.h - BLE security and bonding for CLUE.
 *
 * LESC Just Works pairing with bonding. No MITM (no display/keyboard
 * for passkey). 16-byte encryption key. PM_LESC_ENABLED serviced by
 * calling nrf_ble_lesc_request_handler() in the main loop.
 */
#ifndef SECURITY_H
#define SECURITY_H

#include "ble_eval.h"

#ifdef __cplusplus

#include <stdint.h>

#include "peer_manager.h"
#include "ble.h"

/*
 * SecurityManager wraps the SDK 15.3 Peer Manager for:
 *   - LESC (LE Secure Connections) Just Works pairing
 *   - Bonding (persist bond keys via FDS — Todo 22)
 *   - Connection security state tracking
 *
 * Security parameters (from ble_eval.h):
 *   bond=true, mitm=false, lesc=true, keypress=false
 *   io_caps=NONE, min_key_size=16, max_key_size=16
 */
class SecurityManager {
public:
	SecurityManager();
	~SecurityManager();

	/* Initialize Peer Manager with LESC config. */
	bool init(void);

	/* Handle BLE events (security establishment, disconnection). */
	void onBleEvent(const ble_evt_t *p_ble_evt);

	/* Service LESC handler. Must be called from main loop. */
	void processLesc(void);

	/* Returns true if the current connection is secured (encrypted). */
	bool isSecured(void) const { return m_secured; }

private:
	bool m_secured;

	/* PM event handler (static → dispatches to singleton). */
	static void pmEventHandler(pm_evt_t const *p_evt);
};

#endif /* __cplusplus */

#endif /* SECURITY_H */
