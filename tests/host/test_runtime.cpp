/*
 * test_runtime.cpp - dual-runtime selection, switching, and policy tests.
 *
 * Covers: runtime precedence (GPREGRET2 > store > raw default),
 * one-shot clearing, invalid value handling, switching state machine,
 * CDC-close policy, capability table registration, mode validation.
 *
 * No hardware required — pure logic with mock backend.
 */
#include "test_framework.h"
#include "../../src/runtime.cpp"

/* ---- Mock backend state ---- */

static uint32_t mock_gpregret2;
static runtime_store_result_t mock_store_result;
static runtime_mode_t mock_store_mode;
static int mock_reset_count;
static int mock_wdt_arm_count;
static bool mock_store_write_called;
static runtime_mode_t mock_store_write_mode;

/* Sequence tracker for ordering verification. Each backend call appends
 * its ordinal so tests can assert on the exact call sequence. */
static int mock_seq_cursor;
static int mock_seq_clear_at;
static int mock_seq_set_at;
static int mock_seq_wdt_at;
static int mock_seq_reset_at;

static uint32_t mock_gpregret2_read(void) { return mock_gpregret2; }
static void mock_gpregret2_clear(void) {
	mock_gpregret2 = 0;
	mock_seq_clear_at = ++mock_seq_cursor;
}
static void mock_gpregret2_set(uint8_t v) {
	mock_gpregret2 = v;
	mock_seq_set_at = ++mock_seq_cursor;
}

static runtime_store_result_t mock_store_read(runtime_mode_t *out) {
	if (mock_store_result == RUNTIME_STORE_OK) {
		*out = mock_store_mode;
	}
	return mock_store_result;
}
static runtime_store_result_t mock_store_write(runtime_mode_t mode) {
	mock_store_write_called = true;
	mock_store_write_mode = mode;
	return RUNTIME_STORE_OK;
}
static void mock_reset(void) {
	mock_reset_count++;
	mock_seq_reset_at = ++mock_seq_cursor;
}
static uint64_t mock_now_us(void) { return 0; }
static void mock_wdt_arm(void) {
	mock_wdt_arm_count++;
	mock_seq_wdt_at = ++mock_seq_cursor;
}
static void mock_wdt_feed(void) {}

static const runtime_backend_t MOCK_BACKEND = {
	mock_gpregret2_read,
	mock_gpregret2_clear,
	mock_gpregret2_set,
	mock_store_read,
	mock_store_write,
	mock_reset,
	mock_now_us,
	mock_wdt_arm,
	mock_wdt_feed,
};

static void reset_mocks(void) {
	mock_gpregret2 = 0;
	mock_store_result = RUNTIME_STORE_UNAVAILABLE;
	mock_store_mode = RUNTIME_RAW_WHAD;
	mock_reset_count = 0;
	mock_wdt_arm_count = 0;
	mock_store_write_called = false;
	mock_store_write_mode = RUNTIME_RAW_WHAD;
	mock_seq_cursor = 0;
	mock_seq_clear_at = 0;
	mock_seq_set_at = 0;
	mock_seq_wdt_at = 0;
	mock_seq_reset_at = 0;
	runtime_init(&MOCK_BACKEND);
}

/* ---- Tests: precedence ---- */

static void test_default_raw_when_no_oneshot_no_store(void) {
	reset_mocks();
	mock_store_result = RUNTIME_STORE_UNAVAILABLE;
	int source = -1;
	runtime_mode_t mode = runtime_select(&source);
	TEST_ASSERT_EQ_INT(RUNTIME_RAW_WHAD, (int)mode);
	TEST_ASSERT_EQ_INT(0, source);
}

static void test_oneshot_raw_selects_raw(void) {
	reset_mocks();
	mock_gpregret2 = RUNTIME_GPREGRET2_RAW_WHAD;
	int source = -1;
	runtime_mode_t mode = runtime_select(&source);
	TEST_ASSERT_EQ_INT(RUNTIME_RAW_WHAD, (int)mode);
	TEST_ASSERT_EQ_INT(2, source);
}

static void test_oneshot_ble_selects_ble(void) {
	reset_mocks();
	mock_gpregret2 = RUNTIME_GPREGRET2_BLE_HID;
	int source = -1;
	runtime_mode_t mode = runtime_select(&source);
	TEST_ASSERT_EQ_INT(RUNTIME_BLE_HID, (int)mode);
	TEST_ASSERT_EQ_INT(2, source);
}

static void test_oneshot_cleared_after_read(void) {
	reset_mocks();
	mock_gpregret2 = RUNTIME_GPREGRET2_BLE_HID;
	runtime_select(NULL);
	TEST_ASSERT_EQ_INT(0, (int)mock_gpregret2);
}

