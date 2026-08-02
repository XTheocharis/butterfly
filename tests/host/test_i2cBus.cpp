/*
 * test_i2cBus.cpp - async TWIM1 bus manager host tests.
 *
 * Covers: serialization, callback exactly once, timeout/cancel races,
 * NACK/bus error → recovery, SDA release at clocks 0-9, STOP/reinit/
 * reprobe, all five device IDs, SHT31 CRC probe, conversion delay does
 * not hold the bus.
 *
 * Mock backend: test-controlled time, SDA level, device responses,
 * auto-completion. No SDK deps, no hardware.
 */
#include "test_framework.h"
#include "../../src/i2cBus.cpp"
#include <string.h>

/* ---- Test backend state ---------------------------------------------- */

#define MAX_TEST_DEVICES 8

struct DeviceResp {
	uint8_t addr;
	uint8_t data[8];
	uint8_t data_len;
	bool   nack;
};

struct BackendState {
	i2cbus_result_t pending_result;
	uint8_t  last_addr;
	size_t   last_write_len;
	size_t   last_read_len;

	/* Recovery */
	int sda_level;
	int scl_toggles;
	int stop_count;
	int reinit_count;
	int release_count;

	/* Time */
	uint64_t now;

	/* Devices */
	DeviceResp devices[MAX_TEST_DEVICES];
	int device_count;

	/* Counters */
	int start_xfer_count;
};

static BackendState g_st;

static int test_start_xfer(uint8_t addr, const uint8_t * /*wb*/, size_t wl,
                           uint8_t *rb, size_t rl) {
	g_st.start_xfer_count++;
	g_st.last_addr = addr;
	g_st.last_write_len = wl;
	g_st.last_read_len = rl;
	g_st.pending_result = I2CBUS_OK;

	for (int i = 0; i < g_st.device_count; i++) {
		if (g_st.devices[i].addr != addr) continue;
		if (g_st.devices[i].nack) {
			g_st.pending_result = I2CBUS_ERR_NACK;
		} else if (rl > 0 && rb) {
			size_t n = g_st.devices[i].data_len;
			if (n > rl) n = rl;
			memcpy(rb, g_st.devices[i].data, n);
		}
		return 0;
	}
	g_st.pending_result = I2CBUS_ERR_NACK;
	return 0;
}

static void test_release_sda(void) { g_st.release_count++; }
static void test_toggle_scl(void)  { g_st.scl_toggles++; }
static int  test_read_sda(void)    { return g_st.sda_level; }
static void test_gen_stop(void)    { g_st.stop_count++; }
static void test_reinit(void)      { g_st.reinit_count++; }
static uint64_t test_now(void)     { return g_st.now; }

static const i2cbus_backend_t BACKEND = {
	test_start_xfer, test_release_sda, test_toggle_scl,
	test_read_sda, test_gen_stop, test_reinit, test_now,
};

/* ---- Callback tracking ----------------------------------------------- */

static int g_cb_count;
static i2cbus_result_t g_cb_last_result;
static i2cbus_token_t g_cb_last_token;

static void track_cb(i2cbus_result_t r, void *user) {
	g_cb_count++;
	g_cb_last_result = r;
	g_cb_last_token = (i2cbus_token_t)(uintptr_t)user;
}

/* ---- Helpers --------------------------------------------------------- */

static void install_device(uint8_t addr, const uint8_t *data, uint8_t len) {
	for (int i = 0; i < g_st.device_count; i++) {
		if (g_st.devices[i].addr == addr) {
			memcpy(g_st.devices[i].data, data, len);
			g_st.devices[i].data_len = len;
			g_st.devices[i].nack = false;
			return;
		}
	}
	if (g_st.device_count >= MAX_TEST_DEVICES) return;
	g_st.devices[g_st.device_count].addr = addr;
	memcpy(g_st.devices[g_st.device_count].data, data, len);
	g_st.devices[g_st.device_count].data_len = len;
	g_st.devices[g_st.device_count].nack = false;
	g_st.device_count++;
}

