/*
 * test_gatt.cpp - Host tests for the GATT server eval layer.
 *
 * Covers the GATT-specific extraction NOT already covered by test_ble_runtime:
 *   - HID Generic Remote appearance constant
 *   - Connection-handle invalid sentinel + is_connected predicate
 *   - PPCP builder produces the exact expected GAP-unit values
 *   - PPCP validator accepts valid combos and rejects each violation axis
 */
#include "test_framework.h"
#include "gatt_eval.h"

/* ---- Appearance + conn-handle constants ------------------------------- */

static void test_appearance_is_hid_generic_remote(void) {
	TEST_ASSERT_EQ_INT(962, (int)GATT_APPEARANCE_HID_REMOTE);
}

static void test_conn_handle_invalid_sentinel(void) {
	TEST_ASSERT_EQ_INT(0xFFFF, (int)GATT_CONN_HANDLE_INVALID);
}

/* ---- is_connected predicate ------------------------------------------- */

static void test_is_connected_invalid_handle(void) {
	TEST_ASSERT(!gatt_eval_is_connected(GATT_CONN_HANDLE_INVALID),
	            "invalid handle must report not connected");
}

static void test_is_connected_zero_handle(void) {
	TEST_ASSERT(gatt_eval_is_connected(0),
	            "handle 0 is a valid connection");
}

static void test_is_connected_real_handle(void) {
	TEST_ASSERT(gatt_eval_is_connected(0x0001),
	            "handle 1 is a valid connection");
	TEST_ASSERT(gatt_eval_is_connected(0xFFFE),
	            "handle 0xFFFE is a valid connection (last before invalid)");
}

/* ---- PPCP builder ----------------------------------------------------- */

static void test_ppcp_builder_min_interval(void) {
	uint16_t min = 0, max = 0, lat = 0, to = 0;
	gatt_eval_build_ppcp_params(&min, &max, &lat, &to);
	TEST_ASSERT_EQ_INT(6, (int)min);
}

static void test_ppcp_builder_max_interval(void) {
	uint16_t min = 0, max = 0, lat = 0, to = 0;
	gatt_eval_build_ppcp_params(&min, &max, &lat, &to);
	TEST_ASSERT_EQ_INT(12, (int)max);
}

static void test_ppcp_builder_latency_active_zero(void) {
	uint16_t min = 0, max = 0, lat = 0, to = 0;
	gatt_eval_build_ppcp_params(&min, &max, &lat, &to);
	TEST_ASSERT_EQ_INT(0, (int)lat);
}

static void test_ppcp_builder_timeout_400(void) {
	uint16_t min = 0, max = 0, lat = 0, to = 0;
	gatt_eval_build_ppcp_params(&min, &max, &lat, &to);
	TEST_ASSERT_EQ_INT(400, (int)to);
}

static void test_ppcp_builder_null_safe(void) {
	gatt_eval_build_ppcp_params(0, 0, 0, 0);
	TEST_ASSERT(true, "null pointers must not crash");
}

/* ---- PPCP validator: valid combos ------------------------------------- */

static void test_validate_ppcp_active_valid(void) {
	TEST_ASSERT(gatt_eval_validate_ppcp(8, 15, 0, 4000),
	            "active params (8,15,0,4000) must be valid");
}

static void test_validate_ppcp_idle_valid(void) {
	TEST_ASSERT(gatt_eval_validate_ppcp(8, 15, 4, 4000),
	            "idle params (8,15,4,4000) must be valid");
}

static void test_validate_ppcp_wide_range_valid(void) {
	TEST_ASSERT(gatt_eval_validate_ppcp(10, 100, 9, 32000),
	            "wide range (10,100,9,32000) must be valid");
}

/* ---- PPCP validator: invalid combos ----------------------------------- */

static void test_validate_ppcp_min_above_max(void) {
	TEST_ASSERT(!gatt_eval_validate_ppcp(20, 15, 0, 4000),
	            "min > max must fail");
}

static void test_validate_ppcp_min_below_ble_floor(void) {
	TEST_ASSERT(!gatt_eval_validate_ppcp(5, 15, 0, 4000),
	            "min < 8 ms must fail");
}

static void test_validate_ppcp_max_above_ceiling(void) {
	TEST_ASSERT(!gatt_eval_validate_ppcp(8, 5000, 0, 32000),
	            "max > 4000 ms must fail");
}

static void test_validate_ppcp_latency_above_max(void) {
	TEST_ASSERT(!gatt_eval_validate_ppcp(8, 15, 500, 32000),
	            "latency > 499 must fail");
}

static void test_validate_ppcp_timeout_below_floor(void) {
	TEST_ASSERT(!gatt_eval_validate_ppcp(8, 15, 0, 50),
	            "timeout < 100 ms must fail");
}

static void test_validate_ppcp_timeout_too_short_for_latency(void) {
	TEST_ASSERT(!gatt_eval_validate_ppcp(8, 4000, 499, 100),
	            "timeout <= (1+latency)*max*2 must fail");
}

static void test_validate_ppcp_timeout_above_ceiling(void) {
	TEST_ASSERT(!gatt_eval_validate_ppcp(8, 15, 0, 40000),
	            "timeout > 32000 ms must fail");
}

int main(void) {
	test_framework_init();

	RUN_TEST(test_appearance_is_hid_generic_remote);
	RUN_TEST(test_conn_handle_invalid_sentinel);

	RUN_TEST(test_is_connected_invalid_handle);
	RUN_TEST(test_is_connected_zero_handle);
	RUN_TEST(test_is_connected_real_handle);

	RUN_TEST(test_ppcp_builder_min_interval);
	RUN_TEST(test_ppcp_builder_max_interval);
	RUN_TEST(test_ppcp_builder_latency_active_zero);
	RUN_TEST(test_ppcp_builder_timeout_400);
	RUN_TEST(test_ppcp_builder_null_safe);

	RUN_TEST(test_validate_ppcp_active_valid);
	RUN_TEST(test_validate_ppcp_idle_valid);
	RUN_TEST(test_validate_ppcp_wide_range_valid);

	RUN_TEST(test_validate_ppcp_min_above_max);
	RUN_TEST(test_validate_ppcp_min_below_ble_floor);
	RUN_TEST(test_validate_ppcp_max_above_ceiling);
	RUN_TEST(test_validate_ppcp_latency_above_max);
	RUN_TEST(test_validate_ppcp_timeout_below_floor);
	RUN_TEST(test_validate_ppcp_timeout_too_short_for_latency);
	RUN_TEST(test_validate_ppcp_timeout_above_ceiling);

	return test_framework_finish();
}
