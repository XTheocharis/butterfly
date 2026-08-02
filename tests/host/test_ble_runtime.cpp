/*
 * test_ble_runtime.cpp - Host tests for BLE-HID runtime eval layer.
 *
 * Tests all acceptance criteria from Todo 20:
 *   - Exact 0x3004/0x3008/0x300C SD validation and invalid-header short-circuit
 *   - One-peripheral/zero-central configuration
 *   - HVN TX queue >= 2
 *   - Baseline-and-returned RAM-origin checks
 *   - Exact device name
 *   - Interval-unit conversions (30ms→48, 250ms→400, 7.5ms→6, 15ms→12, 4s→400)
 *   - Fast→slow→directed/open advertising states
 *   - Active-0/idle-4 latency transitions and rejection backoff
 *   - Runtime IRQ assertions
 *   - Security parameters (LESC Just Works, 16-byte key, MITM false)
 */
#include "test_framework.h"
#include "ble_eval.h"
#include <string.h>

/* ---- Mock SD memory table -------------------------------------------- */

#define MOCK_MEM_SIZE 0x4000
static uint32_t g_mockMem[MOCK_MEM_SIZE / 4];

static void mock_mem_set(uint32_t addr, uint32_t val) {
	uint32_t idx = (addr - BLE_SD_INFO_STRUCT_ADDR) / 4;
	if (idx < MOCK_MEM_SIZE / 4) g_mockMem[idx] = val;
}

static uint32_t mock_read32(uint32_t addr) {
	uint32_t idx = (addr - BLE_SD_INFO_STRUCT_ADDR) / 4;
	if (idx < MOCK_MEM_SIZE / 4) return g_mockMem[idx];
	return 0xDEADBEEFu;
}

static void mock_mem_reset(void) {
	memset(g_mockMem, 0xFF, sizeof(g_mockMem));
}

static void mock_mem_set_valid_sd(void) {
	mock_mem_reset();
	mock_mem_set(BLE_SD_MAGIC_ADDR, BLE_SD_MAGIC_EXPECTED);
	mock_mem_set(BLE_SD_SIZE_ADDR, BLE_SD_SIZE_EXPECTED);
	mock_mem_set(BLE_SD_FWID_ADDR, BLE_SD_FWID_EXPECTED);
}

static const ble_sd_backend_t g_backend = { mock_read32 };

/* ---- SD validation tests --------------------------------------------- */

static void test_sd_valid_header(void) {
	mock_mem_set_valid_sd();
	ble_sd_validation_t r = ble_validate_sd(&g_backend);
	TEST_ASSERT(r == BLE_SD_VALID, "valid SD header should pass");
}

static void test_sd_magic_mismatch_short_circuits(void) {
	mock_mem_set_valid_sd();
	mock_mem_set(BLE_SD_MAGIC_ADDR, 0x00000000);
	ble_sd_validation_t r = ble_validate_sd(&g_backend);
	TEST_ASSERT(r == BLE_SD_MAGIC_MISMATCH, "bad magic must short-circuit");
}

static void test_sd_magic_at_wrong_address_0x1000(void) {
	/* Writing magic at 0x1000 must NOT pass validation — it must be
	 * at 0x3004 (Adafruit bootloader SD_INFO_STRUCT + 4). */
	mock_mem_reset();
	mock_mem_set(0x1000 + 4, BLE_SD_MAGIC_EXPECTED);
	ble_sd_validation_t r = ble_validate_sd(&g_backend);
	TEST_ASSERT(r == BLE_SD_MAGIC_MISMATCH, "magic at 0x1004 must not validate");
}

static void test_sd_size_mismatch(void) {
	mock_mem_set_valid_sd();
	mock_mem_set(BLE_SD_SIZE_ADDR, 0x10000);
	ble_sd_validation_t r = ble_validate_sd(&g_backend);
	TEST_ASSERT(r == BLE_SD_SIZE_MISMATCH, "wrong size must fail");
}

static void test_sd_fwid_mismatch(void) {
	mock_mem_set_valid_sd();
	mock_mem_set(BLE_SD_FWID_ADDR, 0x0100); /* S140 7.x FWID — wrong for 6.1.1 */
	ble_sd_validation_t r = ble_validate_sd(&g_backend);
	TEST_ASSERT(r == BLE_SD_FWID_MISMATCH, "S140 7.x FWID must fail");
}