static void install_all_five(void) {
	uint8_t lsm  = 0x69; install_device(0x6A, &lsm, 1);
	uint8_t lis  = 0x3D; install_device(0x1C, &lis, 1);
	uint8_t apds = 0xAB; install_device(0x39, &apds, 1);
	uint8_t bmp  = 0x58; install_device(0x77, &bmp, 1);

	uint8_t sht[6];
	sht[0] = 0xBE; sht[1] = 0xEF;
	sht[2] = i2cbus_sht31_crc(sht, 2);
	sht[3] = 0x7B; sht[4] = 0x58;
	sht[5] = i2cbus_sht31_crc(sht + 3, 2);
	install_device(0x44, sht, 6);
}

static void reset_state(void) {
	memset(&g_st, 0, sizeof g_st);
	g_st.sda_level = 1;
	g_cb_count = 0;
	g_cb_last_result = I2CBUS_OK;
	g_cb_last_token = 0;
	i2cbus_init(&BACKEND, 0);
}

/* Advance one transfer: tick + auto-complete. */
static void pump_ok(void) {
	i2cbus_tick();
	if (i2cbus_get_state() == I2CBUS_STATE_BUSY)
		i2cbus_report_xfer_complete(g_st.pending_result);
}

/* Run the full probe to completion. */
static void run_probe(void) {
	for (int guard = 0; guard < 40 && g_probeIdx != 0; guard++) {
		pump_ok();
		if (g_probeConvert) {
			g_st.now += 16000;
			i2cbus_tick();
			pump_ok();
		}
	}
}

/* ---- Compile/config tests -------------------------------------------- */

static void test_address_table_has_five_entries(void) {
	TEST_ASSERT_EQ_INT(I2CBUS_ONBOARD_COUNT, 5);
	TEST_ASSERT_EQ_INT(I2CBUS_ONBOARD_ADDRESSES[0], 0x6A);
	TEST_ASSERT_EQ_INT(I2CBUS_ONBOARD_ADDRESSES[1], 0x1C);
	TEST_ASSERT_EQ_INT(I2CBUS_ONBOARD_ADDRESSES[2], 0x39);
	TEST_ASSERT_EQ_INT(I2CBUS_ONBOARD_ADDRESSES[3], 0x44);
	TEST_ASSERT_EQ_INT(I2CBUS_ONBOARD_ADDRESSES[4], 0x77);
}

static void test_frequency_is_400khz(void) {
	TEST_ASSERT_EQ_INT(I2CBUS_FREQUENCY_HZ, 400000);
}

static void test_irq_priority_is_6(void) {
	TEST_ASSERT_EQ_INT(I2CBUS_IRQ_PRIORITY, 6);
}

/* ---- Serialization --------------------------------------------------- */

static void test_two_transfers_serialize(void) {
	reset_state();
	install_all_five();

	uint8_t w1 = 0x0F, r1, r2;
	i2cbus_transfer_t a = {0x6A, &w1, 1, &r1, 1, track_cb, (void*)1};
	i2cbus_transfer_t b = {0x1C, &w1, 1, &r2, 1, track_cb, (void*)2};
	i2cbus_token_t ta = i2cbus_enqueue(&a);
	i2cbus_token_t tb = i2cbus_enqueue(&b);
	TEST_ASSERT(ta != I2CBUS_TOKEN_INVALID, "enqueue a");
	TEST_ASSERT(tb != I2CBUS_TOKEN_INVALID, "enqueue b");

	g_cb_count = 0;
	pump_ok();
	TEST_ASSERT_EQ_INT(1, g_cb_count);
	TEST_ASSERT_EQ_INT((int)I2CBUS_OK, (int)g_cb_last_result);
	TEST_ASSERT_EQ_INT(0x6A, g_st.last_addr);

	pump_ok();
	TEST_ASSERT_EQ_INT(2, g_cb_count);
	TEST_ASSERT_EQ_INT(0x1C, g_st.last_addr);
}

