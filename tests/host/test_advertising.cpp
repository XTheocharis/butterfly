/*
 * test_advertising.cpp - Host tests for the advertising-specific ble_eval
 * additions and existing ble_eval FSMs that lacked dedicated coverage.
 *
 * Tests:
 *   - ble_eval_state_to_adv_mode mapping
 *   - ble_eval_classify_param_update_error decision logic
 *   - BLE_EVAL_ERR_* error code constants
 *   - Latency value mapping for each state
 *   - FSM transition edge cases not exercised by test_ble_runtime
 */
#include "test_framework.h"
#include "ble_eval.h"

/* ---- state_to_adv_mode mapping ---------------------------------------- */

static void test_adv_mode_idle_is_zero(void) {
	TEST_ASSERT_EQ_INT(0, ble_eval_state_to_adv_mode(BLE_ADV_STATE_IDLE));
}

static void test_adv_mode_fast_is_one(void) {
	TEST_ASSERT_EQ_INT(1, ble_eval_state_to_adv_mode(BLE_ADV_STATE_FAST));
}

static void test_adv_mode_slow_is_two(void) {
	TEST_ASSERT_EQ_INT(2, ble_eval_state_to_adv_mode(BLE_ADV_STATE_SLOW));
}

static void test_adv_mode_directed_is_three(void) {
	TEST_ASSERT_EQ_INT(3, ble_eval_state_to_adv_mode(BLE_ADV_STATE_DIRECTED));
}

static void test_adv_mode_stopped_is_zero(void) {
	TEST_ASSERT_EQ_INT(0, ble_eval_state_to_adv_mode(BLE_ADV_STATE_STOPPED));
}

/* ---- SDK error code constants ----------------------------------------- */

static void test_err_success_is_zero(void) {
	TEST_ASSERT_EQ_INT(0, (int)BLE_EVAL_ERR_SUCCESS);
}

static void test_err_invalid_state_is_three(void) {
	TEST_ASSERT_EQ_INT(3, (int)BLE_EVAL_ERR_INVALID_STATE);
}

/* ---- classify_param_update_error -------------------------------------- */

static void test_classify_success_returns_ok(void) {
	ble_param_update_result_t r =
	    ble_eval_classify_param_update_error(BLE_EVAL_ERR_SUCCESS, true);
	TEST_ASSERT(r == BLE_EVAL_PARAM_UPDATE_OK,
	            "NRF_SUCCESS must classify OK");
}

static void test_classify_success_unconnected_returns_ok(void) {
	ble_param_update_result_t r =
	    ble_eval_classify_param_update_error(BLE_EVAL_ERR_SUCCESS, false);
	TEST_ASSERT(r == BLE_EVAL_PARAM_UPDATE_OK,
	            "NRF_SUCCESS is OK regardless of connection");
}

static void test_classify_invalid_state_connected_rate_limited(void) {
	ble_param_update_result_t r =
	    ble_eval_classify_param_update_error(BLE_EVAL_ERR_INVALID_STATE, true);
	TEST_ASSERT(r == BLE_EVAL_PARAM_UPDATE_RATE_LIMITED,
	            "INVALID_STATE while connected is rate-limited");
}

static void test_classify_invalid_state_unconnected_no_connection(void) {
	ble_param_update_result_t r =
	    ble_eval_classify_param_update_error(BLE_EVAL_ERR_INVALID_STATE, false);
	TEST_ASSERT(r == BLE_EVAL_PARAM_UPDATE_NO_CONNECTION,
	            "INVALID_STATE while disconnected is no-connection");
}

static void test_classify_unknown_error_connected_backoff(void) {
	ble_param_update_result_t r =
	    ble_eval_classify_param_update_error(0x1234u, true);
	TEST_ASSERT(r == BLE_EVAL_PARAM_UPDATE_BACKOFF,
	            "unknown error while connected is backoff");
}

static void test_classify_unknown_error_unconnected_no_connection(void) {
	ble_param_update_result_t r =
	    ble_eval_classify_param_update_error(0x1234u, false);
	TEST_ASSERT(r == BLE_EVAL_PARAM_UPDATE_NO_CONNECTION,
	            "unknown error while disconnected is no-connection");
}

/* ---- Latency value mapping (additional coverage) --------------------- */

static void test_latency_value_active_zero(void) {
	TEST_ASSERT_EQ_INT(0, (int)ble_latency_value(BLE_LAT_STATE_ACTIVE));
}

