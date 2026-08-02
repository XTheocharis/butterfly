/*
 * bond.cpp - Peer Manager + FDS bond storage implementation for CLUE.
 *
 * SDK 15.3 wrappers around bond_eval pure-logic.
 *
 * CRITICAL: Uses nrf_fstorage_sd ONLY. Never nrf_fstorage_nvmc —
 * direct NVMC access faults when S140 is enabled.
 *
 * Peer Manager handles CCCD sys_attr persistence automatically:
 *   - gatts_cache_manager saves sys_attr on disconnect
 *   - gatts_cache_manager restores sys_attr on bonded reconnect
 * This class tracks the lifecycle state and validates via bond_eval.
 *
 * allow: SIZE_OK — single-class SDK wrapper for PM+FDS+fstorage; all
 *        three subsystems need direct access to BondStorage private
 *        state (m_fstorage, m_bondedPeerId, m_cccdState, FSM fields).
 *        Splitting would require friend-class plumbing or breaking
 *        encapsulation across SDK subsystem boundaries.
 */
#include "bond.h"

#ifdef __cplusplus
#ifdef BOARD_CLUE

#include "ble_eval.h"

#include "nrf.h"
#include "nrf_error.h"
#include "fds.h"
#include "peer_manager.h"
#include "peer_manager_handler.h"
#include "ble_conn_state.h"
#include "ble.h"
#include "app_error.h"
#include "nrf_fstorage_sd.h"
#include <string.h>

/* ---- Singleton for PM event dispatch --------------------------------- */

static BondStorage *g_bondInstance = NULL;

/* ---- PM event handler (dispatches to singleton) ---------------------- */

static void bondPmEventHandler(pm_evt_t const *p_evt)
{
	if (g_bondInstance == NULL || p_evt == NULL) {
		return;
	}

	/* Forward to PM internal sub-managers (gatts_cache_manager for
	 * sys_attr persistence, data manager for bond keys). Skipping this
	 * leaves CCCD state un-persisted across disconnect/reconnect. */
	pm_handler_on_pm_evt(p_evt);

	switch (p_evt->evt_id) {
	case PM_EVT_PEERS_DELETE_SUCCEEDED:
		/* Async forget bonds completed — PM callback received. */
		break;

	case PM_EVT_PEERS_DELETE_FAILED:
		/* Async delete failed. */
		break;

	case PM_EVT_STORAGE_FULL:
		/* FDS queue full — PM will retry internally. */
		break;

	default:
		break;
	}
}

/* ---- BondStorage implementation -------------------------------------- */

BondStorage::BondStorage()
	: m_ready(false)
	, m_bootloaderStart(0)
	, m_initPolicy(BOND_INIT_DISABLED_LAYOUT)
	, m_confirmState(BOND_CONFIRM_IDLE)
	, m_cccdState(BOND_CCCD_IDLE)
	, m_forgetState(BOND_FORGET_IDLE)
	, m_bondedPeerId(0xFFFF)
	, m_sysAttrBuf(NULL)
	, m_sysAttrLen(0)
{
	memset(&m_fstorage, 0, sizeof(m_fstorage));
	for (uint8_t i = 0; i < BOND_FDS_VIRTUAL_PAGES; i++) {
		m_pageState[i] = BOND_PAGE_ERASED;
	}
}

BondStorage::~BondStorage()
{
	if (g_bondInstance == this) {
		g_bondInstance = NULL;
	}
}

/* Configure the fstorage_sd backend for direct (non-FDS) flash access. */
bool BondStorage::configureFstorage(void)
{
	/* Start address covers the FDS region only; writes outside this
	 * range would corrupt bootloader/MBR. */
	m_fstorage.start_addr = BOND_FDS_START;
	m_fstorage.end_addr   = BOND_FDS_END;

	ret_code_t err = nrf_fstorage_init(&m_fstorage, &nrf_fstorage_sd, NULL);
	return err == NRF_SUCCESS;
}

bool BondStorage::verifyBootloader(void)
{
	/* Read bootloader start address from UICR.
	 * UICR.BOOTLOADER is at 0x10001208 per nRF52840 Product Spec section 6.18.9.
	 * A value of 0xFFFFFFFF means no bootloader is configured. */
	m_bootloaderStart = *(volatile uint32_t *)BOND_UICR_BOOTLOADER_ADDR;
	return bond_verify_bootloader_start(m_bootloaderStart);
}