static void test_fifo_order_preserved(void) {
	reset_state();
	install_all_five();

	uint8_t reg = 0x0F, buf[3];
	i2cbus_transfer_t xfers[3] = {
		{0x6A, &reg, 1, &buf[0], 1, track_cb, (void*)10},
		{0x1C, &reg, 1, &buf[1], 1, track_cb, (void*)11},
		{0x39, &reg, 1, &buf[2], 1, track_cb, (void*)12},
	};
	for (int i = 0; i < 3; i++)
		TEST_ASSERT(i2cbus_enqueue(&xfers[i]) != I2CBUS_TOKEN_INVALID, "enqueue");

	int order[3];
	g_cb_count = 0;
	for (int i = 0; i < 3; i++) {
		pump_ok();
		order[i] = (int)g_cb_last_token;
	}
	TEST_ASSERT_EQ_INT(10, order[0]);
	TEST_ASSERT_EQ_INT(11, order[1]);
	TEST_ASSERT_EQ_INT(12, order[2]);
}

/* ---- Callback exactly once ------------------------------------------- */

static void test_callback_fires_exactly_once(void) {
	reset_state();
	install_all_five();

	uint8_t reg = 0x0F, val;
	i2cbus_transfer_t xfer = {0x6A, &reg, 1, &val, 1, track_cb, (void*)42};
	i2cbus_enqueue(&xfer);

	g_cb_count = 0;
	pump_ok();
	TEST_ASSERT_EQ_INT(1, g_cb_count);

	/* Report a second time — must NOT fire callback again. */
	i2cbus_report_xfer_complete(I2CBUS_OK);
	TEST_ASSERT_EQ_INT(1, g_cb_count);
}

/* ---- Timeout --------------------------------------------------------- */

static void test_timeout_triggers_recovery(void) {
	reset_state();
	g_st.sda_level = 1;

	uint8_t reg = 0x0F, val;
	i2cbus_transfer_t xfer = {0x6A, &reg, 1, &val, 1, track_cb, (void*)1};
	i2cbus_enqueue(&xfer);

	g_cb_count = 0;
	i2cbus_tick(); /* start transfer */
	TEST_ASSERT_EQ_INT(I2CBUS_STATE_BUSY, (int)i2cbus_get_state());

	/* Advance past 10ms without completion. */
	g_st.now += 10001;
	i2cbus_tick();
	TEST_ASSERT_EQ_INT(1, g_cb_count);
	TEST_ASSERT_EQ_INT((int)I2CBUS_ERR_TIMEOUT, (int)g_cb_last_result);
	TEST_ASSERT_EQ_INT(I2CBUS_STATE_RECOVERY, (int)i2cbus_get_state());
}

/* ---- Cancel ---------------------------------------------------------- */

static void test_cancel_queued_transfer(void) {
	reset_state();

	uint8_t reg = 0x0F, v1, v2;
	i2cbus_transfer_t a = {0x6A, &reg, 1, &v1, 1, track_cb, (void*)1};
	i2cbus_transfer_t b = {0x1C, &reg, 1, &v2, 1, track_cb, (void*)2};
	(void)i2cbus_enqueue(&a);
	i2cbus_token_t tb = i2cbus_enqueue(&b);

	/* Cancel B while A is executing. */
	i2cbus_tick(); /* starts A */
	TEST_ASSERT_EQ_INT(0, i2cbus_cancel(tb));

	install_all_five();
	i2cbus_report_xfer_complete(I2CBUS_OK);
	TEST_ASSERT_EQ_INT(1, g_cb_count);

	/* Next tick dequeues B, which is cancelled → CANCELLED completion. */
	g_cb_count = 0;
	i2cbus_tick();
	TEST_ASSERT_EQ_INT(1, g_cb_count);
	TEST_ASSERT_EQ_INT((int)I2CBUS_ERR_CANCELLED, (int)g_cb_last_result);
}