static void test_latency_value_idle_four(void) {
	TEST_ASSERT_EQ_INT(4, (int)ble_latency_value(BLE_LAT_STATE_IDLE));
}

static void test_latency_value_backoff_four(void) {
	TEST_ASSERT_EQ_INT(4, (int)ble_latency_value(BLE_LAT_STATE_BACKOFF));
}

/* ---- Adv FSM edge cases not in test_ble_runtime ---------------------- */

static void test_adv_fsm_stop_from_idle_stays_idle(void) {
	ble_adv_state_t r = ble_adv_fsm(BLE_ADV_STATE_IDLE,
	                                BLE_ADV_EVT_STOP, false);
	TEST_ASSERT(r == BLE_ADV_STATE_IDLE,
	            "STOP from IDLE must stay IDLE");
}

static void test_adv_fsm_start_from_fast_stays_fast(void) {
	ble_adv_state_t r = ble_adv_fsm(BLE_ADV_STATE_FAST,
	                                BLE_ADV_EVT_START, false);
	TEST_ASSERT(r == BLE_ADV_STATE_FAST,
	            "START from FAST is no-op");
}

static void test_adv_fsm_bonded_peer_ignored_in_slow(void) {
	ble_adv_state_t r = ble_adv_fsm(BLE_ADV_STATE_SLOW,
	                                BLE_ADV_EVT_BONDED_PEER, false);
	TEST_ASSERT(r == BLE_ADV_STATE_SLOW,
	            "BONDED_PEER event has no effect in SLOW state");
}

static void test_adv_fsm_disconnect_with_bond_goes_directed(void) {
	ble_adv_state_t r = ble_adv_fsm(BLE_ADV_STATE_STOPPED,
	                                BLE_ADV_EVT_DISCONNECTED, true);
	TEST_ASSERT(r == BLE_ADV_STATE_DIRECTED,
	            "DISCONNECTED + bond must go DIRECTED");
}

/* ---- Latency FSM edge cases ------------------------------------------ */

static void test_lat_fsm_idle_activity_goes_active(void) {
	ble_latency_state_t r = ble_latency_fsm(BLE_LAT_STATE_IDLE,
	                                        BLE_LAT_EVT_ACTIVITY);
	TEST_ASSERT(r == BLE_LAT_STATE_ACTIVE,
	            "ACTIVITY from IDLE goes ACTIVE");
}

static void test_lat_fsm_active_idle_5s_goes_idle(void) {
	ble_latency_state_t r = ble_latency_fsm(BLE_LAT_STATE_ACTIVE,
	                                        BLE_LAT_EVT_IDLE_5S);
	TEST_ASSERT(r == BLE_LAT_STATE_IDLE,
	            "IDLE_5S from ACTIVE goes IDLE");
}

int main(void) {
	test_framework_init();

	RUN_TEST(test_adv_mode_idle_is_zero);
	RUN_TEST(test_adv_mode_fast_is_one);
	RUN_TEST(test_adv_mode_slow_is_two);
	RUN_TEST(test_adv_mode_directed_is_three);
	RUN_TEST(test_adv_mode_stopped_is_zero);

	RUN_TEST(test_err_success_is_zero);
	RUN_TEST(test_err_invalid_state_is_three);

	RUN_TEST(test_classify_success_returns_ok);
	RUN_TEST(test_classify_success_unconnected_returns_ok);
	RUN_TEST(test_classify_invalid_state_connected_rate_limited);
	RUN_TEST(test_classify_invalid_state_unconnected_no_connection);
	RUN_TEST(test_classify_unknown_error_connected_backoff);
	RUN_TEST(test_classify_unknown_error_unconnected_no_connection);

	RUN_TEST(test_latency_value_active_zero);
	RUN_TEST(test_latency_value_idle_four);
	RUN_TEST(test_latency_value_backoff_four);

	RUN_TEST(test_adv_fsm_stop_from_idle_stays_idle);
	RUN_TEST(test_adv_fsm_start_from_fast_stays_fast);
	RUN_TEST(test_adv_fsm_bonded_peer_ignored_in_slow);
	RUN_TEST(test_adv_fsm_disconnect_with_bond_goes_directed);

	RUN_TEST(test_lat_fsm_idle_activity_goes_active);
	RUN_TEST(test_lat_fsm_active_idle_5s_goes_idle);

	return test_framework_finish();
}
