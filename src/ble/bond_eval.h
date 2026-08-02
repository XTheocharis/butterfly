/*
 * bond_eval.h - Pure-C bond storage evaluation layer (host-testable, no SDK deps).
 *
 * Contains constants, flash-layout validation, FDS page classification, init
 * policy, CCCD system-attribute lifecycle FSM, Forget Bonds confirmation FSM,
 * and async FDS error classification. The firmware C++ class bond.cpp wraps
 * these evaluations with SDK 15.3 Peer Manager / FDS / nrf_fstorage_sd calls.
 *
 * Compiled on BOTH host (for unit tests) and device (linked into firmware).
 */
#ifndef BOND_EVAL_H
#define BOND_EVAL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Compile-time constants ------------------------------------------ */

/* FDS virtual page configuration (must match sdk_config.h). */
#define BOND_FDS_VIRTUAL_PAGES         3u
#define BOND_FDS_VIRTUAL_PAGE_WORDS    1024u   /* 1024 x 32-bit = 4096 bytes */
#define BOND_FDS_PAGE_SIZE_BYTES       (BOND_FDS_VIRTUAL_PAGE_WORDS * 4u)

/* Flash regions (Adafruit Bootloader layout, verified from bootloader src). */
#define BOND_FLASH_APP_END            0xE0000u
#define BOND_FLASH_APP_DATA_START     0xE0000u
#define BOND_FDS_START                0xF1000u
#define BOND_FDS_END                  0xF4000u  /* exclusive */
#define BOND_BOOTLOADER_START_EXPECTED 0xF4000u

#define BOND_FDS_TOTAL_SIZE           (BOND_FDS_END - BOND_FDS_START)
#define BOND_FDS_PAGE_COUNT_EXPECTED  \
    (BOND_FDS_TOTAL_SIZE / BOND_FDS_PAGE_SIZE_BYTES)

/* Bootloader start read from UICR register at 0x10001208 (UICR.BOOTLOADER). */
#define BOND_UICR_BOOTLOADER_ADDR     0x10001208u

/* ---- Bootloader address verification --------------------------------- */

/*
 * Verify the bootloader start address read at runtime from UICR/MBR.
 * Returns true only if bootloader_start == BOND_BOOTLOADER_START_EXPECTED.
 * If false, bonding storage MUST be disabled — never calculate a different
 * FDS range dynamically.
 */
bool bond_verify_bootloader_start(uint32_t bootloader_start);

/* ---- Address range validation ---------------------------------------- */

/* Returns true if [addr, addr+size) is entirely within FDS region. */
bool bond_addr_in_fds_range(uint32_t addr, uint32_t size);

/* Returns true if addr is at or above bootloader start (forbidden zone). */
bool bond_addr_in_bootloader_zone(uint32_t addr, uint32_t bootloader_start);

/* Returns true if addr is in the app-data gap (0xE0000-0xF0FFF). */
bool bond_addr_in_app_data_gap(uint32_t addr);

/* ---- FDS page classification ----------------------------------------- */

/*
 * FDS page tag occupies the first two 32-bit words of each 4 KiB page.
 * SDK 15.3 FDS writes the page ID as two identical words during fds_init().
 * An erased page has both words as 0xFFFFFFFF.
 * A page tagged "erased by FDS" has word0 == 0x00000000.
 */
#define BOND_FDS_TAG_ERASED_FLASH     0xFFFFFFFFu
#define BOND_FDS_TAG_FDS_ERASED       0x00000000u

typedef enum {
	BOND_PAGE_ERASED     = 0,  /* all 0xFF — safe to init */
	BOND_PAGE_RECOGNIZED = 1,  /* valid FDS tag or records — reopen */
	BOND_PAGE_UNKNOWN    = 2,  /* non-erased, unrecognized — warn */
} bond_page_state_t;

/*
 * Classify a single FDS page.
 * Parameters:
 *   is_all_erased: true if every word in the page is 0xFFFFFFFF.
 *   header_word0:  first 32-bit word (FDS page tag word 0).
 *   header_word1:  second 32-bit word (FDS page tag word 1).
 *
 * Classification:
 *   is_all_erased                           → ERASED
 *   word0 == FDS_TAG_FDS_ERASED (0x00000000) → RECOGNIZED (FDS-managed)
 *   word0 == word1 && word0 != 0xFFFFFFFF   → RECOGNIZED (FDS page ID pair)
 *   otherwise                               → UNKNOWN
 */
bond_page_state_t bond_classify_page(bool is_all_erased,
                                     uint32_t header_word0,
                                     uint32_t header_word1);

/*
 * Detect partial write from power loss.
 * A partially written page has: some non-0xFF words, but header_word0
 * does not match any recognized FDS tag pattern. This is a sub-case of
 * UNKNOWN but returned separately for diagnostics.
 *
 * Returns true if the page shows signs of interrupted write
 * (non-erased bytes present but no valid FDS tag).
 */
bool bond_detect_partial_write(bool is_all_erased,
                               uint32_t header_word0,
                               uint32_t header_word1);

/* ---- Init policy ----------------------------------------------------- */