bool BondStorage::isPageAllErased(uint32_t page_addr)
{
	/* Memory-mapped flash reads are always safe, even with S140 active.
	 * Only writes require nrf_fstorage_sd / sd_protected_register_write. */
	for (uint32_t i = 0; i < BOND_FDS_VIRTUAL_PAGE_WORDS; i++) {
		uint32_t word = *(volatile uint32_t *)(page_addr + i * 4u);
		if (word != BOND_FDS_TAG_ERASED_FLASH) {
			return false;
		}
	}
	return true;
}

bool BondStorage::readFlashWord(uint32_t addr, uint32_t *out)
{
	/* Memory-mapped read — safe under S140. */
	*out = *(volatile uint32_t *)addr;
	return true;
}

bond_init_policy_t BondStorage::inspectPages(void)
{
	if (!verifyBootloader()) {
		m_initPolicy = BOND_INIT_DISABLED_LAYOUT;
		return m_initPolicy;
	}

	for (uint8_t i = 0; i < BOND_FDS_VIRTUAL_PAGES; i++) {
		uint32_t page_addr = BOND_FDS_START +
		                     (uint32_t)i * BOND_FDS_PAGE_SIZE_BYTES;

		bool all_erased = isPageAllErased(page_addr);

		uint32_t word0 = BOND_FDS_TAG_ERASED_FLASH;
		uint32_t word1 = BOND_FDS_TAG_ERASED_FLASH;
		if (!all_erased) {
			word0 = *(volatile uint32_t *)page_addr;
			word1 = *(volatile uint32_t *)(page_addr + 4u);
		}

		m_pageState[i] = bond_classify_page(all_erased, word0, word1);
	}

	m_initPolicy = bond_eval_init_policy(
		m_pageState[0], m_pageState[1], m_pageState[2],
		true /* bootloader verified above */);

	return m_initPolicy;
}

bool BondStorage::init(void)
{
	if (!verifyBootloader()) {
		m_initPolicy = BOND_INIT_DISABLED_LAYOUT;
		return false;
	}

	bond_init_policy_t policy = inspectPages();

	switch (policy) {
	case BOND_INIT_OK:
	case BOND_INIT_REOPEN:
		/* Proceed with FDS + PM init. */
		break;

	case BOND_INIT_WARN_UNKNOWN:
		/* Require explicit 3-second A confirmation before init. */
		if (!isEraseConfirmed()) {
			return false;
		}
		/* After confirmation, PM GC handles corrupt pages during init. */
		break;

	case BOND_INIT_DISABLED_LAYOUT:
	default:
		/* Layout mismatch — disable bonding storage entirely. */
		return false;
	}

	/* Register as singleton for PM event dispatch. */
	g_bondInstance = this;

	/* Configure SD-backed fstorage before pm_init() so any direct flash
	 * access cooperates with the SoftDevice flash scheduler. */
	if (!configureFstorage()) {
		g_bondInstance = NULL;
		return false;
	}

	/* Initialize Peer Manager.
	 * PM internally initializes FDS with nrf_fstorage_sd backend
	 * (configured via FDS_BACKEND in sdk_config.h).
	 * NEVER call nrf_fstorage_nvmc_init — direct NVMC faults under S140. */
	ret_code_t err = pm_init();
	if (err != NRF_SUCCESS) {
		g_bondInstance = NULL;
		return false;
	}

	/* Register PM event handler. */
	err = pm_register(bondPmEventHandler);
	if (err != NRF_SUCCESS) {
		g_bondInstance = NULL;
		return false;
	}

	m_ready = true;
	return true;
}

/* --- Confirmation flow (storage erase) --- */

void BondStorage::requestEraseConfirmation(void)
{
	m_confirmState = bond_confirm_fsm(m_confirmState,
	                                  BOND_CONFIRM_EVT_REQUEST);
}

bool BondStorage::confirmEraseHold(void)
{
	m_confirmState = bond_confirm_fsm(m_confirmState,
	                                  BOND_CONFIRM_EVT_HOLD_3S);
	return m_confirmState == BOND_CONFIRM_GRANTED;
}

void BondStorage::cancelEraseConfirmation(void)
{
	m_confirmState = bond_confirm_fsm(m_confirmState,
	                                  BOND_CONFIRM_EVT_RELEASE);
}

bool BondStorage::isEraseConfirmed(void) const
{
	return m_confirmState == BOND_CONFIRM_GRANTED;
}

/* --- FDS sys_attr record layout -------------------------------------- */
/*
 * Persisted CCCD system attribute blob. peer_id matches the value
 * returned by pm_peer_id_get() so reconnects can verify identity.
 */
