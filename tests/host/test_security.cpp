/*
 * test_security.cpp - Host tests for the BLE security eval layer.
 *
 * Covers the security-parameter validator using the BLE_SEC_* constants
 * already in ble_eval.h, plus the key-distribution constants. Does NOT
 * test any state machine (security.cpp uses a simple bool m_secured).
 */
#include "test_framework.h"
#include "security_eval.h"

/* ---- Key-distribution constants --------------------------------------- */

static void test_kdist_own_enc_set(void) {
	TEST_ASSERT_EQ_INT(1, (int)SEC_EVAL_KDIST_OWN_ENC);
}

static void test_kdist_peer_enc_set(void) {
	TEST_ASSERT_EQ_INT(1, (int)SEC_EVAL_KDIST_PEER_ENC);
}

/* ---- Validator: valid combos ------------------------------------------ */

static void test_validate_lesc_just_works_valid(void) {
	TEST_ASSERT(security_eval_validate_sec_params(
	                BLE_SEC_BOND, BLE_SEC_MITM, BLE_SEC_LESC,
	                BLE_SEC_IO_CAPS, BLE_SEC_MIN_KEY_SIZE, BLE_SEC_MAX_KEY_SIZE),
	            "LESC Just Works config must be valid");
}

static void test_validate_no_bond_no_mitm_valid(void) {
	TEST_ASSERT(security_eval_validate_sec_params(0, 0, 0, 3, 7, 16),
	            "no-bond no-MITM min-size-7 must be valid");
}

static void test_validate_max_key_only_valid(void) {
	TEST_ASSERT(security_eval_validate_sec_params(1, 0, 1, 3, 16, 16),
	            "lesc 16-16 must be valid");
}

static void test_validate_mitm_with_io_caps_valid(void) {
	TEST_ASSERT(security_eval_validate_sec_params(1, 1, 1, 1, 16, 16),
	            "MITM with keyboard display must be valid");
}

/* ---- Validator: lesc requires 16-byte key ----------------------------- */

static void test_validate_lesc_short_key_fails(void) {
	TEST_ASSERT(!security_eval_validate_sec_params(1, 0, 1, 3, 7, 16),
	            "LESC + 7-byte key must fail (LESC requires 16)");
}

static void test_validate_lesc_below_16_fails(void) {
	TEST_ASSERT(!security_eval_validate_sec_params(1, 0, 1, 3, 15, 16),
	            "LESC + 15-byte key must fail");
}

/* ---- Validator: key-size range ---------------------------------------- */

static void test_validate_min_above_max_fails(void) {
	TEST_ASSERT(!security_eval_validate_sec_params(1, 0, 0, 3, 16, 7),
	            "min > max must fail");
}

static void test_validate_below_absolute_min_fails(void) {
	TEST_ASSERT(!security_eval_validate_sec_params(1, 0, 0, 3, 6, 16),
	            "key < 7 must fail (BLE absolute minimum)");
}

/* ---- Validator: MITM requires I/O capability -------------------------- */

static void test_validate_mitm_none_caps_fails(void) {
	TEST_ASSERT(!security_eval_validate_sec_params(1, 1, 1, BLE_SEC_IO_CAPS, 16, 16),
	            "MITM + IO_CAPS_NONE must fail");
}

static void test_validate_mitm_with_display_valid(void) {
	TEST_ASSERT(security_eval_validate_sec_params(1, 1, 0, 0, 16, 16),
	            "MITM + display I/O (caps=0) must be valid");
}

int main(void) {
	test_framework_init();

	RUN_TEST(test_kdist_own_enc_set);
	RUN_TEST(test_kdist_peer_enc_set);

	RUN_TEST(test_validate_lesc_just_works_valid);
	RUN_TEST(test_validate_no_bond_no_mitm_valid);
	RUN_TEST(test_validate_max_key_only_valid);
	RUN_TEST(test_validate_mitm_with_io_caps_valid);

	RUN_TEST(test_validate_lesc_short_key_fails);
	RUN_TEST(test_validate_lesc_below_16_fails);
	RUN_TEST(test_validate_min_above_max_fails);
	RUN_TEST(test_validate_below_absolute_min_fails);
	RUN_TEST(test_validate_mitm_none_caps_fails);
	RUN_TEST(test_validate_mitm_with_display_valid);

	return test_framework_finish();
}