static void test_oneshot_precedence_over_store(void) {
	reset_mocks();
	mock_gpregret2 = RUNTIME_GPREGRET2_RAW_WHAD;
	mock_store_result = RUNTIME_STORE_OK;
	mock_store_mode = RUNTIME_BLE_HID;
	int source = -1;
	runtime_mode_t mode = runtime_select(&source);
	TEST_ASSERT_EQ_INT(RUNTIME_RAW_WHAD, (int)mode);
	TEST_ASSERT_EQ_INT(2, source);
}

static void test_store_precedence_over_default(void) {
	reset_mocks();
	mock_gpregret2 = 0;
	mock_store_result = RUNTIME_STORE_OK;
	mock_store_mode = RUNTIME_BLE_HID;
	int source = -1;
	runtime_mode_t mode = runtime_select(&source);
	TEST_ASSERT_EQ_INT(RUNTIME_BLE_HID, (int)mode);
	TEST_ASSERT_EQ_INT(1, source);
}

/* ---- Tests: invalid one-shot ---- */

static void test_invalid_oneshot_cleared(void) {
	reset_mocks();
	mock_gpregret2 = 0xFF;
	runtime_select(NULL);
	TEST_ASSERT_EQ_INT(0, (int)mock_gpregret2);
}

static void test_invalid_oneshot_falls_through_to_store(void) {
	reset_mocks();
	mock_gpregret2 = 0xAB;
	mock_store_result = RUNTIME_STORE_OK;
	mock_store_mode = RUNTIME_BLE_HID;
	int source = -1;
	runtime_mode_t mode = runtime_select(&source);
	TEST_ASSERT_EQ_INT(RUNTIME_BLE_HID, (int)mode);
	TEST_ASSERT_EQ_INT(1, source);
}

static void test_zero_oneshot_not_cleared(void) {
	reset_mocks();
	mock_gpregret2 = 0;
	runtime_select(NULL);
	TEST_ASSERT_EQ_INT(0, (int)mock_gpregret2);
}

static void test_store_unavailable_falls_to_default(void) {
	reset_mocks();
	mock_gpregret2 = 0;
	mock_store_result = RUNTIME_STORE_UNAVAILABLE;
	int source = -1;
	runtime_mode_t mode = runtime_select(&source);
	TEST_ASSERT_EQ_INT(RUNTIME_RAW_WHAD, (int)mode);
	TEST_ASSERT_EQ_INT(0, source);
}

static void test_store_invalid_falls_to_default(void) {
	reset_mocks();
	mock_gpregret2 = 0;
	mock_store_result = RUNTIME_STORE_INVALID;
	int source = -1;
	runtime_mode_t mode = runtime_select(&source);
	TEST_ASSERT_EQ_INT(RUNTIME_RAW_WHAD, (int)mode);
	TEST_ASSERT_EQ_INT(0, source);
}

static void test_store_io_error_falls_to_default(void) {
	reset_mocks();
	mock_gpregret2 = 0;
	mock_store_result = RUNTIME_STORE_IO_ERROR;
	int source = -1;
	runtime_mode_t mode = runtime_select(&source);
	TEST_ASSERT_EQ_INT(RUNTIME_RAW_WHAD, (int)mode);
	TEST_ASSERT_EQ_INT(0, source);
}

/* ---- Tests: switching ---- */

static void test_switch_same_mode_noop(void) {
	reset_mocks();
	runtime_select(NULL);
	TEST_ASSERT_EQ_INT(RUNTIME_RAW_WHAD, (int)runtime_get_selected());
	runtime_switch_result_t r = runtime_request_switch(RUNTIME_RAW_WHAD);
	TEST_ASSERT_EQ_INT(RUNTIME_SWITCH_OK, (int)r);
	TEST_ASSERT_EQ_INT(0, mock_reset_count);
	TEST_ASSERT_EQ_INT(0, mock_wdt_arm_count);
}

static void test_switch_raw_to_ble_sets_gpregret2(void) {
	reset_mocks();
	runtime_select(NULL);
	runtime_switch_result_t r = runtime_request_switch(RUNTIME_BLE_HID);
	TEST_ASSERT_EQ_INT(RUNTIME_SWITCH_OK, (int)r);
	TEST_ASSERT_EQ_INT(RUNTIME_GPREGRET2_BLE_HID, (int)mock_gpregret2);
}

static void test_switch_ble_to_raw_sets_gpregret2(void) {
	reset_mocks();
	mock_gpregret2 = RUNTIME_GPREGRET2_BLE_HID;
	runtime_select(NULL);
	TEST_ASSERT_EQ_INT(RUNTIME_BLE_HID, (int)runtime_get_selected());
	runtime_switch_result_t r = runtime_request_switch(RUNTIME_RAW_WHAD);
	TEST_ASSERT_EQ_INT(RUNTIME_SWITCH_OK, (int)r);
	TEST_ASSERT_EQ_INT(RUNTIME_GPREGRET2_RAW_WHAD, (int)mock_gpregret2);
}

