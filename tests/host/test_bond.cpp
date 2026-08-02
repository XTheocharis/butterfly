/*
 * test_bond.cpp - Host tests for bond storage eval layer.
 *
 * Tests all acceptance criteria from Todo 22:
 *   - Bootloader start address verification (0xF4000 expected)
 *   - FDS page classification (erased / recognized / unknown / partial write)
 *   - Init policy: all-erased → OK, recognized → reopen,
 *     unknown → warn + confirm, wrong bootloader → disabled
 *   - Address range validation (FDS region, bootloader zone, app-data gap)
 *   - CCCD lifecycle: bonded → disconnect → reconnect → restore, clear
 *   - System-attribute validation (null, length, peer mismatch)
 *   - Forget Bonds FSM (request → confirm → async → complete/failed)
 *   - Async FDS error classification (BUSY, NOT_FOUND, ERROR, retryable)
 *   - Confirmation FSM (request → pending → granted/cancelled)
 *   - Compile-time constants (3 pages, 1024 words, 0xF4000)
 */
#include "test_framework.h"
#include "bond_eval.h"
#include <string.h>

/* ---- Compile-time constant tests ------------------------------------- */

static void test_constants_fds_pages(void) {
	TEST_ASSERT_EQ_INT(3, BOND_FDS_VIRTUAL_PAGES);
}

static void test_constants_page_words(void) {
	TEST_ASSERT_EQ_INT(1024, BOND_FDS_VIRTUAL_PAGE_WORDS);
}

static void test_constants_page_size_bytes(void) {
	TEST_ASSERT_EQ_INT(4096, BOND_FDS_PAGE_SIZE_BYTES);
}

static void test_constants_fds_region(void) {
	TEST_ASSERT_EQ_INT(0xF1000, BOND_FDS_START);
	TEST_ASSERT_EQ_INT(0xF4000, BOND_FDS_END);
}

static void test_constants_bootloader(void) {
	TEST_ASSERT_EQ_INT(0xF4000, BOND_BOOTLOADER_START_EXPECTED);
}

static void test_constants_fds_end_equals_bootloader(void) {
	TEST_ASSERT_EQ_INT(BOND_FDS_END, BOND_BOOTLOADER_START_EXPECTED);
}

static void test_constants_page_count(void) {
	TEST_ASSERT_EQ_INT(3, BOND_FDS_PAGE_COUNT_EXPECTED);
}

static void test_constants_sys_attr_max(void) {
	TEST_ASSERT(BOND_SYS_ATTR_MAX_LEN > 0, "sys attr max must be positive");
	TEST_ASSERT(BOND_SYS_ATTR_MAX_LEN <= 62, "sys attr max within BLE limit");
}

/* ---- Bootloader verification tests ----------------------------------- */

static void test_bootloader_correct_address(void) {
	TEST_ASSERT(bond_verify_bootloader_start(0xF4000),
	            "0xF4000 must verify");
}

static void test_bootloader_wrong_address_0(void) {
	TEST_ASSERT(!bond_verify_bootloader_start(0),
	            "0x0 must not verify");
}

static void test_bootloader_wrong_address_high(void) {
	TEST_ASSERT(!bond_verify_bootloader_start(0xF8000),
	            "0xF8000 must not verify");
}

static void test_bootloader_erased_uicr(void) {
	/* UICR reads 0xFFFFFFFF when bootloader not configured. */
	TEST_ASSERT(!bond_verify_bootloader_start(0xFFFFFFFF),
	            "erased UICR must not verify");
}

static void test_bootloader_off_by_one(void) {
	TEST_ASSERT(!bond_verify_bootloader_start(0xF4001),
	            "off-by-one must not verify");
}

/* ---- Address range validation tests ---------------------------------- */

static void test_addr_in_fds_range_start(void) {
	TEST_ASSERT(bond_addr_in_fds_range(0xF1000, 4),
	            "FDS start + 4 bytes is in range");
}

static void test_addr_in_fds_range_end_boundary(void) {
	TEST_ASSERT(bond_addr_in_fds_range(0xF3FFC, 4),
	            "last 4 bytes of FDS are in range");
}

static void test_addr_in_fds_range_full_page(void) {
	TEST_ASSERT(bond_addr_in_fds_range(0xF1000, 0x1000),
	            "full first page is in range");
}

static void test_addr_below_fds_range(void) {
	TEST_ASSERT(!bond_addr_in_fds_range(0xF0FFF, 4),
	            "address below FDS is out of range");
}

static void test_addr_crosses_fds_end(void) {
	/* 4 bytes starting at 0xF3FFD would cross 0xF4000. */
	TEST_ASSERT(!bond_addr_in_fds_range(0xF3FFD, 4),
	            "crossing FDS end is out of range");
}