static void test_sd_null_backend(void) {
	ble_sd_validation_t r = ble_validate_sd(nullptr);
	TEST_ASSERT(r == BLE_SD_MAGIC_MISMATCH, "null backend must fail safely");
}

static void test_sd_address_constants(void) {
	TEST_ASSERT_EQ_INT(0x3004, (int)BLE_SD_MAGIC_ADDR);
	TEST_ASSERT_EQ_INT(0x3008, (int)BLE_SD_SIZE_ADDR);
	TEST_ASSERT_EQ_INT(0x300C, (int)BLE_SD_FWID_ADDR);
}

/* ---- RAM check tests ------------------------------------------------- */

static void test_ram_ok(void) {
	ble_ram_check_t r = ble_check_ram_origin(BLE_RAM_ORIGIN_BASELINE,
	                                         BLE_RAM_ORIGIN_BASELINE);
	TEST_ASSERT(r == BLE_RAM_OK, "matching RAM origin should be OK");
}

static void test_ram_surplus(void) {
	/* SD requires more than linker provides. */
	ble_ram_check_t r = ble_check_ram_origin(BLE_RAM_ORIGIN_BASELINE,
	                                         BLE_RAM_ORIGIN_BASELINE + 0x1000);
	TEST_ASSERT(r == BLE_RAM_SURPLUS, "SD needs more → must rebuild");
}

static void test_ram_insufficient(void) {
	/* Linker provides less than baseline, and SD agrees (SD also fits there).
	 * This means the linker is wrong — below the known S140 6.1.1 baseline. */
	ble_ram_check_t r = ble_check_ram_origin(BLE_RAM_ORIGIN_BASELINE - 0x1000,
	                                         BLE_RAM_ORIGIN_BASELINE - 0x1000);
	TEST_ASSERT(r == BLE_RAM_INSUFFICIENT, "below baseline must abort");
}

static void test_ram_baseline_constant(void) {
	TEST_ASSERT_EQ_INT(0x20006000, (int)BLE_RAM_ORIGIN_BASELINE);
}

/* ---- Configuration constant tests ------------------------------------ */

static void test_link_counts(void) {
	TEST_ASSERT_EQ_INT(1, (int)BLE_PERIPHERAL_LINK_COUNT);
	TEST_ASSERT_EQ_INT(0, (int)BLE_CENTRAL_LINK_COUNT);
	TEST_ASSERT_EQ_INT(1, (int)BLE_TOTAL_LINK_COUNT);
}

static void test_hvn_queue_minimum(void) {
	TEST_ASSERT(BLE_HVN_TX_QUEUE_MIN >= 2, "HVN TX queue must be >= 2");
}

static void test_device_name_exact(void) {
	TEST_ASSERT(strcmp(BLE_DEVICE_NAME, "Butterfly CLUE Remote") == 0,
	            "device name must be exact");
	TEST_ASSERT_EQ_INT(21, (int)BLE_DEVICE_NAME_LEN);
}

static void test_lfrc_config(void) {
	TEST_ASSERT_EQ_INT(1, (int)BLE_LF_CLK_SRC);
}

/* ---- Unit conversion tests ------------------------------------------- */

static void test_gap_interval_7_5ms(void) {
	/* 7.5ms is the BLE minimum. ceil(7.5)=8 as integer ms input,
	 * which converts to 6 GAP units (7.5/1.25). */
	TEST_ASSERT_EQ_INT(6, (int)ble_gap_interval_from_ms(8));
}

static void test_gap_interval_15ms(void) {
	TEST_ASSERT_EQ_INT(12, (int)ble_gap_interval_from_ms(15));
}

static void test_adv_interval_30ms(void) {
	TEST_ASSERT_EQ_INT(48, (int)ble_adv_interval_from_ms(30));
}

static void test_adv_interval_250ms(void) {
	TEST_ASSERT_EQ_INT(400, (int)ble_adv_interval_from_ms(250));
}

static void test_sup_timeout_4s(void) {
	TEST_ASSERT_EQ_INT(400, (int)ble_sup_timeout_from_ms(4000));
}

static void test_adv_timeout_30s(void) {
	TEST_ASSERT_EQ_INT(3000, (int)BLE_ADV_TIMEOUT_FROM_MS(30000));
}

static void test_adv_timeout_unlimited(void) {
	TEST_ASSERT_EQ_INT(0, (int)BLE_ADV_SLOW_TIMEOUT_UNLIM);
}

