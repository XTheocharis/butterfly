/*
 * bond_eval.c - Pure-C bond storage evaluation implementations.
 *
 * No SDK dependencies. Compiled on both host (tests) and device (firmware).
 */
#include "bond_eval.h"
#include <stddef.h>

/* ---- Bootloader address verification --------------------------------- */

bool bond_verify_bootloader_start(uint32_t bootloader_start) {
	return bootloader_start == BOND_BOOTLOADER_START_EXPECTED;
}

/* ---- Address range validation ---------------------------------------- */

bool bond_addr_in_fds_range(uint32_t addr, uint32_t size) {
	/* Guard against overflow: addr + size must not wrap. */
	if (size == 0u) {
		return false;
	}
	if (addr < BOND_FDS_START) {
		return false;
	}
	/* Addresses at or above FDS_END are out of range. This also catches
	 * high addresses (near UINT32_MAX) that would wrap in subtraction. */
	if (addr >= BOND_FDS_END) {
		return false;
	}
	/* Check upper bound: addr + size <= BOND_FDS_END.
	 * addr < FDS_END is guaranteed above, so subtraction is safe. */
	if ((BOND_FDS_END - addr) < size) {
		return false;
	}
	return true;
}

bool bond_addr_in_bootloader_zone(uint32_t addr, uint32_t bootloader_start) {
	return addr >= bootloader_start;
}

bool bond_addr_in_app_data_gap(uint32_t addr) {
	return addr >= BOND_FLASH_APP_DATA_START && addr < BOND_FDS_START;
}

/* ---- FDS page classification ----------------------------------------- */

bond_page_state_t bond_classify_page(bool is_all_erased,
                                     uint32_t header_word0,
                                     uint32_t header_word1) {
	if (is_all_erased) {
		return BOND_PAGE_ERASED;
	}

	/* FDS-managed erased page: word0 is 0x00000000 (FDS writes this
	 * after erase to mark the page as available). */
	if (header_word0 == BOND_FDS_TAG_FDS_ERASED) {
		return BOND_PAGE_RECOGNIZED;
	}

	/* FDS page ID pair: SDK 15.3 writes the page ID as two identical
	 * words. If they match and are not 0xFFFFFFFF, this is FDS data. */
	if (header_word0 == header_word1 &&
	    header_word0 != BOND_FDS_TAG_ERASED_FLASH) {
		return BOND_PAGE_RECOGNIZED;
	}

	return BOND_PAGE_UNKNOWN;
}

bool bond_detect_partial_write(bool is_all_erased,
                               uint32_t header_word0,
                               uint32_t header_word1) {
	if (is_all_erased) {
		return false;
	}

	/* If the page is recognized as valid FDS, it's not a partial write. */
	bond_page_state_t state = bond_classify_page(is_all_erased,
	                                              header_word0,
	                                              header_word1);
	if (state == BOND_PAGE_RECOGNIZED) {
		return false;
	}

	/* Non-erased page with no valid FDS tag = partial write or garbage. */
	return true;
}

/* ---- Init policy ----------------------------------------------------- */

bond_init_policy_t bond_eval_init_policy(bond_page_state_t page0,
                                         bond_page_state_t page1,
                                         bond_page_state_t page2,
                                         bool bootloader_verified) {
	/* If bootloader address doesn't match expected, disable bonding.
	 * Never calculate a different FDS range dynamically. */
	if (!bootloader_verified) {
		return BOND_INIT_DISABLED_LAYOUT;
	}

	/* Check for unknown data — requires confirmation before init. */
	if (page0 == BOND_PAGE_UNKNOWN ||
	    page1 == BOND_PAGE_UNKNOWN ||
	    page2 == BOND_PAGE_UNKNOWN) {
		return BOND_INIT_WARN_UNKNOWN;
	}

	/* All pages are ERASED or RECOGNIZED.
	 * If any page is RECOGNIZED, we have existing FDS data → reopen.
	 * If all ERASED → fresh init. */
	if (page0 == BOND_PAGE_RECOGNIZED ||
	    page1 == BOND_PAGE_RECOGNIZED ||
	    page2 == BOND_PAGE_RECOGNIZED) {
		return BOND_INIT_REOPEN;
	}

	/* All erased. */
	return BOND_INIT_OK;
}

/* ---- Confirmation state machine (storage erase) ---------------------- */

bond_confirm_state_t bond_confirm_fsm(bond_confirm_state_t current,
                                      bond_confirm_event_t event) {
	switch (current) {
	case BOND_CONFIRM_IDLE:
		if (event == BOND_CONFIRM_EVT_REQUEST) {
			return BOND_CONFIRM_PENDING;
		}
		return current;

	case BOND_CONFIRM_PENDING:
		if (event == BOND_CONFIRM_EVT_HOLD_3S) {
			return BOND_CONFIRM_GRANTED;
		}
		if (event == BOND_CONFIRM_EVT_RELEASE ||
		    event == BOND_CONFIRM_EVT_TIMEOUT) {
			return BOND_CONFIRM_CANCELLED;
		}
		return current;

	case BOND_CONFIRM_GRANTED:
		/* Terminal — caller resets to IDLE after erase completes. */
		return current;

	case BOND_CONFIRM_CANCELLED:
		/* Terminal — caller resets to IDLE. */
		return current;

	default:
		return current;
	}
}

/* ---- CCCD system-attribute lifecycle --------------------------------- */