static void test_addr_at_bootloader_start(void) {
	TEST_ASSERT(!bond_addr_in_fds_range(0xF4000, 4),
	            "bootloader start is out of FDS range");
}

static void test_addr_zero_size(void) {
	TEST_ASSERT(!bond_addr_in_fds_range(0xF1000, 0),
	            "zero-size access is invalid");
}

static void test_addr_overflow_guard(void) {
	/* Address near UINT32_MAX should not wrap around into FDS range. */
	TEST_ASSERT(!bond_addr_in_fds_range(0xFFFFFFF0, 0x20),
	            "overflow address must not wrap into FDS");
}

static void test_addr_in_bootloader_zone(void) {
	TEST_ASSERT(bond_addr_in_bootloader_zone(0xF4000, 0xF4000),
	            "0xF4000 is in bootloader zone");
}

static void test_addr_above_bootloader(void) {
	TEST_ASSERT(bond_addr_in_bootloader_zone(0xF5000, 0xF4000),
	            "0xF5000 is above bootloader start");
}

static void test_addr_below_bootloader(void) {
	TEST_ASSERT(!bond_addr_in_bootloader_zone(0xF3FFF, 0xF4000),
	            "0xF3FFF is below bootloader start");
}

static void test_addr_in_app_data_gap(void) {
	TEST_ASSERT(bond_addr_in_app_data_gap(0xE0000),
	            "0xE0000 is in app-data gap");
	TEST_ASSERT(bond_addr_in_app_data_gap(0xF0000),
	            "0xF0000 is in app-data gap");
	TEST_ASSERT(bond_addr_in_app_data_gap(0xF0FFF),
	            "0xF0FFF is in app-data gap");
}

static void test_addr_not_in_app_data_gap(void) {
	TEST_ASSERT(!bond_addr_in_app_data_gap(0xDFFFF),
	            "0xDFFFF is below app-data gap");
	TEST_ASSERT(!bond_addr_in_app_data_gap(0xF1000),
	            "0xF1000 is FDS, not app-data gap");
}

/* ---- Page classification tests --------------------------------------- */

static void test_page_erased_all_ff(void) {
	bond_page_state_t s = bond_classify_page(true, 0xFFFFFFFF, 0xFFFFFFFF);
	TEST_ASSERT(s == BOND_PAGE_ERASED, "all-FF page is ERASED");
}

static void test_page_recognized_fds_erased_tag(void) {
	/* FDS writes 0x00000000 after erase to tag the page. */
	bond_page_state_t s = bond_classify_page(false, 0x00000000, 0xFFFFFFFF);
	TEST_ASSERT(s == BOND_PAGE_RECOGNIZED,
	            "word0=0 is FDS-erased tag → RECOGNIZED");
}

static void test_page_recognized_page_id_pair(void) {
	/* FDS writes page ID as two identical words. */
	bond_page_state_t s = bond_classify_page(false, 0x00000001, 0x00000001);
	TEST_ASSERT(s == BOND_PAGE_RECOGNIZED,
	            "matching word0/word1 page ID → RECOGNIZED");
}

static void test_page_recognized_higher_page_id(void) {
	bond_page_state_t s = bond_classify_page(false, 0x00000005, 0x00000005);
	TEST_ASSERT(s == BOND_PAGE_RECOGNIZED,
	            "page ID 5/5 → RECOGNIZED");
}

static void test_page_unknown_mismatched_words(void) {
	bond_page_state_t s = bond_classify_page(false, 0xDEADBEEF, 0xCAFEBABE);
	TEST_ASSERT(s == BOND_PAGE_UNKNOWN,
	            "mismatched non-FF words → UNKNOWN");
}

static void test_page_unknown_garbage(void) {
	bond_page_state_t s = bond_classify_page(false, 0x12345678, 0x87654321);
	TEST_ASSERT(s == BOND_PAGE_UNKNOWN,
	            "garbage data → UNKNOWN");
}

static void test_page_unknown_word0_ff_word1_not(void) {
	/* Partial write: word0 still FF but word1 has data. */
	bond_page_state_t s = bond_classify_page(false, 0xFFFFFFFF, 0x12345678);
	TEST_ASSERT(s == BOND_PAGE_UNKNOWN,
	            "mixed FF/data → UNKNOWN");
}

static void test_page_erased_overrides_all_params(void) {
	/* is_all_erased=true should return ERASED regardless of header words. */
	bond_page_state_t s = bond_classify_page(true, 0xDEADBEEF, 0x12345678);
	TEST_ASSERT(s == BOND_PAGE_ERASED,
	            "is_all_erased overrides header values");
}