static void test_switch_clears_before_set(void) {
	reset_mocks();
	mock_gpregret2 = 0xEE;
	runtime_set_selected(RUNTIME_RAW_WHAD);
	runtime_set_state(RUNTIME_STATE_RUNNING);
	runtime_request_switch(RUNTIME_BLE_HID);
	TEST_ASSERT_EQ_INT(RUNTIME_GPREGRET2_BLE_HID, (int)mock_gpregret2);
}

static void test_switch_arms_wdt(void) {
	reset_mocks();
	runtime_select(NULL);
	runtime_request_switch(RUNTIME_BLE_HID);
	TEST_ASSERT(mock_wdt_arm_count >= 1, "wdt armed");
}

static void test_switch_calls_reset(void) {
	reset_mocks();
	runtime_select(NULL);
	runtime_request_switch(RUNTIME_BLE_HID);
	TEST_ASSERT(mock_reset_count >= 1, "reset called");
}

static void test_switch_ordering_clear_set_wdt_reset(void) {
	reset_mocks();
	runtime_select(NULL);
	runtime_request_switch(RUNTIME_BLE_HID);
	TEST_ASSERT(mock_seq_clear_at > 0, "clear was called");
	TEST_ASSERT(mock_seq_set_at > mock_seq_clear_at,
		"set happens after clear");
	TEST_ASSERT(mock_seq_wdt_at > mock_seq_set_at,
		"wdt arm happens after set");
	TEST_ASSERT(mock_seq_reset_at > mock_seq_wdt_at,
		"reset happens after wdt arm (USB drain / quiesce gate)");
}

static void test_double_switch_rejected(void) {
	reset_mocks();
	runtime_select(NULL);
	runtime_set_state(RUNTIME_STATE_SWITCHING);
	runtime_switch_result_t r = runtime_request_switch(RUNTIME_BLE_HID);
	TEST_ASSERT_EQ_INT(RUNTIME_SWITCH_ALREADY_SW, (int)r);
}

static void test_switch_invalid_mode_rejected(void) {
	reset_mocks();
	runtime_select(NULL);
	runtime_switch_result_t r = runtime_request_switch((runtime_mode_t)2);
	TEST_ASSERT_EQ_INT(RUNTIME_SWITCH_INVALID_MODE, (int)r);
}

static void test_switch_not_init(void) {
	runtime_init(NULL);
	runtime_switch_result_t r = runtime_request_switch(RUNTIME_BLE_HID);
	TEST_ASSERT_EQ_INT(RUNTIME_SWITCH_NOT_INIT, (int)r);
}

/* ---- Tests: CDC close policy ---- */

static void test_cdc_close_resets_raw_whad(void) {
	reset_mocks();
	runtime_select(NULL);
	TEST_ASSERT(runtime_cdc_close_should_reset(), "raw resets on CDC close");
}

static void test_cdc_close_no_reset_ble_hid(void) {
	reset_mocks();
	mock_gpregret2 = RUNTIME_GPREGRET2_BLE_HID;
	runtime_select(NULL);
	TEST_ASSERT(!runtime_cdc_close_should_reset(), "BLE no reset on CDC close");
}

/* ---- Tests: validation ---- */

static void test_mode_validation(void) {
	TEST_ASSERT(runtime_mode_is_valid(RUNTIME_RAW_WHAD), "raw valid");
	TEST_ASSERT(runtime_mode_is_valid(RUNTIME_BLE_HID), "ble valid");
	TEST_ASSERT(!runtime_mode_is_valid((runtime_mode_t)2), "2 invalid");
	TEST_ASSERT(!runtime_mode_is_valid((runtime_mode_t)99), "99 invalid");
}

static void test_gpregret2_validation(void) {
	TEST_ASSERT(runtime_gpregret2_is_valid(RUNTIME_GPREGRET2_RAW_WHAD), "0xC1 valid");
	TEST_ASSERT(runtime_gpregret2_is_valid(RUNTIME_GPREGRET2_BLE_HID), "0xC2 valid");
	TEST_ASSERT(!runtime_gpregret2_is_valid(0), "0 invalid");
	TEST_ASSERT(!runtime_gpregret2_is_valid(0xFF), "0xFF invalid");
	TEST_ASSERT(!runtime_gpregret2_is_valid(0x57), "0x57 invalid");
	TEST_ASSERT(!runtime_gpregret2_is_valid(0xB1), "0xB1 invalid");
}