/* ---- Advertising FSM tests ------------------------------------------- */

static void test_adv_start_idle_to_fast(void) {
	TEST_ASSERT(ble_adv_fsm(BLE_ADV_STATE_IDLE, BLE_ADV_EVT_START, false)
	            == BLE_ADV_STATE_FAST, "START from IDLE → FAST");
}

static void test_adv_fast_timeout_to_slow(void) {
	TEST_ASSERT(ble_adv_fsm(BLE_ADV_STATE_FAST, BLE_ADV_EVT_FAST_TIMEOUT, false)
	            == BLE_ADV_STATE_SLOW, "FAST_TIMEOUT → SLOW");
}

static void test_adv_connect_stops(void) {
	TEST_ASSERT(ble_adv_fsm(BLE_ADV_STATE_FAST, BLE_ADV_EVT_CONNECTED, false)
	            == BLE_ADV_STATE_STOPPED, "CONNECTED → STOPPED");
	TEST_ASSERT(ble_adv_fsm(BLE_ADV_STATE_SLOW, BLE_ADV_EVT_CONNECTED, false)
	            == BLE_ADV_STATE_STOPPED, "CONNECTED from SLOW → STOPPED");
}

static void test_adv_disconnect_no_bond_goes_fast(void) {
	TEST_ASSERT(ble_adv_fsm(BLE_ADV_STATE_STOPPED, BLE_ADV_EVT_DISCONNECTED, false)
	            == BLE_ADV_STATE_FAST, "DISCONNECTED without bond → FAST");
}

static void test_adv_disconnect_with_bond_goes_directed(void) {
	TEST_ASSERT(ble_adv_fsm(BLE_ADV_STATE_STOPPED, BLE_ADV_EVT_DISCONNECTED, true)
	            == BLE_ADV_STATE_DIRECTED, "DISCONNECTED with bond → DIRECTED");
}

static void test_adv_idle_activity_restarts_fast(void) {
	TEST_ASSERT(ble_adv_fsm(BLE_ADV_STATE_SLOW, BLE_ADV_EVT_IDLE_ACTIVITY, false)
	            == BLE_ADV_STATE_FAST, "IDLE_ACTIVITY from SLOW → FAST");
	TEST_ASSERT(ble_adv_fsm(BLE_ADV_STATE_DIRECTED, BLE_ADV_EVT_IDLE_ACTIVITY, false)
	            == BLE_ADV_STATE_FAST, "IDLE_ACTIVITY from DIRECTED → FAST");
}

static void test_adv_stop_from_running_states(void) {
	/* STOP from advertising states (FAST, SLOW, DIRECTED) must STOP.
	 * STOP from IDLE is a no-op (nothing running). */
	TEST_ASSERT(ble_adv_fsm(BLE_ADV_STATE_FAST, BLE_ADV_EVT_STOP, false)
	            == BLE_ADV_STATE_STOPPED, "STOP from FAST → STOPPED");
	TEST_ASSERT(ble_adv_fsm(BLE_ADV_STATE_SLOW, BLE_ADV_EVT_STOP, false)
	            == BLE_ADV_STATE_STOPPED, "STOP from SLOW → STOPPED");
	TEST_ASSERT(ble_adv_fsm(BLE_ADV_STATE_DIRECTED, BLE_ADV_EVT_STOP, false)
	            == BLE_ADV_STATE_STOPPED, "STOP from DIRECTED → STOPPED");
}

/* ---- Latency FSM tests ----------------------------------------------- */

static void test_latency_active_to_idle_on_5s(void) {
	TEST_ASSERT(ble_latency_fsm(BLE_LAT_STATE_ACTIVE, BLE_LAT_EVT_IDLE_5S)
	            == BLE_LAT_STATE_IDLE, "IDLE_5S from ACTIVE → IDLE");
}

static void test_latency_idle_to_active_on_activity(void) {
	TEST_ASSERT(ble_latency_fsm(BLE_LAT_STATE_IDLE, BLE_LAT_EVT_ACTIVITY)
	            == BLE_LAT_STATE_ACTIVE, "ACTIVITY from IDLE → ACTIVE");
}

static void test_latency_reject_enters_backoff(void) {
	TEST_ASSERT(ble_latency_fsm(BLE_LAT_STATE_ACTIVE, BLE_LAT_EVT_HOST_REJECT)
	            == BLE_LAT_STATE_BACKOFF, "REJECT from ACTIVE → BACKOFF");
	TEST_ASSERT(ble_latency_fsm(BLE_LAT_STATE_IDLE, BLE_LAT_EVT_HOST_REJECT)
	            == BLE_LAT_STATE_BACKOFF, "REJECT from IDLE → BACKOFF");
}