/* ---- Partial write detection tests ----------------------------------- */

static void test_partial_write_detected_on_unknown(void) {
	bool partial = bond_detect_partial_write(false, 0xDEADBEEF, 0x12345678);
	TEST_ASSERT(partial, "unknown non-erased page is partial write");
}

static void test_partial_write_not_detected_on_erased(void) {
	bool partial = bond_detect_partial_write(true, 0xFFFFFFFF, 0xFFFFFFFF);
	TEST_ASSERT(!partial, "erased page is not partial write");
}

static void test_partial_write_not_detected_on_recognized(void) {
	bool partial = bond_detect_partial_write(false, 0x00000001, 0x00000001);
	TEST_ASSERT(!partial, "recognized FDS page is not partial write");
}

static void test_partial_write_not_detected_on_fds_erased(void) {
	bool partial = bond_detect_partial_write(false, 0x00000000, 0xFFFFFFFF);
	TEST_ASSERT(!partial, "FDS-erased tag is not partial write");
}

/* ---- Init policy tests ----------------------------------------------- */

static void test_init_policy_all_erased_bootloader_ok(void) {
	bond_init_policy_t p = bond_eval_init_policy(
		BOND_PAGE_ERASED, BOND_PAGE_ERASED, BOND_PAGE_ERASED, true);
	TEST_ASSERT(p == BOND_INIT_OK,
	            "all erased + bootloader OK → fresh init");
}

static void test_init_policy_all_recognized_bootloader_ok(void) {
	bond_init_policy_t p = bond_eval_init_policy(
		BOND_PAGE_RECOGNIZED, BOND_PAGE_RECOGNIZED, BOND_PAGE_RECOGNIZED,
		true);
	TEST_ASSERT(p == BOND_INIT_REOPEN,
	            "all recognized → reopen existing FDS");
}

static void test_init_policy_mixed_erased_recognized(void) {
	/* Normal after GC: some pages erased, some with data. */
	bond_init_policy_t p = bond_eval_init_policy(
		BOND_PAGE_ERASED, BOND_PAGE_RECOGNIZED, BOND_PAGE_ERASED, true);
	TEST_ASSERT(p == BOND_INIT_REOPEN,
	            "mixed erased/recognized → reopen");
}

static void test_init_policy_unknown_requires_confirm(void) {
	bond_init_policy_t p = bond_eval_init_policy(
		BOND_PAGE_ERASED, BOND_PAGE_UNKNOWN, BOND_PAGE_ERASED, true);
	TEST_ASSERT(p == BOND_INIT_WARN_UNKNOWN,
	            "unknown page → warn + require confirmation");
}

static void test_init_policy_all_unknown(void) {
	bond_init_policy_t p = bond_eval_init_policy(
		BOND_PAGE_UNKNOWN, BOND_PAGE_UNKNOWN, BOND_PAGE_UNKNOWN, true);
	TEST_ASSERT(p == BOND_INIT_WARN_UNKNOWN,
	            "all unknown → warn");
}

static void test_init_policy_bootloader_not_verified_disables(void) {
	bond_init_policy_t p = bond_eval_init_policy(
		BOND_PAGE_ERASED, BOND_PAGE_ERASED, BOND_PAGE_ERASED, false);
	TEST_ASSERT(p == BOND_INIT_DISABLED_LAYOUT,
	            "wrong bootloader → disable, never calculate range");
}

static void test_init_policy_bootloader_not_verified_ignores_pages(void) {
	/* Even if pages are recognized, wrong bootloader disables. */
	bond_init_policy_t p = bond_eval_init_policy(
		BOND_PAGE_RECOGNIZED, BOND_PAGE_RECOGNIZED, BOND_PAGE_UNKNOWN,
		false);
	TEST_ASSERT(p == BOND_INIT_DISABLED_LAYOUT,
	            "wrong bootloader overrides page states");
}

static void test_init_policy_unknown_and_recognized_still_warns(void) {
	bond_init_policy_t p = bond_eval_init_policy(
		BOND_PAGE_RECOGNIZED, BOND_PAGE_UNKNOWN, BOND_PAGE_RECOGNIZED,
		true);
	TEST_ASSERT(p == BOND_INIT_WARN_UNKNOWN,
	            "any unknown page with recognized → warn");
}

/* ---- Confirmation FSM tests (storage erase) -------------------------- */

static void test_confirm_idle_to_pending(void) {
	bond_confirm_state_t s = bond_confirm_fsm(
		BOND_CONFIRM_IDLE, BOND_CONFIRM_EVT_REQUEST);
	TEST_ASSERT(s == BOND_CONFIRM_PENDING, "IDLE + REQUEST → PENDING");
}