typedef enum {
	BOND_INIT_OK              = 0,  /* all erased or recognized → proceed */
	BOND_INIT_REOPEN          = 1,  /* recognized data → reopen existing FDS */
	BOND_INIT_WARN_UNKNOWN    = 2,  /* unknown data → require confirmation */
	BOND_INIT_DISABLED_LAYOUT = 3,  /* layout mismatch → bonding disabled */
} bond_init_policy_t;

/*
 * Determine FDS init policy from 3 page states + bootloader verification.
 *
 * If bootloader not verified → DISABLED_LAYOUT (no risky range calculation).
 * If all pages ERASED → OK (fresh init).
 * If all pages ERASED or RECOGNIZED → REOPEN (existing FDS data).
 * If any page UNKNOWN → WARN_UNKNOWN (require 3s A confirmation).
 * Mixed ERASED + RECOGNIZED is also REOPEN (normal after GC).
 */
bond_init_policy_t bond_eval_init_policy(bond_page_state_t page0,
                                         bond_page_state_t page1,
                                         bond_page_state_t page2,
                                         bool bootloader_verified);

/* ---- Confirmation state machine (storage erase) ---------------------- */

typedef enum {
	BOND_CONFIRM_IDLE       = 0,
	BOND_CONFIRM_PENDING    = 1,  /* unknown data detected, awaiting 3s A */
	BOND_CONFIRM_GRANTED    = 2,  /* 3s A hold complete → may erase */
	BOND_CONFIRM_CANCELLED  = 3,  /* user released before 3s */
} bond_confirm_state_t;

typedef enum {
	BOND_CONFIRM_EVT_REQUEST   = 0,  /* unknown data detected */
	BOND_CONFIRM_EVT_HOLD_3S   = 1,  /* 3-second A hold completed */
	BOND_CONFIRM_EVT_RELEASE   = 2,  /* user released button */
	BOND_CONFIRM_EVT_TIMEOUT   = 3,  /* confirmation window expired */
} bond_confirm_event_t;

/*
 * Confirmation FSM for erasing unknown FDS data.
 *   IDLE + REQUEST → PENDING
 *   PENDING + HOLD_3S → GRANTED
 *   PENDING + RELEASE/TIMEOUT → CANCELLED
 *   GRANTED → stays GRANTED (one-shot, reset by caller after erase)
 *   CANCELLED → stays CANCELLED (reset by caller)
 */
bond_confirm_state_t bond_confirm_fsm(bond_confirm_state_t current,
                                      bond_confirm_event_t event);

/* ---- CCCD system-attribute lifecycle --------------------------------- */

/*
 * Tracks the save/restore lifecycle of GATTS system attributes (CCCD
 * subscriptions) for a bonded peer. On disconnect, sd_ble_gatts_sys_attr_get
 * saves the current CCCD state to the bond. On reconnect of a bonded peer,
 * sd_ble_gatts_sys_attr_set restores it before any GATT operations.
 */
typedef enum {
	BOND_CCCD_IDLE          = 0,  /* no bond, no connection */
	BOND_CCCD_BONDED        = 1,  /* paired+bonded, connection active */
	BOND_CCCD_DISCONNECTED  = 2,  /* was bonded, disconnected, sys_attr saved */
	BOND_CCCD_RECONNECTING  = 3,  /* bonded peer reconnecting */
	BOND_CCCD_RESTORED      = 4,  /* sys_attr restored on reconnect */
} bond_cccd_state_t;

typedef enum {
	BOND_CCCD_EVT_BONDED      = 0,  /* pairing complete with bonding */
	BOND_CCCD_EVT_DISCONNECT  = 1,  /* connection lost — save sys_attr */
	BOND_CCCD_EVT_RECONNECT   = 2,  /* bonded peer connected — restore */
	BOND_CCCD_EVT_SYS_ATTR_SET = 3, /* sd_ble_gatts_sys_attr_set done */
	BOND_CCCD_EVT_CLEAR       = 4,  /* bond deleted — reset */
} bond_cccd_event_t;

/*
 * CCCD lifecycle FSM.
 *   IDLE + BONDED → BONDED
 *   BONDED + DISCONNECT → DISCONNECTED (caller saves sys_attr)
 *   DISCONNECTED + RECONNECT → RECONNECTING
 *   RECONNECTING + SYS_ATTR_SET → RESTORED
 *   RESTORED + DISCONNECT → DISCONNECTED
 *   Any + CLEAR → IDLE
 */
bond_cccd_state_t bond_cccd_fsm(bond_cccd_state_t current,
                                bond_cccd_event_t event);

/* ---- System-attribute validation ------------------------------------- */

#define BOND_SYS_ATTR_MAX_LEN  62u  /* BLE_GATTS_ATTR_TAB_SIZE secondary limit */

typedef enum {
	BOND_SYS_ATTR_OK          = 0,
	BOND_SYS_ATTR_NULL        = 1,  /* null data pointer */
	BOND_SYS_ATTR_INVALID_LEN = 2,  /* length 0 or > max */
	BOND_SYS_ATTR_WRONG_PEER  = 3,  /* stored peer_id != expected peer_id */
} bond_sys_attr_validation_t;