static void test_latency_activity_cancels_backoff(void) {
	TEST_ASSERT(ble_latency_fsm(BLE_LAT_STATE_BACKOFF, BLE_LAT_EVT_ACTIVITY)
	            == BLE_LAT_STATE_ACTIVE, "ACTIVITY cancels BACKOFF → ACTIVE");
}

static void test_latency_backoff_elapsed_retries_idle(void) {
	TEST_ASSERT(ble_latency_fsm(BLE_LAT_STATE_BACKOFF, BLE_LAT_EVT_BACKOFF_ELAPSED)
	            == BLE_LAT_STATE_IDLE, "BACKOFF_ELAPSED → IDLE (retry)");
}

static void test_latency_value_active_is_zero(void) {
	TEST_ASSERT_EQ_INT(0, (int)ble_latency_value(BLE_LAT_STATE_ACTIVE));
}

static void test_latency_value_idle_is_four(void) {
	TEST_ASSERT_EQ_INT(4, (int)ble_latency_value(BLE_LAT_STATE_IDLE));
	TEST_ASSERT_EQ_INT(4, (int)ble_latency_value(BLE_LAT_STATE_BACKOFF));
}

static void test_latency_backoff_duration(void) {
	TEST_ASSERT_EQ_INT(30000, (int)BLE_LATENCY_BACKOFF_MS);
}

/* ---- IRQ priority assertion tests ------------------------------------ */

static void test_irq_sd_reserved_is_prio_zero(void) {
	TEST_ASSERT(ble_irq_priority_is_sd_reserved(0), "prio 0 = SD reserved");
	TEST_ASSERT(!ble_irq_priority_is_sd_reserved(1), "prio 1 != SD reserved");
	TEST_ASSERT(!ble_irq_priority_is_sd_reserved(6), "prio 6 != SD reserved");
}

static void test_irq_app_safe_range(void) {
	TEST_ASSERT(ble_irq_priority_is_app_safe(2), "prio 2 = app safe");
	TEST_ASSERT(ble_irq_priority_is_app_safe(6), "prio 6 = app safe");
	TEST_ASSERT(!ble_irq_priority_is_app_safe(0), "prio 0 = NOT app safe (SD)");
	TEST_ASSERT(!ble_irq_priority_is_app_safe(1), "prio 1 = NOT app safe (raw timer)");
	TEST_ASSERT(!ble_irq_priority_is_app_safe(7), "prio 7 = NOT app safe");
}

static void test_ble_mode_no_raw_timers(void) {
	TEST_ASSERT(ble_ble_mode_raw_timer_check(false, false),
	            "BLE mode: neither timer started = safe");
	TEST_ASSERT(!ble_ble_mode_raw_timer_check(true, false),
	            "BLE mode: TIMER3 started = unsafe");
	TEST_ASSERT(!ble_ble_mode_raw_timer_check(false, true),
	            "BLE mode: TIMER4 started = unsafe");
	TEST_ASSERT(!ble_ble_mode_raw_timer_check(true, true),
	            "BLE mode: both timers started = unsafe");
}

static void test_irq_priority_constants(void) {
	TEST_ASSERT_EQ_INT(0, (int)BLE_IRQ_PRIO_SD_RESERVED);
	TEST_ASSERT_EQ_INT(1, (int)BLE_IRQ_PRIO_RAW_TIMER);
	TEST_ASSERT_EQ_INT(6, (int)BLE_IRQ_PRIO_APP_DEFAULT);
}

/* ---- Security parameter tests ---------------------------------------- */

static void test_sec_lesc_just_works(void) {
	TEST_ASSERT_EQ_INT(1, (int)BLE_SEC_BOND);
	TEST_ASSERT_EQ_INT(0, (int)BLE_SEC_MITM);
	TEST_ASSERT_EQ_INT(1, (int)BLE_SEC_LESC);
	TEST_ASSERT_EQ_INT(0, (int)BLE_SEC_KEYPRESS);
	TEST_ASSERT_EQ_INT(3, (int)BLE_SEC_IO_CAPS); /* NONE */
}