static void test_confirm_pending_to_granted(void) {
	bond_confirm_state_t s = bond_confirm_fsm(
		BOND_CONFIRM_PENDING, BOND_CONFIRM_EVT_HOLD_3S);
	TEST_ASSERT(s == BOND_CONFIRM_GRANTED, "PENDING + HOLD_3S → GRANTED");
}

static void test_confirm_pending_to_cancelled_on_release(void) {
	bond_confirm_state_t s = bond_confirm_fsm(
		BOND_CONFIRM_PENDING, BOND_CONFIRM_EVT_RELEASE);
	TEST_ASSERT(s == BOND_CONFIRM_CANCELLED,
	            "PENDING + RELEASE → CANCELLED");
}

static void test_confirm_pending_to_cancelled_on_timeout(void) {
	bond_confirm_state_t s = bond_confirm_fsm(
		BOND_CONFIRM_PENDING, BOND_CONFIRM_EVT_TIMEOUT);
	TEST_ASSERT(s == BOND_CONFIRM_CANCELLED,
	            "PENDING + TIMEOUT → CANCELLED");
}

static void test_confirm_granted_is_terminal(void) {
	bond_confirm_state_t s = bond_confirm_fsm(
		BOND_CONFIRM_GRANTED, BOND_CONFIRM_EVT_REQUEST);
	TEST_ASSERT(s == BOND_CONFIRM_GRANTED,
	            "GRANTED is terminal, stays GRANTED");
}

static void test_confirm_cancelled_is_terminal(void) {
	bond_confirm_state_t s = bond_confirm_fsm(
		BOND_CONFIRM_CANCELLED, BOND_CONFIRM_EVT_HOLD_3S);
	TEST_ASSERT(s == BOND_CONFIRM_CANCELLED,
	            "CANCELLED is terminal");
}

static void test_confirm_idle_ignores_hold(void) {
	bond_confirm_state_t s = bond_confirm_fsm(
		BOND_CONFIRM_IDLE, BOND_CONFIRM_EVT_HOLD_3S);
	TEST_ASSERT(s == BOND_CONFIRM_IDLE,
	            "IDLE + HOLD without REQUEST → stays IDLE");
}

/* ---- CCCD lifecycle FSM tests ---------------------------------------- */

static void test_cccd_idle_to_bonded(void) {
	bond_cccd_state_t s = bond_cccd_fsm(
		BOND_CCCD_IDLE, BOND_CCCD_EVT_BONDED);
	TEST_ASSERT(s == BOND_CCCD_BONDED, "IDLE + BONDED → BONDED");
}

static void test_cccd_bonded_to_disconnected(void) {
	bond_cccd_state_t s = bond_cccd_fsm(
		BOND_CCCD_BONDED, BOND_CCCD_EVT_DISCONNECT);
	TEST_ASSERT(s == BOND_CCCD_DISCONNECTED,
	            "BONDED + DISCONNECT → DISCONNECTED (save sys_attr)");
}

static void test_cccd_disconnected_to_reconnecting(void) {
	bond_cccd_state_t s = bond_cccd_fsm(
		BOND_CCCD_DISCONNECTED, BOND_CCCD_EVT_RECONNECT);
	TEST_ASSERT(s == BOND_CCCD_RECONNECTING,
	            "DISCONNECTED + RECONNECT → RECONNECTING");
}

static void test_cccd_reconnecting_to_restored(void) {
	bond_cccd_state_t s = bond_cccd_fsm(
		BOND_CCCD_RECONNECTING, BOND_CCCD_EVT_SYS_ATTR_SET);
	TEST_ASSERT(s == BOND_CCCD_RESTORED,
	            "RECONNECTING + SYS_ATTR_SET → RESTORED");
}

static void test_cccd_restored_to_disconnected(void) {
	bond_cccd_state_t s = bond_cccd_fsm(
		BOND_CCCD_RESTORED, BOND_CCCD_EVT_DISCONNECT);
	TEST_ASSERT(s == BOND_CCCD_DISCONNECTED,
	            "RESTORED + DISCONNECT → DISCONNECTED (save again)");
}

static void test_cccd_full_lifecycle(void) {
	/* Full round-trip: bond → disconnect → reconnect → restore → disconnect. */
	bond_cccd_state_t s = BOND_CCCD_IDLE;
	s = bond_cccd_fsm(s, BOND_CCCD_EVT_BONDED);
	TEST_ASSERT(s == BOND_CCCD_BONDED, "step 1: bonded");
	s = bond_cccd_fsm(s, BOND_CCCD_EVT_DISCONNECT);
	TEST_ASSERT(s == BOND_CCCD_DISCONNECTED, "step 2: disconnected");
	s = bond_cccd_fsm(s, BOND_CCCD_EVT_RECONNECT);
	TEST_ASSERT(s == BOND_CCCD_RECONNECTING, "step 3: reconnecting");
	s = bond_cccd_fsm(s, BOND_CCCD_EVT_SYS_ATTR_SET);
	TEST_ASSERT(s == BOND_CCCD_RESTORED, "step 4: restored");
	s = bond_cccd_fsm(s, BOND_CCCD_EVT_DISCONNECT);
	TEST_ASSERT(s == BOND_CCCD_DISCONNECTED, "step 5: disconnected again");
}