/*
 * Validate system-attribute data before passing to sd_ble_gatts_sys_attr_set.
 * Checks: non-null, valid length, peer_id match.
 */
bond_sys_attr_validation_t bond_validate_sys_attr(
    uint16_t peer_id_expected,
    uint16_t peer_id_stored,
    const void *sys_attr_data,
    uint16_t sys_attr_len);

/* Returns true if sys_attr_len is in valid range [1, max]. */
bool bond_sys_attr_len_valid(uint16_t sys_attr_len);

/* ---- Forget Bonds state machine -------------------------------------- */

/*
 * Forget Bonds is SEPARATE from storage erase confirmation.
 * Flow: user requests → MENU_CONF_FORGET_WARN → 3s hold → async PM delete
 * → wait for callback → complete or failed.
 */
typedef enum {
	BOND_FORGET_IDLE         = 0,
	BOND_FORGET_PENDING      = 1,  /* user requested, awaiting confirmation */
	BOND_FORGET_CONFIRMED    = 2,  /* 3s hold complete */
	BOND_FORGET_IN_PROGRESS  = 3,  /* async PM/FDS delete running */
	BOND_FORGET_COMPLETE     = 4,  /* async callback: success */
	BOND_FORGET_FAILED       = 5,  /* async callback: error */
} bond_forget_state_t;

typedef enum {
	BOND_FORGET_EVT_REQUEST     = 0,  /* user initiates forget */
	BOND_FORGET_EVT_CONFIRM     = 1,  /* 3s hold confirmed */
	BOND_FORGET_EVT_CANCEL      = 2,  /* user cancels */
	BOND_FORGET_EVT_ASYNC_START = 3,  /* begin async PM delete */
	BOND_FORGET_EVT_ASYNC_DONE  = 4,  /* callback: success */
	BOND_FORGET_EVT_ASYNC_ERROR = 5,  /* callback: error */
} bond_forget_event_t;

/*
 * Forget Bonds FSM.
 *   IDLE + REQUEST → PENDING
 *   PENDING + CONFIRM → CONFIRMED
 *   PENDING + CANCEL → IDLE
 *   CONFIRMED + ASYNC_START → IN_PROGRESS
 *   IN_PROGRESS + ASYNC_DONE → COMPLETE
 *   IN_PROGRESS + ASYNC_ERROR → FAILED
 *   COMPLETE/FAILED → stay (caller resets to IDLE when ready)
 */
bond_forget_state_t bond_forget_fsm(bond_forget_state_t current,
                                    bond_forget_event_t event);

/* Returns true if the forget state is terminal (COMPLETE or FAILED). */
bool bond_forget_is_terminal(bond_forget_state_t state);

/* Returns true if an async operation is in flight (IN_PROGRESS). */
bool bond_forget_async_in_progress(bond_forget_state_t state);

/* ---- Async FDS error classification ---------------------------------- */

/*
 * Maps SDK 15.3 FDS return codes to a compact classification.
 * The firmware passes the raw ret_code_t; the eval classifies it.
 */
typedef enum {
	BOND_ASYNC_OK         = 0,  /* success */
	BOND_ASYNC_BUSY       = 1,  /* FDS_ERR_BUSY / NRF_ERROR_BUSY — retry later */
	BOND_ASYNC_NOT_FOUND  = 2,  /* FDS_ERR_NOT_FOUND — nothing to delete */
	BOND_ASYNC_ERROR      = 3,  /* other error */
	BOND_ASYNC_TIMEOUT    = 4,  /* async operation timed out (no callback) */
} bond_async_result_t;

/*
 * Classify an FDS/PM async operation result code.
 * Maps known SDK error codes to the compact enum.
 * The firmware passes the SDK ret_code_t; the eval normalizes it.
 */
bond_async_result_t bond_classify_async_result(uint32_t fds_result_code);

/* Returns true if the result is retryable (BUSY or TIMEOUT). */
bool bond_async_is_retryable(bond_async_result_t result);

/* ---- Compile-time assertions ----------------------------------------- */

#if defined(__STDC_VERSION__) && (__STDC_VERSION__ >= 201112L)
_Static_assert(BOND_FDS_VIRTUAL_PAGES == 3u,
    "FDS virtual pages must be 3");
_Static_assert(BOND_FDS_VIRTUAL_PAGE_WORDS == 1024u,
    "FDS virtual page size must be 1024 words (4096 bytes)");
_Static_assert(BOND_FDS_PAGE_COUNT_EXPECTED == 3u,
    "FDS partition must contain exactly 3 pages");
_Static_assert(BOND_BOOTLOADER_START_EXPECTED == 0xF4000u,
    "Bootloader must start at 0xF4000");
_Static_assert(BOND_FDS_END == BOND_BOOTLOADER_START_EXPECTED,
    "FDS end must equal bootloader start (no gap, no overlap)");
#endif

#ifdef __cplusplus
}
#endif

#endif /* BOND_EVAL_H */