static void test_cancel_active_transfer(void) {
	reset_state();

	uint8_t reg = 0x0F, val;
	i2cbus_transfer_t xfer = {0x6A, &reg, 1, &val, 1, track_cb, (void*)1};
	i2cbus_token_t tok = i2cbus_enqueue(&xfer);

	g_cb_count = 0;
	i2cbus_tick(); /* start transfer */
	TEST_ASSERT_EQ_INT(I2CBUS_STATE_BUSY, (int)i2cbus_get_state());

	TEST_ASSERT_EQ_INT(0, i2cbus_cancel(tok));

	/* Next tick sees cancelled flag → CANCELLED, no recovery. */
	i2cbus_tick();
	TEST_ASSERT_EQ_INT(1, g_cb_count);
	TEST_ASSERT_EQ_INT((int)I2CBUS_ERR_CANCELLED, (int)g_cb_last_result);
	TEST_ASSERT_EQ_INT(I2CBUS_STATE_IDLE, (int)i2cbus_get_state());
}

/* ---- NACK triggers recovery ------------------------------------------ */

static void test_bus_error_triggers_recovery(void) {
	reset_state();
	g_st.sda_level = 1;

	uint8_t reg = 0x0F, val;
	i2cbus_transfer_t xfer = {0x6A, &reg, 1, &val, 1, track_cb, (void*)1};
	i2cbus_enqueue(&xfer);

	g_cb_count = 0;
	i2cbus_tick(); /* start */
	TEST_ASSERT_EQ_INT(I2CBUS_STATE_BUSY, (int)i2cbus_get_state());

	i2cbus_report_xfer_complete(I2CBUS_ERR_BUS);
	TEST_ASSERT_EQ_INT(1, g_cb_count);
	TEST_ASSERT_EQ_INT((int)I2CBUS_ERR_BUS, (int)g_cb_last_result);
	TEST_ASSERT_EQ_INT(I2CBUS_STATE_RECOVERY, (int)i2cbus_get_state());
}

static void test_nack_completes_without_recovery(void) {
	reset_state();

	uint8_t reg = 0x0F, val;
	i2cbus_transfer_t xfer = {0x6A, &reg, 1, &val, 1, track_cb, (void*)1};
	i2cbus_enqueue(&xfer);

	g_cb_count = 0;
	i2cbus_tick();
	i2cbus_report_xfer_complete(I2CBUS_ERR_NACK);
	TEST_ASSERT_EQ_INT(1, g_cb_count);
	TEST_ASSERT_EQ_INT((int)I2CBUS_ERR_NACK, (int)g_cb_last_result);
	TEST_ASSERT_EQ_INT(I2CBUS_STATE_IDLE, (int)i2cbus_get_state());
}

/* ---- Recovery: SDA release at clocks 0-9 ----------------------------- */

static void test_recovery_sda_released_immediately(void) {
	reset_state();
	g_st.sda_level = 1;

	uint8_t reg = 0x0F, val;
	i2cbus_transfer_t xfer = {0x6A, &reg, 1, &val, 1, track_cb, (void*)1};
	i2cbus_enqueue(&xfer);
	i2cbus_tick();
	i2cbus_report_xfer_complete(I2CBUS_ERR_BUS);

	g_st.release_count = 0;
	g_st.scl_toggles = 0;
	/* Phase 0: release SDA */
	i2cbus_tick();
	TEST_ASSERT_EQ_INT(1, g_st.release_count);

	/* Phase 1: clock SCL. SDA is high (released) → STOP after first clock. */
	i2cbus_tick();
	TEST_ASSERT_EQ_INT(1, g_st.scl_toggles);
	TEST_ASSERT_EQ_INT(2, (int)g_recPhase); /* jumped to STOP */
}

static void test_recovery_nine_clocks_when_sda_stuck(void) {
	reset_state();
	g_st.sda_level = 0; /* SDA stuck low */

	uint8_t reg = 0x0F, val;
	i2cbus_transfer_t xfer = {0x6A, &reg, 1, &val, 1, track_cb, (void*)1};
	i2cbus_enqueue(&xfer);
	i2cbus_tick();
	i2cbus_report_xfer_complete(I2CBUS_ERR_BUS);

	g_st.scl_toggles = 0;
	/* Phase 0: release SDA */
	i2cbus_tick();
	/* Phase 1: all 9 clocks since SDA stays low */
	for (int i = 0; i < 9; i++) {
		i2cbus_tick();
		TEST_ASSERT_EQ_INT(i + 1, g_st.scl_toggles);
	}
	TEST_ASSERT_EQ_INT(2, (int)g_recPhase); /* moved to STOP */
}