static void test_cccd_clear_from_any_state(void) {
	bond_cccd_state_t s = bond_cccd_fsm(
		BOND_CCCD_BONDED, BOND_CCCD_EVT_CLEAR);
	TEST_ASSERT(s == BOND_CCCD_IDLE, "CLEAR from BONDED → IDLE");

	s = bond_cccd_fsm(BOND_CCCD_RESTORED, BOND_CCCD_EVT_CLEAR);
	TEST_ASSERT(s == BOND_CCCD_IDLE, "CLEAR from RESTORED → IDLE");

	s = bond_cccd_fsm(BOND_CCCD_DISCONNECTED, BOND_CCCD_EVT_CLEAR);
	TEST_ASSERT(s == BOND_CCCD_IDLE, "CLEAR from DISCONNECTED → IDLE");
}

static void test_cccd_reconnecting_disconnect_goes_back(void) {
	/* If disconnect happens during reconnect attempt, go back to saved. */
	bond_cccd_state_t s = bond_cccd_fsm(
		BOND_CCCD_RECONNECTING, BOND_CCCD_EVT_DISCONNECT);
	TEST_ASSERT(s == BOND_CCCD_DISCONNECTED,
	            "RECONNECTING + DISCONNECT → DISCONNECTED");
}

static void test_cccd_idle_ignores_disconnect(void) {
	bond_cccd_state_t s = bond_cccd_fsm(
		BOND_CCCD_IDLE, BOND_CCCD_EVT_DISCONNECT);
	TEST_ASSERT(s == BOND_CCCD_IDLE,
	            "IDLE + DISCONNECT → stays IDLE");
}

/* ---- System-attribute validation tests ------------------------------- */

static void test_sys_attr_valid(void) {
	uint8_t data[16] = {0};
	bond_sys_attr_validation_t v = bond_validate_sys_attr(
		1, 1, data, 16);
	TEST_ASSERT(v == BOND_SYS_ATTR_OK, "valid sys attr passes");
}

static void test_sys_attr_null_data(void) {
	bond_sys_attr_validation_t v = bond_validate_sys_attr(
		1, 1, NULL, 16);
	TEST_ASSERT(v == BOND_SYS_ATTR_NULL, "null data rejected");
}

static void test_sys_attr_zero_length(void) {
	uint8_t data[16] = {0};
	bond_sys_attr_validation_t v = bond_validate_sys_attr(
		1, 1, data, 0);
	TEST_ASSERT(v == BOND_SYS_ATTR_INVALID_LEN, "zero length rejected");
}

static void test_sys_attr_too_long(void) {
	uint8_t data[128] = {0};
	bond_sys_attr_validation_t v = bond_validate_sys_attr(
		1, 1, data, BOND_SYS_ATTR_MAX_LEN + 1);
	TEST_ASSERT(v == BOND_SYS_ATTR_INVALID_LEN, "over-max length rejected");
}

static void test_sys_attr_max_boundary_ok(void) {
	uint8_t data[BOND_SYS_ATTR_MAX_LEN] = {0};
	bond_sys_attr_validation_t v = bond_validate_sys_attr(
		1, 1, data, BOND_SYS_ATTR_MAX_LEN);
	TEST_ASSERT(v == BOND_SYS_ATTR_OK, "exactly max length is OK");
}

static void test_sys_attr_wrong_peer(void) {
	uint8_t data[16] = {0};
	bond_sys_attr_validation_t v = bond_validate_sys_attr(
		1, 2, data, 16);
	TEST_ASSERT(v == BOND_SYS_ATTR_WRONG_PEER, "peer mismatch rejected");
}

static void test_sys_attr_wrong_peer_checked_after_len(void) {
	/* Null data with wrong peer → NULL error first (priority order). */
	bond_sys_attr_validation_t v = bond_validate_sys_attr(
		1, 2, NULL, 16);
	TEST_ASSERT(v == BOND_SYS_ATTR_NULL,
	            "null checked before wrong peer");
}