typedef struct {
	uint16_t peer_id;
	uint16_t len;
	uint8_t  data[BOND_SYS_ATTR_MAX_LEN];
} bond_sys_attr_record_t;


/* --- CCCD save/restore --- */

bool BondStorage::saveCCCD(uint16_t conn_handle, uint16_t peer_id)
{
	/* sys_attr blobs for one service fit in BOND_SYS_ATTR_MAX_LEN. */
	uint8_t sys_attr_buf[BOND_SYS_ATTR_MAX_LEN];
	uint16_t sys_attr_len = sizeof(sys_attr_buf);

	ret_code_t rc = sd_ble_gatts_sys_attr_get(
		conn_handle, sys_attr_buf, &sys_attr_len, 0);

	if (rc != NRF_SUCCESS || sys_attr_len == 0) {
		/* No system attributes — PM may still have bond keys. */
		m_cccdState = bond_cccd_fsm(m_cccdState,
		                            BOND_CCCD_EVT_DISCONNECT);
		return false;
	}

	/* Validate via eval layer. */
	bond_sys_attr_validation_t v = bond_validate_sys_attr(
		peer_id, peer_id, sys_attr_buf, sys_attr_len);
	if (v != BOND_SYS_ATTR_OK) {
		m_cccdState = bond_cccd_fsm(m_cccdState,
		                            BOND_CCCD_EVT_DISCONNECT);
		return false;
	}

	bond_sys_attr_record_t record;
	memset(&record, 0, sizeof(record));
	record.peer_id = peer_id;
	record.len     = sys_attr_len;
	memcpy(record.data, sys_attr_buf, sys_attr_len);

	fds_record_t fds_record;
	memset(&fds_record, 0, sizeof(fds_record));
	fds_record.file_id          = BOND_FDS_FILE_ID;
	fds_record.key              = BOND_FDS_RECORD_KEY_SYS;
	fds_record.data.p_data      = &record;
	fds_record.data.length_words = (sizeof(record) + 3u) / 4u;

	/* Update if a record already exists; otherwise write a new one.
	 * fds_record_update keeps the existing record desc valid and avoids
	 * leaving the old copy until FDS GC reclaims it. */
	fds_record_desc_t desc;
	fds_find_token_t  token;
	memset(&desc, 0, sizeof(desc));
	memset(&token, 0, sizeof(token));

	if (fds_record_find_by_key(BOND_FDS_RECORD_KEY_SYS, &desc, &token)
	    == NRF_SUCCESS) {
		rc = fds_record_update(&desc, &fds_record);
	} else {
		rc = fds_record_write(&desc, &fds_record);
	}

	m_cccdState = bond_cccd_fsm(m_cccdState, BOND_CCCD_EVT_DISCONNECT);
	return rc == NRF_SUCCESS;
}

bool BondStorage::restoreCCCD(uint16_t conn_handle, uint16_t peer_id)
{
	m_cccdState = bond_cccd_fsm(m_cccdState, BOND_CCCD_EVT_RECONNECT);

	fds_record_desc_t desc;
	fds_find_token_t  token;
	memset(&desc, 0, sizeof(desc));
	memset(&token, 0, sizeof(token));

	ret_code_t rc = fds_record_find_by_key(
		BOND_FDS_RECORD_KEY_SYS, &desc, &token);

	if (rc == NRF_SUCCESS) {
		fds_flash_record_t flash_rec;
		memset(&flash_rec, 0, sizeof(flash_rec));
		rc = fds_record_open(&desc, &flash_rec);
		if (rc == NRF_SUCCESS) {
			const bond_sys_attr_record_t *stored =
				(const bond_sys_attr_record_t *)flash_rec.p_data;

			uint16_t stored_len = stored->len;
			if (stored_len > BOND_SYS_ATTR_MAX_LEN) {
				stored_len = BOND_SYS_ATTR_MAX_LEN;
			}

			if (stored->peer_id == peer_id && stored_len > 0u) {
				rc = sd_ble_gatts_sys_attr_set(
					conn_handle,
					stored->data, stored_len,
					BLE_GATTS_SYS_ATTR_FLAG_SYS_SRVCS);
			} else {
				rc = NRF_ERROR_NOT_FOUND;
			}
			(void)fds_record_close(&desc);
		}
	}

	if (rc != NRF_SUCCESS) {
		/* Fall back to PM cache (NULL/0) so a freshly-bonded peer
		 * still works without a stored record. */
		rc = sd_ble_gatts_sys_attr_set(
			conn_handle, NULL, 0, BLE_GATTS_SYS_ATTR_FLAG_SYS_SRVCS);
		if (rc == NRF_SUCCESS) {
			rc = sd_ble_gatts_sys_attr_set(
				conn_handle, NULL, 0,
				BLE_GATTS_SYS_ATTR_FLAG_USR_SRVCS);
		}
	}

	if (rc == NRF_SUCCESS) {
		m_cccdState = bond_cccd_fsm(m_cccdState,
		                            BOND_CCCD_EVT_SYS_ATTR_SET);
		m_bondedPeerId = peer_id;
		return true;
	}

	return false;
}