bond_cccd_state_t bond_cccd_fsm(bond_cccd_state_t current,
                                bond_cccd_event_t event) {
	/* CLEAR resets from any state. */
	if (event == BOND_CCCD_EVT_CLEAR) {
		return BOND_CCCD_IDLE;
	}

	switch (current) {
	case BOND_CCCD_IDLE:
		if (event == BOND_CCCD_EVT_BONDED) {
			return BOND_CCCD_BONDED;
		}
		return current;

	case BOND_CCCD_BONDED:
		if (event == BOND_CCCD_EVT_DISCONNECT) {
			return BOND_CCCD_DISCONNECTED;
		}
		return current;

	case BOND_CCCD_DISCONNECTED:
		if (event == BOND_CCCD_EVT_RECONNECT) {
			return BOND_CCCD_RECONNECTING;
		}
		return current;

	case BOND_CCCD_RECONNECTING:
		if (event == BOND_CCCD_EVT_SYS_ATTR_SET) {
			return BOND_CCCD_RESTORED;
		}
		/* If disconnected during reconnect attempt, go back to saved. */
		if (event == BOND_CCCD_EVT_DISCONNECT) {
			return BOND_CCCD_DISCONNECTED;
		}
		return current;

	case BOND_CCCD_RESTORED:
		if (event == BOND_CCCD_EVT_DISCONNECT) {
			return BOND_CCCD_DISCONNECTED;
		}
		return current;

	default:
		return current;
	}
}

/* ---- System-attribute validation ------------------------------------- */

bool bond_sys_attr_len_valid(uint16_t sys_attr_len) {
	return sys_attr_len >= 1u && sys_attr_len <= BOND_SYS_ATTR_MAX_LEN;
}

bond_sys_attr_validation_t bond_validate_sys_attr(
    uint16_t peer_id_expected,
    uint16_t peer_id_stored,
    const void *sys_attr_data,
    uint16_t sys_attr_len) {
	if (sys_attr_data == NULL) {
		return BOND_SYS_ATTR_NULL;
	}
	if (!bond_sys_attr_len_valid(sys_attr_len)) {
		return BOND_SYS_ATTR_INVALID_LEN;
	}
	if (peer_id_stored != peer_id_expected) {
		return BOND_SYS_ATTR_WRONG_PEER;
	}
	return BOND_SYS_ATTR_OK;
}

/* ---- Forget Bonds state machine -------------------------------------- */

bond_forget_state_t bond_forget_fsm(bond_forget_state_t current,
                                    bond_forget_event_t event) {
	switch (current) {
	case BOND_FORGET_IDLE:
		if (event == BOND_FORGET_EVT_REQUEST) {
			return BOND_FORGET_PENDING;
		}
		return current;

	case BOND_FORGET_PENDING:
		if (event == BOND_FORGET_EVT_CONFIRM) {
			return BOND_FORGET_CONFIRMED;
		}
		if (event == BOND_FORGET_EVT_CANCEL) {
			return BOND_FORGET_IDLE;
		}
		return current;

	case BOND_FORGET_CONFIRMED:
		if (event == BOND_FORGET_EVT_ASYNC_START) {
			return BOND_FORGET_IN_PROGRESS;
		}
		return current;

	case BOND_FORGET_IN_PROGRESS:
		if (event == BOND_FORGET_EVT_ASYNC_DONE) {
			return BOND_FORGET_COMPLETE;
		}
		if (event == BOND_FORGET_EVT_ASYNC_ERROR) {
			return BOND_FORGET_FAILED;
		}
		return current;

	case BOND_FORGET_COMPLETE:
		/* Terminal — caller resets to IDLE. */
		return current;

	case BOND_FORGET_FAILED:
		/* Terminal — caller resets to IDLE. */
		return current;

	default:
		return current;
	}
}

bool bond_forget_is_terminal(bond_forget_state_t state) {
	return state == BOND_FORGET_COMPLETE || state == BOND_FORGET_FAILED;
}

bool bond_forget_async_in_progress(bond_forget_state_t state) {
	return state == BOND_FORGET_IN_PROGRESS;
}

/* ---- Async FDS error classification ---------------------------------- */

/*
 * SDK 15.3 FDS error codes (from fds.h):
 *   FDS_SUCCESS              = NRF_SUCCESS          = 0x0000
 *   FDS_ERR_BUSY             = NRF_ERROR_BUSY       = 0x0003 (NRF_ERROR_BUSY)
 *   FDS_ERR_NOT_FOUND        = 0x0A01
 *   FDS_ERR_NO_MEM           = NRF_ERROR_NO_MEM     = 0x0004
 *
 * NRF_ERROR codes (from nrf_error.h):
 *   NRF_SUCCESS              = 0x0000
 *   NRF_ERROR_BUSY           = 0x0003
 *
 * Custom:
 *   BOND_ASYNC_TIMEOUT = firmware-defined when no callback within deadline.
 */
#define NRF_SUCCESS_VAL        0x00000000u
#define NRF_ERROR_BUSY_VAL     0x00000003u
#define FDS_ERR_NOT_FOUND_VAL  0x00000A01u

bond_async_result_t bond_classify_async_result(uint32_t fds_result_code) {
	if (fds_result_code == NRF_SUCCESS_VAL) {
		return BOND_ASYNC_OK;
	}
	if (fds_result_code == NRF_ERROR_BUSY_VAL) {
		return BOND_ASYNC_BUSY;
	}
	if (fds_result_code == FDS_ERR_NOT_FOUND_VAL) {
		return BOND_ASYNC_NOT_FOUND;
	}
	return BOND_ASYNC_ERROR;
}

bool bond_async_is_retryable(bond_async_result_t result) {
	return result == BOND_ASYNC_BUSY || result == BOND_ASYNC_TIMEOUT;
}