static void test_sys_attr_len_valid_range(void) {
	TEST_ASSERT(bond_sys_attr_len_valid(1), "len 1 valid");
	TEST_ASSERT(bond_sys_attr_len_valid(BOND_SYS_ATTR_MAX_LEN),
	            "max len valid");
	TEST_ASSERT(!bond_sys_attr_len_valid(0), "len 0 invalid");
	TEST_ASSERT(!bond_sys_attr_len_valid(BOND_SYS_ATTR_MAX_LEN + 1),
	            "over-max invalid");
}

/* ---- Forget Bonds FSM tests ------------------------------------------ */

static void test_forget_idle_to_pending(void) {
	bond_forget_state_t s = bond_forget_fsm(
		BOND_FORGET_IDLE, BOND_FORGET_EVT_REQUEST);
	TEST_ASSERT(s == BOND_FORGET_PENDING, "IDLE + REQUEST → PENDING");
}

static void test_forget_pending_to_confirmed(void) {
	bond_forget_state_t s = bond_forget_fsm(
		BOND_FORGET_PENDING, BOND_FORGET_EVT_CONFIRM);
	TEST_ASSERT(s == BOND_FORGET_CONFIRMED, "PENDING + CONFIRM → CONFIRMED");
}

static void test_forget_pending_cancel_to_idle(void) {
	bond_forget_state_t s = bond_forget_fsm(
		BOND_FORGET_PENDING, BOND_FORGET_EVT_CANCEL);
	TEST_ASSERT(s == BOND_FORGET_IDLE, "PENDING + CANCEL → IDLE");
}

static void test_forget_confirmed_to_in_progress(void) {
	bond_forget_state_t s = bond_forget_fsm(
		BOND_FORGET_CONFIRMED, BOND_FORGET_EVT_ASYNC_START);
	TEST_ASSERT(s == BOND_FORGET_IN_PROGRESS,
	            "CONFIRMED + ASYNC_START → IN_PROGRESS");
}

static void test_forget_in_progress_to_complete(void) {
	bond_forget_state_t s = bond_forget_fsm(
		BOND_FORGET_IN_PROGRESS, BOND_FORGET_EVT_ASYNC_DONE);
	TEST_ASSERT(s == BOND_FORGET_COMPLETE,
	            "IN_PROGRESS + ASYNC_DONE → COMPLETE");
}

static void test_forget_in_progress_to_failed(void) {
	bond_forget_state_t s = bond_forget_fsm(
		BOND_FORGET_IN_PROGRESS, BOND_FORGET_EVT_ASYNC_ERROR);
	TEST_ASSERT(s == BOND_FORGET_FAILED,
	            "IN_PROGRESS + ASYNC_ERROR → FAILED");
}

static void test_forget_full_flow_success(void) {
	bond_forget_state_t s = BOND_FORGET_IDLE;
	s = bond_forget_fsm(s, BOND_FORGET_EVT_REQUEST);
	TEST_ASSERT(s == BOND_FORGET_PENDING, "request");
	s = bond_forget_fsm(s, BOND_FORGET_EVT_CONFIRM);
	TEST_ASSERT(s == BOND_FORGET_CONFIRMED, "confirm");
	s = bond_forget_fsm(s, BOND_FORGET_EVT_ASYNC_START);
	TEST_ASSERT(s == BOND_FORGET_IN_PROGRESS, "async start");
	s = bond_forget_fsm(s, BOND_FORGET_EVT_ASYNC_DONE);
	TEST_ASSERT(s == BOND_FORGET_COMPLETE, "complete");
	TEST_ASSERT(bond_forget_is_terminal(s), "COMPLETE is terminal");
}

static void test_forget_full_flow_failure(void) {
	bond_forget_state_t s = BOND_FORGET_IDLE;
	s = bond_forget_fsm(s, BOND_FORGET_EVT_REQUEST);
	s = bond_forget_fsm(s, BOND_FORGET_EVT_CONFIRM);
	s = bond_forget_fsm(s, BOND_FORGET_EVT_ASYNC_START);
	s = bond_forget_fsm(s, BOND_FORGET_EVT_ASYNC_ERROR);
	TEST_ASSERT(s == BOND_FORGET_FAILED, "async error → FAILED");
	TEST_ASSERT(bond_forget_is_terminal(s), "FAILED is terminal");
}

static void test_forget_complete_is_terminal(void) {
	TEST_ASSERT(bond_forget_is_terminal(BOND_FORGET_COMPLETE),
	            "COMPLETE is terminal");
	TEST_ASSERT(bond_forget_is_terminal(BOND_FORGET_FAILED),
	            "FAILED is terminal");
	TEST_ASSERT(!bond_forget_is_terminal(BOND_FORGET_IDLE),
	            "IDLE is not terminal");
	TEST_ASSERT(!bond_forget_is_terminal(BOND_FORGET_IN_PROGRESS),
	            "IN_PROGRESS is not terminal");
}