static void test_recovery_sda_releases_at_clock_5(void) {
	reset_state();

	uint8_t reg = 0x0F, val;
	i2cbus_transfer_t xfer = {0x6A, &reg, 1, &val, 1, track_cb, (void*)1};
	i2cbus_enqueue(&xfer);
	i2cbus_tick();
	i2cbus_report_xfer_complete(I2CBUS_ERR_BUS);

	/* Phase 0: release SDA */
	g_st.sda_level = 0; /* stuck initially */
	i2cbus_tick();

	/* Clock 1-4: SDA low */
	g_st.scl_toggles = 0;
	for (int i = 0; i < 4; i++) i2cbus_tick();
	TEST_ASSERT_EQ_INT(4, g_st.scl_toggles);
	TEST_ASSERT_EQ_INT(1, (int)g_recPhase); /* still clocking */

	/* Clock 5: SDA goes high */
	g_st.sda_level = 1;
	i2cbus_tick();
	TEST_ASSERT_EQ_INT(5, g_st.scl_toggles);
	TEST_ASSERT_EQ_INT(2, (int)g_recPhase); /* jumped to STOP */
}

/* ---- Recovery: STOP + reinit + reprobe ------------------------------- */

static void test_recovery_full_sequence(void) {
	reset_state();
	g_st.sda_level = 1;
	install_all_five();

	uint8_t reg = 0x0F, val;
	i2cbus_transfer_t xfer = {0x6A, &reg, 1, &val, 1, track_cb, (void*)1};
	i2cbus_enqueue(&xfer);
	i2cbus_tick();
	i2cbus_report_xfer_complete(I2CBUS_ERR_BUS);

	g_st.stop_count = 0;
	g_st.reinit_count = 0;

	/* Phase 0: release */
	i2cbus_tick();
	/* Phase 1: SDA high → immediate STOP */
	i2cbus_tick();
	TEST_ASSERT_EQ_INT(2, (int)g_recPhase);
	/* Phase 2: STOP */
	i2cbus_tick();
	TEST_ASSERT_EQ_INT(1, g_st.stop_count);
	TEST_ASSERT_EQ_INT(3, (int)g_recPhase);
	/* Phase 3: reinit + probe_all */
	i2cbus_tick();
	TEST_ASSERT_EQ_INT(1, g_st.reinit_count);
	TEST_ASSERT_EQ_INT(I2CBUS_STATE_IDLE, (int)i2cbus_get_state());

	/* After recovery, probe runs and re-identifies devices */
	run_probe();
	TEST_ASSERT(i2cbus_is_present(0x6A), "LSM present after reprobe");
	TEST_ASSERT(i2cbus_is_present(0x77), "BMP present after reprobe");
}

/* ---- Device identification ------------------------------------------- */

static void test_probe_identifies_lsm6ds33(void) {
	reset_state();
	uint8_t id = 0x69;
	install_device(0x6A, &id, 1);
	i2cbus_probe_all();
	run_probe();
	TEST_ASSERT(i2cbus_is_present(0x6A), "LSM present");
	TEST_ASSERT_EQ_INT((int)I2CBUS_DEV_LSM6DS33, (int)i2cbus_get_device_id(0x6A));
}

static void test_probe_identifies_lsm6ds3trc_variant(void) {
	reset_state();
	uint8_t id = 0x6A; /* TR-C variant */
	install_device(0x6A, &id, 1);
	i2cbus_probe_all();
	run_probe();
	TEST_ASSERT(i2cbus_is_present(0x6A), "LSM present");
	TEST_ASSERT_EQ_INT((int)I2CBUS_DEV_LSM6DS3TRC, (int)i2cbus_get_device_id(0x6A));
}