static void test_gpregret2_to_mode_mapping(void) {
	runtime_mode_t mode = RUNTIME_RAW_WHAD;
	TEST_ASSERT(runtime_gpregret2_to_mode(RUNTIME_GPREGRET2_RAW_WHAD, &mode), "raw maps");
	TEST_ASSERT_EQ_INT(RUNTIME_RAW_WHAD, (int)mode);
	TEST_ASSERT(runtime_gpregret2_to_mode(RUNTIME_GPREGRET2_BLE_HID, &mode), "ble maps");
	TEST_ASSERT_EQ_INT(RUNTIME_BLE_HID, (int)mode);
	TEST_ASSERT(!runtime_gpregret2_to_mode(0, &mode), "0 fails");
}

static void test_mode_to_gpregret2_mapping(void) {
	TEST_ASSERT_EQ_INT(RUNTIME_GPREGRET2_RAW_WHAD,
	                   (int)runtime_mode_to_gpregret2(RUNTIME_RAW_WHAD));
	TEST_ASSERT_EQ_INT(RUNTIME_GPREGRET2_BLE_HID,
	                   (int)runtime_mode_to_gpregret2(RUNTIME_BLE_HID));
	TEST_ASSERT_EQ_INT(0, (int)runtime_mode_to_gpregret2((runtime_mode_t)2));
}

/* ---- Tests: capability tables ---- */

static int fake_caps_a;
static int fake_caps_b;

static void test_caps_register_and_get(void) {
	reset_mocks();
	const runtime_caps_t caps_a = { &fake_caps_a, NULL };
	const runtime_caps_t caps_b = { &fake_caps_b, NULL };
	runtime_register_caps(RUNTIME_RAW_WHAD, &caps_a);
	runtime_register_caps(RUNTIME_BLE_HID, &caps_b);

	const runtime_caps_t *ra = runtime_get_caps(RUNTIME_RAW_WHAD);
	const runtime_caps_t *rb = runtime_get_caps(RUNTIME_BLE_HID);
	TEST_ASSERT(ra != NULL, "raw caps not null");
	TEST_ASSERT(rb != NULL, "ble caps not null");
	TEST_ASSERT(ra->capabilities == &fake_caps_a, "raw caps pointer");
	TEST_ASSERT(rb->capabilities == &fake_caps_b, "ble caps pointer");
}

static void test_caps_not_registered_returns_null(void) {
	reset_mocks();
	TEST_ASSERT(runtime_get_caps(RUNTIME_RAW_WHAD) == NULL, "raw null when unregistered");
	TEST_ASSERT(runtime_get_caps(RUNTIME_BLE_HID) == NULL, "ble null when unregistered");
}

static void test_caps_invalid_mode_returns_null(void) {
	reset_mocks();
	TEST_ASSERT(runtime_get_caps((runtime_mode_t)99) == NULL, "invalid mode null");
}

/* ---- Test runner ---- */

int main(void) {
	test_framework_init();

	RUN_TEST(test_default_raw_when_no_oneshot_no_store);
	RUN_TEST(test_oneshot_raw_selects_raw);
	RUN_TEST(test_oneshot_ble_selects_ble);
	RUN_TEST(test_oneshot_cleared_after_read);
	RUN_TEST(test_oneshot_precedence_over_store);
	RUN_TEST(test_store_precedence_over_default);

	RUN_TEST(test_invalid_oneshot_cleared);
	RUN_TEST(test_invalid_oneshot_falls_through_to_store);
	RUN_TEST(test_zero_oneshot_not_cleared);
	RUN_TEST(test_store_unavailable_falls_to_default);
	RUN_TEST(test_store_invalid_falls_to_default);
	RUN_TEST(test_store_io_error_falls_to_default);

	RUN_TEST(test_switch_same_mode_noop);
	RUN_TEST(test_switch_raw_to_ble_sets_gpregret2);
	RUN_TEST(test_switch_ble_to_raw_sets_gpregret2);
	RUN_TEST(test_switch_clears_before_set);
	RUN_TEST(test_switch_arms_wdt);
	RUN_TEST(test_switch_calls_reset);
	RUN_TEST(test_switch_ordering_clear_set_wdt_reset);
	RUN_TEST(test_double_switch_rejected);
	RUN_TEST(test_switch_invalid_mode_rejected);
	RUN_TEST(test_switch_not_init);

	RUN_TEST(test_cdc_close_resets_raw_whad);
	RUN_TEST(test_cdc_close_no_reset_ble_hid);

	RUN_TEST(test_mode_validation);
	RUN_TEST(test_gpregret2_validation);
	RUN_TEST(test_gpregret2_to_mode_mapping);
	RUN_TEST(test_mode_to_gpregret2_mapping);

	RUN_TEST(test_caps_register_and_get);
	RUN_TEST(test_caps_not_registered_returns_null);
	RUN_TEST(test_caps_invalid_mode_returns_null);

	return test_framework_finish();
}