static void test_forget_in_progress_check(void) {
	TEST_ASSERT(bond_forget_async_in_progress(BOND_FORGET_IN_PROGRESS),
	            "IN_PROGRESS reports async active");
	TEST_ASSERT(!bond_forget_async_in_progress(BOND_FORGET_COMPLETE),
	            "COMPLETE reports async not active");
	TEST_ASSERT(!bond_forget_async_in_progress(BOND_FORGET_IDLE),
	            "IDLE reports async not active");
}

static void test_forget_confirmed_ignores_request(void) {
	bond_forget_state_t s = bond_forget_fsm(
		BOND_FORGET_CONFIRMED, BOND_FORGET_EVT_REQUEST);
	TEST_ASSERT(s == BOND_FORGET_CONFIRMED,
	            "CONFIRMED ignores new REQUEST");
}

static void test_forget_in_progress_ignores_cancel(void) {
	/* Cannot cancel once async delete is in flight. */
	bond_forget_state_t s = bond_forget_fsm(
		BOND_FORGET_IN_PROGRESS, BOND_FORGET_EVT_CANCEL);
	TEST_ASSERT(s == BOND_FORGET_IN_PROGRESS,
	            "IN_PROGRESS ignores CANCEL (async cannot be aborted)");
}

/* ---- Async error classification tests -------------------------------- */

static void test_async_success(void) {
	bond_async_result_t r = bond_classify_async_result(0x00000000);
	TEST_ASSERT(r == BOND_ASYNC_OK, "NRF_SUCCESS → OK");
}

static void test_async_busy(void) {
	bond_async_result_t r = bond_classify_async_result(0x00000003);
	TEST_ASSERT(r == BOND_ASYNC_BUSY, "NRF_ERROR_BUSY → BUSY");
}

static void test_async_not_found(void) {
	bond_async_result_t r = bond_classify_async_result(0x00000A01);
	TEST_ASSERT(r == BOND_ASYNC_NOT_FOUND, "FDS_ERR_NOT_FOUND → NOT_FOUND");
}

static void test_async_unknown_error(void) {
	bond_async_result_t r = bond_classify_async_result(0x00001000);
	TEST_ASSERT(r == BOND_ASYNC_ERROR, "unknown error code → ERROR");
}

static void test_async_retryable_busy(void) {
	TEST_ASSERT(bond_async_is_retryable(BOND_ASYNC_BUSY),
	            "BUSY is retryable");
}

static void test_async_retryable_timeout(void) {
	TEST_ASSERT(bond_async_is_retryable(BOND_ASYNC_TIMEOUT),
	            "TIMEOUT is retryable");
}

static void test_async_not_retryable_ok(void) {
	TEST_ASSERT(!bond_async_is_retryable(BOND_ASYNC_OK),
	            "OK is not retryable");
}

static void test_async_not_retryable_error(void) {
	TEST_ASSERT(!bond_async_is_retryable(BOND_ASYNC_ERROR),
	            "ERROR is not retryable");
}

static void test_async_not_retryable_not_found(void) {
	TEST_ASSERT(!bond_async_is_retryable(BOND_ASYNC_NOT_FOUND),
	            "NOT_FOUND is not retryable");
}

/* ---- Main runner ----------------------------------------------------- */