static void test_probe_identifies_lis3mdl(void) {
	reset_state();
	uint8_t id = 0x3D;
	install_device(0x1C, &id, 1);
	i2cbus_probe_all();
	run_probe();
	TEST_ASSERT(i2cbus_is_present(0x1C), "LIS present");
	TEST_ASSERT_EQ_INT((int)I2CBUS_DEV_LIS3MDL, (int)i2cbus_get_device_id(0x1C));
}

static void test_probe_identifies_apds9960(void) {
	reset_state();
	uint8_t id = 0xAB;
	install_device(0x39, &id, 1);
	i2cbus_probe_all();
	run_probe();
	TEST_ASSERT(i2cbus_is_present(0x39), "APDS present");
	TEST_ASSERT_EQ_INT((int)I2CBUS_DEV_APDS9960, (int)i2cbus_get_device_id(0x39));
}

static void test_probe_identifies_bmp280(void) {
	reset_state();
	uint8_t id = 0x58;
	install_device(0x77, &id, 1);
	i2cbus_probe_all();
	run_probe();
	TEST_ASSERT(i2cbus_is_present(0x77), "BMP present");
	TEST_ASSERT_EQ_INT((int)I2CBUS_DEV_BMP280, (int)i2cbus_get_device_id(0x77));
}

static void test_probe_identifies_sht31d(void) {
	reset_state();
	uint8_t sht[6] = {0xBE, 0xEF, 0, 0x7B, 0x58, 0};
	sht[2] = i2cbus_sht31_crc(sht, 2);
	sht[5] = i2cbus_sht31_crc(sht + 3, 2);
	install_device(0x44, sht, 6);
	i2cbus_probe_all();
	run_probe();
	TEST_ASSERT(i2cbus_is_present(0x44), "SHT present");
	TEST_ASSERT_EQ_INT((int)I2CBUS_DEV_SHT31D, (int)i2cbus_get_device_id(0x44));
}

static void test_probe_all_five_present(void) {
	reset_state();
	install_all_five();
	i2cbus_probe_all();
	run_probe();
	TEST_ASSERT_EQ_INT((int)I2CBUS_PRESENCE_ALL, (int)i2cbus_get_presence());
}

/* ---- SHT31 CRC ------------------------------------------------------- */

static void test_sht31_crc_known_vector(void) {
	/* Known vector: CRC-8(0xBE, 0xEF) = 0x92 with poly 0x31, init 0xFF */
	uint8_t data[2] = {0xBE, 0xEF};
	TEST_ASSERT_EQ_INT(0x92, i2cbus_sht31_crc(data, 2));
}

static void test_sht31_crc_rejects_bad_data(void) {
	reset_state();
	/* Bad CRC → SHT31 NOT marked present */
	uint8_t bad[6] = {0xBE, 0xEF, 0xFF, 0x7B, 0x58, 0xFF}; /* wrong CRCs */
	install_device(0x44, bad, 6);
	i2cbus_probe_all();
	run_probe();
	TEST_ASSERT(!i2cbus_is_present(0x44), "SHT must be absent with bad CRC");
}

/* ---- Missing device degrades gracefully ------------------------------ */

static void test_missing_device_not_present(void) {
	reset_state();
	uint8_t lsm = 0x69;
	install_device(0x6A, &lsm, 1);
	/* Only LSM installed — all others missing */
	i2cbus_probe_all();
	run_probe();
	TEST_ASSERT(i2cbus_is_present(0x6A), "LSM present");
	TEST_ASSERT(!i2cbus_is_present(0x1C), "LIS absent");
	TEST_ASSERT(!i2cbus_is_present(0x39), "APDS absent");
	TEST_ASSERT(!i2cbus_is_present(0x44), "SHT absent");
	TEST_ASSERT(!i2cbus_is_present(0x77), "BMP absent");
	TEST_ASSERT_EQ_INT(I2CBUS_PRESENCE_LSM, (int)i2cbus_get_presence());
}

/* ---- No sensor delay holds manager ----------------------------------- */