static void test_sec_key_size_16(void) {
	TEST_ASSERT_EQ_INT(16, (int)BLE_SEC_MIN_KEY_SIZE);
	TEST_ASSERT_EQ_INT(16, (int)BLE_SEC_MAX_KEY_SIZE);
}

/* ---- Advertising constant tests -------------------------------------- */

static void test_adv_interval_constants(void) {
	TEST_ASSERT_EQ_INT(30, (int)BLE_ADV_FAST_INTERVAL_MS);
	TEST_ASSERT_EQ_INT(30000, (int)BLE_ADV_FAST_TIMEOUT_MS);
	TEST_ASSERT_EQ_INT(250, (int)BLE_ADV_SLOW_INTERVAL_MS);
}

static void test_conn_param_constants(void) {
	TEST_ASSERT_EQ_INT(8, (int)BLE_CONN_MIN_INTERVAL_MS);
	TEST_ASSERT_EQ_INT(15, (int)BLE_CONN_MAX_INTERVAL_MS);
	TEST_ASSERT_EQ_INT(4000, (int)BLE_CONN_TIMEOUT_MS);
	TEST_ASSERT_EQ_INT(0, (int)BLE_LATENCY_ACTIVE);
	TEST_ASSERT_EQ_INT(4, (int)BLE_LATENCY_IDLE);
	TEST_ASSERT_EQ_INT(5000, (int)BLE_IDLE_THRESHOLD_MS);
}

/* ---- Main ------------------------------------------------------------ */

int main(void) {
	test_framework_init();

	/* SD validation */
	RUN_TEST(test_sd_address_constants);
	RUN_TEST(test_sd_valid_header);
	RUN_TEST(test_sd_magic_mismatch_short_circuits);
	RUN_TEST(test_sd_magic_at_wrong_address_0x1000);
	RUN_TEST(test_sd_size_mismatch);
	RUN_TEST(test_sd_fwid_mismatch);
	RUN_TEST(test_sd_null_backend);

	/* RAM check */
	RUN_TEST(test_ram_baseline_constant);
	RUN_TEST(test_ram_ok);
	RUN_TEST(test_ram_surplus);
	RUN_TEST(test_ram_insufficient);

	/* Configuration constants */
	RUN_TEST(test_link_counts);
	RUN_TEST(test_hvn_queue_minimum);
	RUN_TEST(test_device_name_exact);
	RUN_TEST(test_lfrc_config);

	/* Unit conversions */
	RUN_TEST(test_gap_interval_7_5ms);
	RUN_TEST(test_gap_interval_15ms);
	RUN_TEST(test_adv_interval_30ms);
	RUN_TEST(test_adv_interval_250ms);
	RUN_TEST(test_sup_timeout_4s);
	RUN_TEST(test_adv_timeout_30s);
	RUN_TEST(test_adv_timeout_unlimited);

	/* Advertising FSM */
	RUN_TEST(test_adv_start_idle_to_fast);
	RUN_TEST(test_adv_fast_timeout_to_slow);
	RUN_TEST(test_adv_connect_stops);
	RUN_TEST(test_adv_disconnect_no_bond_goes_fast);
	RUN_TEST(test_adv_disconnect_with_bond_goes_directed);
	RUN_TEST(test_adv_idle_activity_restarts_fast);
	RUN_TEST(test_adv_stop_from_running_states);

	/* Latency FSM */
	RUN_TEST(test_latency_active_to_idle_on_5s);
	RUN_TEST(test_latency_idle_to_active_on_activity);
	RUN_TEST(test_latency_reject_enters_backoff);
	RUN_TEST(test_latency_activity_cancels_backoff);
	RUN_TEST(test_latency_backoff_elapsed_retries_idle);
	RUN_TEST(test_latency_value_active_is_zero);
	RUN_TEST(test_latency_value_idle_is_four);
	RUN_TEST(test_latency_backoff_duration);

	/* IRQ assertions */
	RUN_TEST(test_irq_priority_constants);
	RUN_TEST(test_irq_sd_reserved_is_prio_zero);
	RUN_TEST(test_irq_app_safe_range);
	RUN_TEST(test_ble_mode_no_raw_timers);

	/* Security parameters */
	RUN_TEST(test_sec_lesc_just_works);
	RUN_TEST(test_sec_key_size_16);

	/* Advertising/conn-param constants */
	RUN_TEST(test_adv_interval_constants);
	RUN_TEST(test_conn_param_constants);

	return test_framework_finish();
}