int main(void) {
	test_framework_init();

	/* Compile-time constants */
	RUN_TEST(test_constants_fds_pages);
	RUN_TEST(test_constants_page_words);
	RUN_TEST(test_constants_page_size_bytes);
	RUN_TEST(test_constants_fds_region);
	RUN_TEST(test_constants_bootloader);
	RUN_TEST(test_constants_fds_end_equals_bootloader);
	RUN_TEST(test_constants_page_count);
	RUN_TEST(test_constants_sys_attr_max);

	/* Bootloader verification */
	RUN_TEST(test_bootloader_correct_address);
	RUN_TEST(test_bootloader_wrong_address_0);
	RUN_TEST(test_bootloader_wrong_address_high);
	RUN_TEST(test_bootloader_erased_uicr);
	RUN_TEST(test_bootloader_off_by_one);

	/* Address range validation */
	RUN_TEST(test_addr_in_fds_range_start);
	RUN_TEST(test_addr_in_fds_range_end_boundary);
	RUN_TEST(test_addr_in_fds_range_full_page);
	RUN_TEST(test_addr_below_fds_range);
	RUN_TEST(test_addr_crosses_fds_end);
	RUN_TEST(test_addr_at_bootloader_start);
	RUN_TEST(test_addr_zero_size);
	RUN_TEST(test_addr_overflow_guard);
	RUN_TEST(test_addr_in_bootloader_zone);
	RUN_TEST(test_addr_above_bootloader);
	RUN_TEST(test_addr_below_bootloader);
	RUN_TEST(test_addr_in_app_data_gap);
	RUN_TEST(test_addr_not_in_app_data_gap);

	/* Page classification */
	RUN_TEST(test_page_erased_all_ff);
	RUN_TEST(test_page_recognized_fds_erased_tag);
	RUN_TEST(test_page_recognized_page_id_pair);
	RUN_TEST(test_page_recognized_higher_page_id);
	RUN_TEST(test_page_unknown_mismatched_words);
	RUN_TEST(test_page_unknown_garbage);
	RUN_TEST(test_page_unknown_word0_ff_word1_not);
	RUN_TEST(test_page_erased_overrides_all_params);

	/* Partial write detection */
	RUN_TEST(test_partial_write_detected_on_unknown);
	RUN_TEST(test_partial_write_not_detected_on_erased);
	RUN_TEST(test_partial_write_not_detected_on_recognized);
	RUN_TEST(test_partial_write_not_detected_on_fds_erased);

	/* Init policy */
	RUN_TEST(test_init_policy_all_erased_bootloader_ok);
	RUN_TEST(test_init_policy_all_recognized_bootloader_ok);
	RUN_TEST(test_init_policy_mixed_erased_recognized);
	RUN_TEST(test_init_policy_unknown_requires_confirm);
	RUN_TEST(test_init_policy_all_unknown);
	RUN_TEST(test_init_policy_bootloader_not_verified_disables);
	RUN_TEST(test_init_policy_bootloader_not_verified_ignores_pages);
	RUN_TEST(test_init_policy_unknown_and_recognized_still_warns);

	/* Confirmation FSM */
	RUN_TEST(test_confirm_idle_to_pending);
	RUN_TEST(test_confirm_pending_to_granted);
	RUN_TEST(test_confirm_pending_to_cancelled_on_release);
	RUN_TEST(test_confirm_pending_to_cancelled_on_timeout);
	RUN_TEST(test_confirm_granted_is_terminal);
	RUN_TEST(test_confirm_cancelled_is_terminal);
	RUN_TEST(test_confirm_idle_ignores_hold);

	/* CCCD lifecycle FSM */
	RUN_TEST(test_cccd_idle_to_bonded);
	RUN_TEST(test_cccd_bonded_to_disconnected);
	RUN_TEST(test_cccd_disconnected_to_reconnecting);
	RUN_TEST(test_cccd_reconnecting_to_restored);
	RUN_TEST(test_cccd_restored_to_disconnected);
	RUN_TEST(test_cccd_full_lifecycle);
	RUN_TEST(test_cccd_clear_from_any_state);
	RUN_TEST(test_cccd_reconnecting_disconnect_goes_back);
	RUN_TEST(test_cccd_idle_ignores_disconnect);

	/* System-attribute validation */
	RUN_TEST(test_sys_attr_valid);
	RUN_TEST(test_sys_attr_null_data);
	RUN_TEST(test_sys_attr_zero_length);
	RUN_TEST(test_sys_attr_too_long);
	RUN_TEST(test_sys_attr_max_boundary_ok);
	RUN_TEST(test_sys_attr_wrong_peer);
	RUN_TEST(test_sys_attr_wrong_peer_checked_after_len);
	RUN_TEST(test_sys_attr_len_valid_range);

	/* Forget Bonds FSM */
	RUN_TEST(test_forget_idle_to_pending);
	RUN_TEST(test_forget_pending_to_confirmed);
	RUN_TEST(test_forget_pending_cancel_to_idle);
	RUN_TEST(test_forget_confirmed_to_in_progress);
	RUN_TEST(test_forget_in_progress_to_complete);
	RUN_TEST(test_forget_in_progress_to_failed);
	RUN_TEST(test_forget_full_flow_success);
	RUN_TEST(test_forget_full_flow_failure);
	RUN_TEST(test_forget_complete_is_terminal);
	RUN_TEST(test_forget_in_progress_check);
	RUN_TEST(test_forget_confirmed_ignores_request);
	RUN_TEST(test_forget_in_progress_ignores_cancel);

	/* Async error classification */
	RUN_TEST(test_async_success);
	RUN_TEST(test_async_busy);
	RUN_TEST(test_async_not_found);
	RUN_TEST(test_async_unknown_error);
	RUN_TEST(test_async_retryable_busy);
	RUN_TEST(test_async_retryable_timeout);
	RUN_TEST(test_async_not_retryable_ok);
	RUN_TEST(test_async_not_retryable_error);
	RUN_TEST(test_async_not_retryable_not_found);

	return test_framework_finish();
}