void BondStorage::onBleEvent(const ble_evt_t *p_ble_evt)
{
	if (p_ble_evt == NULL) {
		return;
	}

	uint16_t conn_handle = p_ble_evt->evt.gap_evt.conn_handle;

	switch (p_ble_evt->header.evt_id) {
	case BLE_GAP_EVT_CONNECTED:
		/* PM identifies bonded peers and fires PM_EVT_BONDED_PEER_CONNECTED.
		 * CCCD restore happens via PM internal handler. */
		break;

	case BLE_GAP_EVT_DISCONNECTED:
		if (m_cccdState == BOND_CCCD_BONDED ||
		    m_cccdState == BOND_CCCD_RESTORED) {
			saveCCCD(conn_handle, m_bondedPeerId);
		}
		m_bondedPeerId = 0xFFFFu;
		break;

	case BLE_GAP_EVT_CONN_SEC_UPDATE:
		{
			/* Security established — bond (if any) is now committed.
			 * Use pm_peer_id_get() to obtain the PM-assigned peer ID;
			 * the conn_handle is NOT a peer ID and conflating them
			 * would corrupt the FDS record key namespace. */
			pm_conn_sec_status_t status;
			ret_code_t rc = pm_conn_sec_status_get(conn_handle, &status);
			if (rc == NRF_SUCCESS && status.encrypted && status.bonded) {
				pm_peer_id_t peer = PM_PEER_ID_INVALID;
				rc = pm_peer_id_get(conn_handle, &peer);
				if (rc == NRF_SUCCESS && peer != PM_PEER_ID_INVALID) {
					m_bondedPeerId = (uint16_t)peer;
					m_cccdState = bond_cccd_fsm(m_cccdState,
					                            BOND_CCCD_EVT_BONDED);
					/* CCCD restore on bonded reconnect. */
					restoreCCCD(conn_handle, m_bondedPeerId);
				}
			}
		}
		break;

	default:
		break;
	}
}

/* --- Forget Bonds --- */

void BondStorage::requestForget(void)
{
	m_forgetState = bond_forget_fsm(m_forgetState,
	                                BOND_FORGET_EVT_REQUEST);
}

bool BondStorage::confirmForget(void)
{
	m_forgetState = bond_forget_fsm(m_forgetState,
	                                BOND_FORGET_EVT_CONFIRM);
	if (m_forgetState != BOND_FORGET_CONFIRMED) {
		return false;
	}

	/* Transition to IN_PROGRESS and begin async PM delete. */
	m_forgetState = bond_forget_fsm(m_forgetState,
	                                BOND_FORGET_EVT_ASYNC_START);

	/* pm_peers_delete is async — completion via PM_EVT_PEERS_DELETE_*. */
	ret_code_t rc = pm_peers_delete();
	bond_async_result_t result = bond_classify_async_result(rc);

	if (result == BOND_ASYNC_OK || result == BOND_ASYNC_BUSY) {
		/* OK: delete started, wait for PM_EVT_PEERS_DELETE_SUCCEEDED.
		 * BUSY: PM queued the request, will process when able. */
		return true;
	}

	/* Error — record failure. */
	m_forgetState = bond_forget_fsm(m_forgetState,
	                                BOND_FORGET_EVT_ASYNC_ERROR);
	return false;
}

void BondStorage::cancelForget(void)
{
	m_forgetState = bond_forget_fsm(m_forgetState,
	                                BOND_FORGET_EVT_CANCEL);
}

bool BondStorage::isForgetInProgress(void) const
{
	return bond_forget_async_in_progress(m_forgetState);
}

/* --- Page state accessor --- */

bond_page_state_t BondStorage::getPageState(uint8_t page_idx) const
{
	if (page_idx >= BOND_FDS_VIRTUAL_PAGES) {
		return BOND_PAGE_UNKNOWN;
	}
	return m_pageState[page_idx];
}

#endif /* BOARD_CLUE */
#endif /* __cplusplus */