static void test_conversion_does_not_hold_bus(void) {
	reset_state();
	install_all_five();

	i2cbus_probe_all();

	/* Advance to SHT probe step: pump LSM, LIS, APDS, BMP first */
	for (int i = 0; i < 4; i++) pump_ok();

	/* Now SHT write should start. Pump it. */
	TEST_ASSERT_EQ_INT(I2CBUS_ADDR_SHT31D, g_st.last_addr);
	pump_ok();

	/* Manager is now in CONVERT state (bus free). Enqueue a user transfer. */
	uint8_t reg = 0x0F, val;
	i2cbus_transfer_t user_xfer = {0x6A, &reg, 1, &val, 1, track_cb, (void*)99};
	i2cbus_enqueue(&user_xfer);

	g_cb_count = 0;
	/* While converting, the bus serves the user transfer */
	pump_ok();
	TEST_ASSERT_EQ_INT(1, g_cb_count);
	TEST_ASSERT_EQ_INT((int)I2CBUS_OK, (int)g_cb_last_result);

	/* Now advance time past conversion and let SHT read complete */
	g_st.now += 16000;
	for (int guard = 0; guard < 4 && g_probeIdx != 0; guard++) pump_ok();
	TEST_ASSERT(i2cbus_is_present(0x44), "SHT probed after conversion");
}

/* ---- Queue full ------------------------------------------------------ */

static void test_queue_full_rejected(void) {
	reset_state();
	uint8_t reg = 0x0F, buf[QUEUE_SIZE];
	i2cbus_transfer_t xfers[QUEUE_SIZE];
	for (int i = 0; i < QUEUE_SIZE; i++) {
		xfers[i] = {0x6A, &reg, 1, &buf[i], 1, track_cb, (void*)(intptr_t)(100 + i)};
		TEST_ASSERT(i2cbus_enqueue(&xfers[i]) != I2CBUS_TOKEN_INVALID, "fill queue");
	}
	/* Queue full → 5th enqueue rejected */
	i2cbus_transfer_t extra = {0x6A, &reg, 1, &buf[0], 1, track_cb, (void*)200};
	TEST_ASSERT(i2cbus_enqueue(&extra) == I2CBUS_TOKEN_INVALID, "reject when full");
}

/* ---- main ------------------------------------------------------------ */

int main(void) {
	test_framework_init();
	RUN_TEST(test_address_table_has_five_entries);
	RUN_TEST(test_frequency_is_400khz);
	RUN_TEST(test_irq_priority_is_6);
	RUN_TEST(test_two_transfers_serialize);
	RUN_TEST(test_fifo_order_preserved);
	RUN_TEST(test_callback_fires_exactly_once);
	RUN_TEST(test_timeout_triggers_recovery);
	RUN_TEST(test_cancel_queued_transfer);
	RUN_TEST(test_cancel_active_transfer);
	RUN_TEST(test_bus_error_triggers_recovery);
	RUN_TEST(test_nack_completes_without_recovery);
	RUN_TEST(test_recovery_sda_released_immediately);
	RUN_TEST(test_recovery_nine_clocks_when_sda_stuck);
	RUN_TEST(test_recovery_sda_releases_at_clock_5);
	RUN_TEST(test_recovery_full_sequence);
	RUN_TEST(test_probe_identifies_lsm6ds33);
	RUN_TEST(test_probe_identifies_lsm6ds3trc_variant);
	RUN_TEST(test_probe_identifies_lis3mdl);
	RUN_TEST(test_probe_identifies_apds9960);
	RUN_TEST(test_probe_identifies_bmp280);
	RUN_TEST(test_probe_identifies_sht31d);
	RUN_TEST(test_probe_all_five_present);
	RUN_TEST(test_sht31_crc_known_vector);
	RUN_TEST(test_sht31_crc_rejects_bad_data);
	RUN_TEST(test_missing_device_not_present);
	RUN_TEST(test_conversion_does_not_hold_bus);
	RUN_TEST(test_queue_full_rejected);
	return test_framework_finish();
}
