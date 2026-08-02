/*
 * security.cpp - BLE security implementation.
 *
 * Initializes Peer Manager with LESC Just Works config and services
 * the LESC handler in the main loop. Bond storage (FDS) is configured
 * by Todo 22; this file sets the security parameter foundation.
 */
#include "security.h"

#ifdef __cplusplus

#include "peer_manager.h"
#include "peer_manager_handler.h"
#include "ble_gap.h"
#include "ble.h"
#include "nrf_ble_lesc.h"

static SecurityManager *g_security = nullptr;

SecurityManager::SecurityManager()
	: m_secured(false)
{
}

SecurityManager::~SecurityManager() {
	g_security = nullptr;
}

void SecurityManager::pmEventHandler(pm_evt_t const *p_evt) {
	if (g_security == nullptr || p_evt == nullptr) return;

	switch (p_evt->evt_id) {
	case PM_EVT_CONN_SEC_SUCCEEDED:
		g_security->m_secured = true;
		break;

	case PM_EVT_CONN_SEC_FAILED:
		g_security->m_secured = false;
		break;

	case PM_EVT_CONN_SEC_CONFIG_REQ:
		/* Allow pairing request from peer. PM_LESC handles the
		 * DH-key exchange internally when PM_LESC_ENABLED is set. */
		break;

	case PM_EVT_PEER_DATA_UPDATE_SUCCEEDED:
		/* Bond data written to FDS. Todo 22 validates the storage. */
		break;

	case PM_EVT_PEERS_DELETE_SUCCEEDED:
		/* All bonds erased — restart advertising. */
		break;

	default:
		break;
	}
}

bool SecurityManager::init(void) {
	g_security = this;

	ret_code_t err = pm_init();
	if (err != NRF_SUCCESS) return false;

	/* Security parameters: LESC Just Works.
	 * bond=1, mitm=0, lesc=1, keypress=0, io_caps=NONE, key_size=16.
	 * Key distribution: enc (LTK/EDIV/RAND) only — no signing, no ID. */
	ble_gap_sec_params_t secParam = {};
	secParam.bond         = BLE_SEC_BOND;
	secParam.mitm         = BLE_SEC_MITM;
	secParam.lesc         = BLE_SEC_LESC;
	secParam.keypress     = BLE_SEC_KEYPRESS;
	secParam.io_caps      = BLE_SEC_IO_CAPS;
	secParam.min_key_size = BLE_SEC_MIN_KEY_SIZE;
	secParam.max_key_size = BLE_SEC_MAX_KEY_SIZE;
	secParam.kdist_own.enc = 1;
	secParam.kdist_peer.enc = 1;

	err = pm_register(pmEventHandler);
	if (err != NRF_SUCCESS) return false;

	/* Set static security parameters. PM uses these when the peer
	 * initiates pairing or when we call pm_conn_secure(). */
	err = pm_sec_params_set(&secParam);
	return err == NRF_SUCCESS;
}

void SecurityManager::onBleEvent(const ble_evt_t *p_ble_evt) {
	if (p_ble_evt == nullptr) return;

	/* PM SDK 15.3 secures the link on connection event. */
	pm_handler_secure_on_connection(p_ble_evt);

	switch (p_ble_evt->header.evt_id) {
	case BLE_GAP_EVT_DISCONNECTED:
		m_secured = false;
		break;

	default:
		break;
	}
}

void SecurityManager::processLesc(void) {
	/* Must be called in every main loop iteration while BLE is active.
	 * Services ongoing LE Secure Connections DH-key exchanges. */
	nrf_ble_lesc_request_handler();
}

#endif /* __cplusplus */
