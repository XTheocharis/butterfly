/*
 * bond.h - Peer Manager + FDS bond storage for CLUE BLE-HID.
 *
 * Wraps bond_eval pure-logic with SDK 15.3 Peer Manager / FDS calls.
 *
 * Responsibilities:
 *   - Verify bootloader start from UICR before enabling storage
 *   - Configure FDS: 3 virtual pages, 1024 words each, at 0xF1000-0xF3FFF
 *   - Use nrf_fstorage_sd ONLY (never nrf_fstorage_nvmc with S140)
 *   - Classify FDS pages before init (erased/recognized/unknown)
 *   - Persist/restore bonded peer's GATTS system attributes (CCCD state)
 *   - Forget Bonds: separate confirmation, async PM/FDS completion
 *
 * Must be called AFTER BleRuntime::init() has enabled S140.
 */
#ifndef BOND_H
#define BOND_H

#include "bond_eval.h"

#ifdef __cplusplus

#include <stdint.h>

#include "ble.h"
#include "peer_manager.h"
#include "nrf_fstorage.h"

/* FDS file and record keys for Butterfly bond data. */
#define BOND_FDS_FILE_ID        0xB0B0u
#define BOND_FDS_RECORD_KEY_SYS 0x0001u  /* sys_attr blob */

/*
 * BondStorage coordinates FDS initialization and CCCD persistence.
 *
 * Lifecycle:
 *   1. verifyBootloader() — read UICR, validate via bond_eval
 *   2. inspectPages() — classify FDS pages, determine init policy
 *   3. init() — call fds_init() with nrf_fstorage_sd backend
 *   4. On disconnect: saveCCCD() — sd_ble_gatts_sys_attr_get → FDS write
 *   5. On reconnect: restoreCCCD() — FDS read → sd_ble_gatts_sys_attr_set
 *   6. forgetBonds() — async PM delete with callback
 */
class BondStorage {
public:
	BondStorage();
	~BondStorage();

	/* Read UICR bootloader address and verify via bond_eval.
	 * Returns true if bootloader_start == 0xF4000. */
	bool verifyBootloader(void);

	/* Classify FDS pages by reading flash (via nrf_fstorage_sd).
	 * Populates m_pageState[3] and m_initPolicy.
	 * Returns the init policy. */
	bond_init_policy_t inspectPages(void);

	/* Full initialization sequence:
	 *   verifyBootloader → inspectPages → init policy check → fds_init.
	 * Returns true only if FDS is ready for use.
	 * If init policy is WARN_UNKNOWN, requires prior confirmation. */
	bool init(void);

	/* --- Confirmation flow for unknown FDS data --- */

	/* Call when unknown data is detected and user must confirm.
	 * Feeds BOND_CONFIRM_EVT_REQUEST to the FSM. */
	void requestEraseConfirmation(void);

	/* Call on 3-second A hold complete.
	 * Feeds BOND_CONFIRM_EVT_HOLD_3S. Returns true if GRANTED. */
	bool confirmEraseHold(void);

	/* Call on button release before 3s.
	 * Feeds BOND_CONFIRM_EVT_RELEASE. */
	void cancelEraseConfirmation(void);

	/* Returns current erase confirmation state. */
	bond_confirm_state_t getConfirmState(void) const { return m_confirmState; }

	/* Returns true if erase is confirmed (may proceed to erase + init). */
	bool isEraseConfirmed(void) const;

	/* --- CCCD system-attribute save/restore --- */

	/* Save CCCD state on disconnect.
	 * Calls sd_ble_gatts_sys_attr_get, validates, writes to FDS.
	 * Returns true on success. */
	bool saveCCCD(uint16_t conn_handle, uint16_t peer_id);

	/* Restore CCCD state on reconnect of bonded peer.
	 * Reads from FDS, validates, calls sd_ble_gatts_sys_attr_set.
	 * Returns true on success. */
	bool restoreCCCD(uint16_t conn_handle, uint16_t peer_id);

	/* Returns current CCCD lifecycle state. */
	bond_cccd_state_t getCccdState(void) const { return m_cccdState; }

	/* Handle BLE events for CCCD lifecycle (connect/disconnect/security). */
	void onBleEvent(const ble_evt_t *p_ble_evt);

	/* --- Forget Bonds --- */

	/* Request forget bonds (separate from storage erase). */
	void requestForget(void);

	/* Confirm forget (3s hold complete). Begins async PM delete. */
	bool confirmForget(void);

	/* Cancel forget request. */
	void cancelForget(void);

	/* Returns current forget state. */
	bond_forget_state_t getForgetState(void) const { return m_forgetState; }

	/* Returns true if async forget operation is in flight. */
	bool isForgetInProgress(void) const;

	/* --- Status queries --- */

	/* Returns true if FDS is initialized and ready. */
	bool isReady(void) const { return m_ready; }

	/* Returns true if a bonded peer ID is currently tracked. */
	bool hasBondedPeer(void) const { return m_bondedPeerId != 0xFFFFu; }

	/* Returns the bonded peer ID (0xFFFF if none). */
	uint16_t getBondedPeerId(void) const { return m_bondedPeerId; }

	/* Returns the init policy from last inspectPages(). */
	bond_init_policy_t getInitPolicy(void) const { return m_initPolicy; }

	/* Returns the page state for page index [0-2]. */
	bond_page_state_t getPageState(uint8_t page_idx) const;

	/* Returns the verified bootloader start address (0 if not verified). */
	uint32_t getBootloaderStart(void) const { return m_bootloaderStart; }

private:
	bool                m_ready;
	uint32_t            m_bootloaderStart;
	bond_init_policy_t  m_initPolicy;
	bond_page_state_t   m_pageState[BOND_FDS_VIRTUAL_PAGES];
	bond_confirm_state_t m_confirmState;
	bond_cccd_state_t   m_cccdState;
	bond_forget_state_t m_forgetState;

	/* Current bonded peer ID (for CCCD save/restore). */
	uint16_t            m_bondedPeerId;

	/* sys_attr scratch buffer (on-stack during save/restore calls). */
	uint8_t            *m_sysAttrBuf;
	uint16_t            m_sysAttrLen;

	/* Direct fstorage instance (SD-backed) for raw flash access. FDS
	 * uses its own internal backend; this is for direct reads/writes
	 * outside the FDS abstraction. */
	nrf_fstorage_t      m_fstorage;

	/* Configure nrf_fstorage_sd backend for FDS. */
	bool configureFstorage(void);

	/* Read a flash word via nrf_fstorage_sd (async wait). */
	bool readFlashWord(uint32_t addr, uint32_t *out);

	/* Check if an entire FDS page is all-0xFF. */
	bool isPageAllErased(uint32_t page_addr);

	/* PM event handler (static → dispatches to singleton). */
	static void pmEventHandler(pm_evt_t const *p_evt);
};

#endif /* __cplusplus */

#endif /* BOND_H */
