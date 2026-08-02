/*
 * test_expert_io.cpp - I2C/SPI expert I/O and force lifecycle host tests.
 *
 * Covers: io_eval (I2C address classification, buffer bounds, SPI mode/
 * frequency validation, EasyDMA RAM check, CS conflict detection, partial
 * transfer computation) and expert_force (force/quiesce/restore lifecycle,
 * non-cancellable rejection, restore fault tracking).
 *
 * No hardware required. All logic is in io_eval.c and expert_force.c.
 */
#include "test_framework.h"

#include "../../src/expert/io_eval.h"
#include "../../src/expert/expert_force.h"

#include <string.h>

/* ================================================================
 * Buffer bounds validation
 * ================================================================ */

static void test_buffer_size_zero_valid(void)
{
	TEST_ASSERT(io_eval_buffer_size_valid(0), "zero is valid");
}

static void test_buffer_size_max_valid(void)
{
	TEST_ASSERT(io_eval_buffer_size_valid(256), "256 is valid");
}

static void test_buffer_size_oversize_rejected(void)
{
	TEST_ASSERT(!io_eval_buffer_size_valid(257), "257 rejected");
	TEST_ASSERT(!io_eval_buffer_size_valid(512), "512 rejected (proto allows 512 but expert rejects)");
	TEST_ASSERT(!io_eval_buffer_size_valid(10000), "10000 rejected");
}

static void test_buffer_max_constant(void)
{
	TEST_ASSERT_EQ_INT(256, IO_EVAL_MAX_BUFFER);
}

/* ================================================================
 * I2C onboard address classification
 * ================================================================ */

static void test_i2c_onboard_count(void)
{
	TEST_ASSERT_EQ_INT(5, IO_EVAL_ONBOARD_ADDR_COUNT);
}

static void test_i2c_onboard_all_five(void)
{
	TEST_ASSERT(io_eval_is_onboard_i2c(0x6A), "LSM6DS33");
	TEST_ASSERT(io_eval_is_onboard_i2c(0x1C), "LIS3MDL");
	TEST_ASSERT(io_eval_is_onboard_i2c(0x39), "APDS9960");
	TEST_ASSERT(io_eval_is_onboard_i2c(0x44), "SHT31D");
	TEST_ASSERT(io_eval_is_onboard_i2c(0x77), "BMP280");
}

static void test_i2c_external_not_onboard(void)
{
	TEST_ASSERT(!io_eval_is_onboard_i2c(0x50), "EEPROM 0x50 external");
	TEST_ASSERT(!io_eval_is_onboard_i2c(0x68), "MPU6050 0x68 external");
	TEST_ASSERT(!io_eval_is_onboard_i2c(0x76), "BME280 0x76 external (NOT BMP280 0x77)");
}

static void test_i2c_address_valid_range(void)
{
	TEST_ASSERT(io_eval_i2c_address_valid(0x08), "0x08 valid");
	TEST_ASSERT(io_eval_i2c_address_valid(0x77), "0x77 valid");
	TEST_ASSERT(io_eval_i2c_address_valid(0x50), "0x50 valid");
}

static void test_i2c_address_reserved_rejected(void)
{
	TEST_ASSERT(!io_eval_i2c_address_valid(0x00), "0x00 reserved");
	TEST_ASSERT(!io_eval_i2c_address_valid(0x07), "0x07 reserved");
	TEST_ASSERT(!io_eval_i2c_address_valid(0x78), "0x78 reserved");
	TEST_ASSERT(!io_eval_i2c_address_valid(0x7F), "0x7F reserved");
}

/* ================================================================
 * SPI mode validation
 * ================================================================ */

static void test_spi_mode_all_valid(void)
{
	for (int32_t m = 1; m <= 4; m++) {
		io_spi_mode_t out = IO_EVAL_SPI_MODE_0;
		TEST_ASSERT(io_eval_spi_mode_valid(m, &out), "proto mode valid");
		TEST_ASSERT_EQ_INT((int)(m - 1), (int)out);
	}
}

static void test_spi_mode_unknown_rejected(void)
{
	io_spi_mode_t out;
	TEST_ASSERT(!io_eval_spi_mode_valid(0, &out), "UNKNOWN=0 rejected");
	TEST_ASSERT(!io_eval_spi_mode_valid(5, &out), "5 out of range");
	TEST_ASSERT(!io_eval_spi_mode_valid(-1, &out), "-1 rejected");
	TEST_ASSERT(!io_eval_spi_mode_valid(99, &out), "99 rejected");
}

static void test_spi_mode_specific_mapping(void)
{
	io_spi_mode_t m;
	io_eval_spi_mode_valid(1, &m);
	TEST_ASSERT_EQ_INT((int)IO_EVAL_SPI_MODE_0, (int)m);

	io_eval_spi_mode_valid(2, &m);
	TEST_ASSERT_EQ_INT((int)IO_EVAL_SPI_MODE_1, (int)m);

	io_eval_spi_mode_valid(3, &m);
	TEST_ASSERT_EQ_INT((int)IO_EVAL_SPI_MODE_2, (int)m);

	io_eval_spi_mode_valid(4, &m);
	TEST_ASSERT_EQ_INT((int)IO_EVAL_SPI_MODE_3, (int)m);
}

/* ================================================================
 * SPI frequency validation
 * ================================================================ */

static void test_spi_freq_count(void)
{
	TEST_ASSERT_EQ_INT(7, IO_EVAL_SPI_FREQ_COUNT);
}

static void test_spi_freq_all_discrete(void)
{
	uint32_t actual = 0;
	TEST_ASSERT(io_eval_spi_freq_clamp(125000, &actual), "125k");
	TEST_ASSERT_EQ_INT(125000, actual);

	TEST_ASSERT(io_eval_spi_freq_clamp(250000, &actual), "250k");
	TEST_ASSERT_EQ_INT(250000, actual);

	TEST_ASSERT(io_eval_spi_freq_clamp(500000, &actual), "500k");
	TEST_ASSERT_EQ_INT(500000, actual);

	TEST_ASSERT(io_eval_spi_freq_clamp(1000000, &actual), "1M");
	TEST_ASSERT_EQ_INT(1000000, actual);

	TEST_ASSERT(io_eval_spi_freq_clamp(2000000, &actual), "2M");
	TEST_ASSERT_EQ_INT(2000000, actual);

	TEST_ASSERT(io_eval_spi_freq_clamp(4000000, &actual), "4M");
	TEST_ASSERT_EQ_INT(4000000, actual);

	TEST_ASSERT(io_eval_spi_freq_clamp(8000000, &actual), "8M");
	TEST_ASSERT_EQ_INT(8000000, actual);
}

static void test_spi_freq_clamp_down(void)
{
	uint32_t actual = 0;

	TEST_ASSERT(io_eval_spi_freq_clamp(300000, &actual), "300k -> 250k");
	TEST_ASSERT_EQ_INT(250000, actual);

	TEST_ASSERT(io_eval_spi_freq_clamp(750000, &actual), "750k -> 500k");
	TEST_ASSERT_EQ_INT(500000, actual);

	TEST_ASSERT(io_eval_spi_freq_clamp(1500000, &actual), "1.5M -> 1M");
	TEST_ASSERT_EQ_INT(1000000, actual);

	TEST_ASSERT(io_eval_spi_freq_clamp(5000000, &actual), "5M -> 4M");
	TEST_ASSERT_EQ_INT(4000000, actual);
}

static void test_spi_freq_clamp_below_min(void)
{
	uint32_t actual = 0;
	TEST_ASSERT(io_eval_spi_freq_clamp(50000, &actual), "50k -> 125k");
	TEST_ASSERT_EQ_INT(125000, actual);

	TEST_ASSERT(io_eval_spi_freq_clamp(1, &actual), "1Hz -> 125k");
	TEST_ASSERT_EQ_INT(125000, actual);
}

static void test_spi_freq_reject_above_max(void)
{
	uint32_t actual = 0;
	TEST_ASSERT(!io_eval_spi_freq_clamp(8000001, &actual), "8M+1 rejected");
	TEST_ASSERT(!io_eval_spi_freq_clamp(16000000, &actual), "16M rejected");
	TEST_ASSERT(!io_eval_spi_freq_clamp(32000000, &actual), "32M rejected");
}

static void test_spi_freq_constants(void)
{
	TEST_ASSERT_EQ_INT(125000, IO_EVAL_SPI_FREQ_MIN);
	TEST_ASSERT_EQ_INT(8000000, IO_EVAL_SPI_FREQ_MAX);
}

/* ================================================================
 * EasyDMA RAM pointer validation
 * ================================================================ */

static void test_ram_pointer_in_range(void)
{
	TEST_ASSERT(io_eval_is_ram_pointer((void *)0x20000000), "RAM base");
	TEST_ASSERT(io_eval_is_ram_pointer((void *)0x20002260), "app RAM start");
	TEST_ASSERT(io_eval_is_ram_pointer((void *)0x2003FFFF), "last RAM byte");
}

static void test_ram_pointer_flash_rejected(void)
{
	TEST_ASSERT(!io_eval_is_ram_pointer((void *)0x00000000), "flash base");
	TEST_ASSERT(!io_eval_is_ram_pointer((void *)0x00026000), "app flash");
	TEST_ASSERT(!io_eval_is_ram_pointer((void *)0x10001000), "FICR");
	TEST_ASSERT(!io_eval_is_ram_pointer((void *)0x00E00000), "FICR region");
}

static void test_ram_pointer_out_of_range(void)
{
	TEST_ASSERT(!io_eval_is_ram_pointer((void *)0x1FFFFFFF), "below RAM");
	TEST_ASSERT(!io_eval_is_ram_pointer((void *)0x20040000), "RAM end exclusive");
	TEST_ASSERT(!io_eval_is_ram_pointer((void *)0x40000000), "peripheral");
	TEST_ASSERT(!io_eval_is_ram_pointer(NULL), "NULL rejected");
}

static void test_ram_constants(void)
{
	TEST_ASSERT_EQ_INT(0x20000000, IO_EVAL_RAM_BASE);
	TEST_ASSERT_EQ_INT(0x20040000, IO_EVAL_RAM_END);
}

/* ================================================================
 * CS pin conflict detection
 * ================================================================ */

static void test_cs_acquire_release(void)
{
	io_eval_cs_release_all();

	TEST_ASSERT(io_eval_cs_acquire(8), "acquire P0.08");
	TEST_ASSERT(io_eval_cs_release(8), "release P0.08");
}

static void test_cs_conflict_rejected(void)
{
	io_eval_cs_release_all();

	TEST_ASSERT(io_eval_cs_acquire(8), "first acquire P0.08");
	TEST_ASSERT(!io_eval_cs_acquire(8), "second acquire P0.08 rejected");
}

static void test_cs_different_pins_ok(void)
{
	io_eval_cs_release_all();

	TEST_ASSERT(io_eval_cs_acquire(8), "P0.08");
	TEST_ASSERT(io_eval_cs_acquire(6), "P0.06");
	TEST_ASSERT(io_eval_cs_acquire(26), "P0.26");
}

static void test_cs_release_allows_reacquire(void)
{
	io_eval_cs_release_all();

	io_eval_cs_acquire(8);
	io_eval_cs_release(8);
	TEST_ASSERT(io_eval_cs_acquire(8), "reacquire after release");
}

static void test_cs_is_leased(void)
{
	io_eval_cs_release_all();

	TEST_ASSERT(!io_eval_cs_is_leased(8), "not leased initially");
	io_eval_cs_acquire(8);
	TEST_ASSERT(io_eval_cs_is_leased(8), "leased after acquire");
	io_eval_cs_release(8);
	TEST_ASSERT(!io_eval_cs_is_leased(8), "not leased after release");
}

static void test_cs_table_full(void)
{
	io_eval_cs_release_all();

	TEST_ASSERT(io_eval_cs_acquire(1), "slot 1");
	TEST_ASSERT(io_eval_cs_acquire(2), "slot 2");
	TEST_ASSERT(io_eval_cs_acquire(3), "slot 3");
	TEST_ASSERT(io_eval_cs_acquire(4), "slot 4");
	TEST_ASSERT(!io_eval_cs_acquire(5), "table full (4 max)");

	io_eval_cs_release(2);
	TEST_ASSERT(io_eval_cs_acquire(5), "freed slot reused");
}

static void test_cs_release_not_leased(void)
{
	io_eval_cs_release_all();

	TEST_ASSERT(!io_eval_cs_release(99), "release not-leased fails");
}

static void test_cs_release_all(void)
{
	io_eval_cs_acquire(1);
	io_eval_cs_acquire(2);
	io_eval_cs_release_all();

	TEST_ASSERT(!io_eval_cs_is_leased(1), "all released 1");
	TEST_ASSERT(!io_eval_cs_is_leased(2), "all released 2");
}

/* ================================================================
 * I2C transfer result computation
 * ================================================================ */

static void test_i2c_result_ok_full(void)
{
	io_transfer_result_t r;
	io_eval_i2c_compute_result(IO_EVAL_BUS_OK, 10, 20, 10, &r);

	TEST_ASSERT_EQ_INT((int)IO_EVAL_BUS_OK, (int)r.bus_result);
	TEST_ASSERT_EQ_INT(10, r.bytes_written);
	TEST_ASSERT_EQ_INT(20, r.bytes_read);
}

static void test_i2c_result_ok_zero_zero(void)
{
	io_transfer_result_t r;
	io_eval_i2c_compute_result(IO_EVAL_BUS_OK, 0, 0, 0, &r);

	TEST_ASSERT_EQ_INT(0, r.bytes_written);
	TEST_ASSERT_EQ_INT(0, r.bytes_read);
}

static void test_i2c_result_nack_during_write(void)
{
	io_transfer_result_t r;
	/* NACK after 3 of 10 write bytes */
	io_eval_i2c_compute_result(IO_EVAL_BUS_NACK, 10, 20, 3, &r);

	TEST_ASSERT_EQ_INT((int)IO_EVAL_BUS_NACK, (int)r.bus_result);
	TEST_ASSERT_EQ_INT(3, r.bytes_written);
	TEST_ASSERT_EQ_INT(0, r.bytes_read);
}

static void test_i2c_result_nack_during_read(void)
{
	io_transfer_result_t r;
	/* nack_after_write >= write_len means NACK was during read phase */
	io_eval_i2c_compute_result(IO_EVAL_BUS_NACK, 10, 20, 10, &r);

	TEST_ASSERT_EQ_INT(10, r.bytes_written);
	TEST_ASSERT_EQ_INT(0, r.bytes_read);
}

static void test_i2c_result_nack_on_address(void)
{
	io_transfer_result_t r;
	io_eval_i2c_compute_result(IO_EVAL_BUS_NACK, 10, 20, 0, &r);

	TEST_ASSERT_EQ_INT(0, r.bytes_written);
	TEST_ASSERT_EQ_INT(0, r.bytes_read);
}

static void test_i2c_result_timeout(void)
{
	io_transfer_result_t r;
	io_eval_i2c_compute_result(IO_EVAL_BUS_TIMEOUT, 10, 20, 10, &r);

	TEST_ASSERT_EQ_INT((int)IO_EVAL_BUS_TIMEOUT, (int)r.bus_result);
	TEST_ASSERT_EQ_INT(0, r.bytes_written);
	TEST_ASSERT_EQ_INT(0, r.bytes_read);
}

static void test_i2c_result_null_out(void)
{
	io_eval_i2c_compute_result(IO_EVAL_BUS_OK, 10, 20, 10, NULL);
}

/* ================================================================
 * SPIM3 fixed pins
 * ================================================================ */

static void test_spim3_pins_fixed(void)
{
	TEST_ASSERT_EQ_INT(8, IO_EVAL_SPIM3_PINS.sck);
	TEST_ASSERT_EQ_INT(6, IO_EVAL_SPIM3_PINS.miso);
	TEST_ASSERT_EQ_INT(26, IO_EVAL_SPIM3_PINS.mosi);
}

static void test_spim3_pin_constants(void)
{
	TEST_ASSERT_EQ_INT(8, IO_EVAL_SPIM3_SCK_PIN);
	TEST_ASSERT_EQ_INT(6, IO_EVAL_SPIM3_MISO_PIN);
	TEST_ASSERT_EQ_INT(26, IO_EVAL_SPIM3_MOSI_PIN);
}

/* ================================================================
 * Expert force lifecycle
 * ================================================================ */

static void test_force_init_all_free(void)
{
	expert_force_init();

	for (uint8_t i = 0; i < EXPERT_FORCE_SVC_COUNT; i++) {
		TEST_ASSERT_EQ_INT((int)EXPERT_FORCE_STATE_FREE,
			(int)expert_force_get_state((expert_force_service_t)i));
	}
}

static void test_force_request_quiesce_release_restore(void)
{
	expert_force_init();

	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_request(EXPERT_FORCE_SVC_DISPLAY));
	TEST_ASSERT_EQ_INT((int)EXPERT_FORCE_STATE_QUIESCING,
		(int)expert_force_get_state(EXPERT_FORCE_SVC_DISPLAY));

	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_quiesce_complete(EXPERT_FORCE_SVC_DISPLAY));
	TEST_ASSERT(expert_force_is_displaced(EXPERT_FORCE_SVC_DISPLAY),
		"display displaced");

	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_release(EXPERT_FORCE_SVC_DISPLAY));
	TEST_ASSERT_EQ_INT((int)EXPERT_FORCE_STATE_RESTORING,
		(int)expert_force_get_state(EXPERT_FORCE_SVC_DISPLAY));

	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_restore_complete(EXPERT_FORCE_SVC_DISPLAY, true));
	TEST_ASSERT_EQ_INT((int)EXPERT_FORCE_STATE_FREE,
		(int)expert_force_get_state(EXPERT_FORCE_SVC_DISPLAY));
}

static void test_force_request_already_displaced(void)
{
	expert_force_init();

	expert_force_request(EXPERT_FORCE_SVC_SENSOR_BUS);
	expert_force_quiesce_complete(EXPERT_FORCE_SVC_SENSOR_BUS);

	TEST_ASSERT_EQ_INT(EXPERT_FORCE_ERR_ALREADY_DISPLACED,
		expert_force_request(EXPERT_FORCE_SVC_SENSOR_BUS));
}

static void test_force_request_not_cancellable(void)
{
	expert_force_init();

	expert_force_set_cancellable(EXPERT_FORCE_SVC_PDM, false);

	TEST_ASSERT_EQ_INT(EXPERT_FORCE_ERR_NOT_CANCELLABLE,
		expert_force_request(EXPERT_FORCE_SVC_PDM));
}

static void test_force_request_cancellable_after_unset(void)
{
	expert_force_init();

	expert_force_set_cancellable(EXPERT_FORCE_SVC_PDM, false);
	expert_force_set_cancellable(EXPERT_FORCE_SVC_PDM, true);

	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_request(EXPERT_FORCE_SVC_PDM));
}

static void test_force_restore_failure_to_fault(void)
{
	expert_force_init();

	expert_force_request(EXPERT_FORCE_SVC_QSPI);
	expert_force_quiesce_complete(EXPERT_FORCE_SVC_QSPI);
	expert_force_release(EXPERT_FORCE_SVC_QSPI);

	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_restore_complete(EXPERT_FORCE_SVC_QSPI, false));

	TEST_ASSERT_EQ_INT((int)EXPERT_FORCE_STATE_FAULT,
		(int)expert_force_get_state(EXPERT_FORCE_SVC_QSPI));
	TEST_ASSERT(expert_force_has_fault(), "global fault flag set");
}

static void test_force_get_fault_service(void)
{
	expert_force_init();

	expert_force_request(EXPERT_FORCE_SVC_NEOPIXEL);
	expert_force_quiesce_complete(EXPERT_FORCE_SVC_NEOPIXEL);
	expert_force_release(EXPERT_FORCE_SVC_NEOPIXEL);
	expert_force_restore_complete(EXPERT_FORCE_SVC_NEOPIXEL, false);

	int32_t fault_svc = expert_force_get_fault_service();
	TEST_ASSERT(fault_svc >= 0, "fault service found");
	TEST_ASSERT_EQ_INT((int)EXPERT_FORCE_SVC_NEOPIXEL, fault_svc);
}

static void test_force_clear_fault(void)
{
	expert_force_init();

	expert_force_request(EXPERT_FORCE_SVC_BUZZER);
	expert_force_quiesce_complete(EXPERT_FORCE_SVC_BUZZER);
	expert_force_release(EXPERT_FORCE_SVC_BUZZER);
	expert_force_restore_complete(EXPERT_FORCE_SVC_BUZZER, false);

	TEST_ASSERT(expert_force_has_fault(), "fault present");

	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_clear_fault(EXPERT_FORCE_SVC_BUZZER));
	TEST_ASSERT(!expert_force_has_fault(), "fault cleared");
	TEST_ASSERT_EQ_INT((int)EXPERT_FORCE_STATE_FREE,
		(int)expert_force_get_state(EXPERT_FORCE_SVC_BUZZER));
}

static void test_force_wrong_state_quiesce_complete(void)
{
	expert_force_init();

	TEST_ASSERT_EQ_INT(EXPERT_FORCE_ERR_WRONG_STATE,
		expert_force_quiesce_complete(EXPERT_FORCE_SVC_DISPLAY));
}

static void test_force_wrong_state_release(void)
{
	expert_force_init();

	TEST_ASSERT_EQ_INT(EXPERT_FORCE_ERR_NOT_DISPLACED,
		expert_force_release(EXPERT_FORCE_SVC_DISPLAY));
}

static void test_force_wrong_state_restore_complete(void)
{
	expert_force_init();

	TEST_ASSERT_EQ_INT(EXPERT_FORCE_ERR_WRONG_STATE,
		expert_force_restore_complete(EXPERT_FORCE_SVC_DISPLAY, true));
}

static void test_force_invalid_param(void)
{
	TEST_ASSERT_EQ_INT(EXPERT_FORCE_ERR_INVALID_PARAM,
		expert_force_request((expert_force_service_t)99));
	TEST_ASSERT_EQ_INT(EXPERT_FORCE_ERR_INVALID_PARAM,
		expert_force_quiesce_complete((expert_force_service_t)99));
	TEST_ASSERT_EQ_INT(EXPERT_FORCE_ERR_INVALID_PARAM,
		expert_force_release((expert_force_service_t)99));
	TEST_ASSERT_EQ_INT(EXPERT_FORCE_ERR_INVALID_PARAM,
		expert_force_restore_complete((expert_force_service_t)99, true));
}

static void test_force_displaced_mask_empty(void)
{
	expert_force_init();
	TEST_ASSERT_EQ_INT(0, expert_force_displaced_mask());
}

static void test_force_displaced_mask_multiple(void)
{
	expert_force_init();

	expert_force_request(EXPERT_FORCE_SVC_DISPLAY);
	expert_force_quiesce_complete(EXPERT_FORCE_SVC_DISPLAY);

	expert_force_request(EXPERT_FORCE_SVC_QSPI);
	expert_force_quiesce_complete(EXPERT_FORCE_SVC_QSPI);

	uint32_t mask = expert_force_displaced_mask();
	TEST_ASSERT(mask & (1u << EXPERT_FORCE_SVC_DISPLAY), "display in mask");
	TEST_ASSERT(mask & (1u << EXPERT_FORCE_SVC_QSPI), "qspi in mask");
	TEST_ASSERT(!(mask & (1u << EXPERT_FORCE_SVC_PDM)), "pdm not in mask");
}

static void test_force_all_six_services(void)
{
	expert_force_init();

	expert_force_service_t svcs[] = {
		EXPERT_FORCE_SVC_DISPLAY,
		EXPERT_FORCE_SVC_SENSOR_BUS,
		EXPERT_FORCE_SVC_QSPI,
		EXPERT_FORCE_SVC_PDM,
		EXPERT_FORCE_SVC_NEOPIXEL,
		EXPERT_FORCE_SVC_BUZZER,
	};

	for (int i = 0; i < 6; i++) {
		TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
			expert_force_request(svcs[i]));
		TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
			expert_force_quiesce_complete(svcs[i]));
		TEST_ASSERT(expert_force_is_displaced(svcs[i]),
			"service displaced");
	}

	TEST_ASSERT_EQ_INT(0x3F, expert_force_displaced_mask());
}

static void test_force_service_count(void)
{
	TEST_ASSERT_EQ_INT(6, EXPERT_FORCE_SVC_COUNT);
}

static void test_force_request_on_fault_rejected(void)
{
	expert_force_init();

	expert_force_request(EXPERT_FORCE_SVC_DISPLAY);
	expert_force_quiesce_complete(EXPERT_FORCE_SVC_DISPLAY);
	expert_force_release(EXPERT_FORCE_SVC_DISPLAY);
	expert_force_restore_complete(EXPERT_FORCE_SVC_DISPLAY, false);

	TEST_ASSERT_EQ_INT(EXPERT_FORCE_ERR_WRONG_STATE,
		expert_force_request(EXPERT_FORCE_SVC_DISPLAY));
}

/* ================================================================
 * Whole-service force simulation for each service type
 * (tests the force flow for display, sensors, QSPI, PDM, NeoPixel,
 * buzzer — the six service categories in the plan)
 * ================================================================ */

static void test_force_display_quiesce_restore(void)
{
	expert_force_init();

	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_request(EXPERT_FORCE_SVC_DISPLAY));
	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_quiesce_complete(EXPERT_FORCE_SVC_DISPLAY));
	TEST_ASSERT(expert_force_is_displaced(EXPERT_FORCE_SVC_DISPLAY),
		"display displaced");

	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_release(EXPERT_FORCE_SVC_DISPLAY));
	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_restore_complete(EXPERT_FORCE_SVC_DISPLAY, true));
	TEST_ASSERT_EQ_INT((int)EXPERT_FORCE_STATE_FREE,
		(int)expert_force_get_state(EXPERT_FORCE_SVC_DISPLAY));
}

static void test_force_sensor_bus_non_cancellable_when_streaming(void)
{
	expert_force_init();

	expert_force_set_cancellable(EXPERT_FORCE_SVC_SENSOR_BUS, false);

	TEST_ASSERT_EQ_INT(EXPERT_FORCE_ERR_NOT_CANCELLABLE,
		expert_force_request(EXPERT_FORCE_SVC_SENSOR_BUS));
	TEST_ASSERT(!expert_force_is_displaced(EXPERT_FORCE_SVC_SENSOR_BUS),
		"sensor bus NOT displaced");
}

static void test_force_qspi_quiesce_pause_resume(void)
{
	expert_force_init();

	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_request(EXPERT_FORCE_SVC_QSPI));
	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_quiesce_complete(EXPERT_FORCE_SVC_QSPI));

	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_release(EXPERT_FORCE_SVC_QSPI));
	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_restore_complete(EXPERT_FORCE_SVC_QSPI, true));
}

static void test_force_pdm_non_cancellable_when_capturing(void)
{
	expert_force_init();

	expert_force_set_cancellable(EXPERT_FORCE_SVC_PDM, false);
	TEST_ASSERT_EQ_INT(EXPERT_FORCE_ERR_NOT_CANCELLABLE,
		expert_force_request(EXPERT_FORCE_SVC_PDM));
}

static void test_force_neopixel_quiesce_restore(void)
{
	expert_force_init();

	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_request(EXPERT_FORCE_SVC_NEOPIXEL));
	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_quiesce_complete(EXPERT_FORCE_SVC_NEOPIXEL));
	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_release(EXPERT_FORCE_SVC_NEOPIXEL));
	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_restore_complete(EXPERT_FORCE_SVC_NEOPIXEL, true));
}

static void test_force_buzzer_quiesce_stop_restore(void)
{
	expert_force_init();

	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_request(EXPERT_FORCE_SVC_BUZZER));
	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_quiesce_complete(EXPERT_FORCE_SVC_BUZZER));
	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_release(EXPERT_FORCE_SVC_BUZZER));
	TEST_ASSERT_EQ_INT(EXPERT_FORCE_OK,
		expert_force_restore_complete(EXPERT_FORCE_SVC_BUZZER, true));
}

static void test_force_multiple_simultaneous(void)
{
	expert_force_init();

	expert_force_request(EXPERT_FORCE_SVC_DISPLAY);
	expert_force_quiesce_complete(EXPERT_FORCE_SVC_DISPLAY);

	expert_force_request(EXPERT_FORCE_SVC_NEOPIXEL);
	expert_force_quiesce_complete(EXPERT_FORCE_SVC_NEOPIXEL);

	TEST_ASSERT(expert_force_is_displaced(EXPERT_FORCE_SVC_DISPLAY), "display");
	TEST_ASSERT(expert_force_is_displaced(EXPERT_FORCE_SVC_NEOPIXEL), "neopixel");
	TEST_ASSERT(!expert_force_is_displaced(EXPERT_FORCE_SVC_QSPI), "qspi free");
}

/* ---- Compile-time guards ---- */
static_assert(IO_EVAL_MAX_BUFFER == 256, "expert buffer limit is 256");
static_assert(IO_EVAL_SPI_FREQ_COUNT == 7, "7 discrete SPI frequencies");
static_assert(EXPERT_FORCE_SVC_COUNT == 6, "6 service categories");

int main(void)
{
	test_framework_init();

	/* Buffer bounds */
	RUN_TEST(test_buffer_size_zero_valid);
	RUN_TEST(test_buffer_size_max_valid);
	RUN_TEST(test_buffer_size_oversize_rejected);
	RUN_TEST(test_buffer_max_constant);

	/* I2C onboard address */
	RUN_TEST(test_i2c_onboard_count);
	RUN_TEST(test_i2c_onboard_all_five);
	RUN_TEST(test_i2c_external_not_onboard);
	RUN_TEST(test_i2c_address_valid_range);
	RUN_TEST(test_i2c_address_reserved_rejected);

	/* SPI mode */
	RUN_TEST(test_spi_mode_all_valid);
	RUN_TEST(test_spi_mode_unknown_rejected);
	RUN_TEST(test_spi_mode_specific_mapping);

	/* SPI frequency */
	RUN_TEST(test_spi_freq_count);
	RUN_TEST(test_spi_freq_all_discrete);
	RUN_TEST(test_spi_freq_clamp_down);
	RUN_TEST(test_spi_freq_clamp_below_min);
	RUN_TEST(test_spi_freq_reject_above_max);
	RUN_TEST(test_spi_freq_constants);

	/* EasyDMA RAM */
	RUN_TEST(test_ram_pointer_in_range);
	RUN_TEST(test_ram_pointer_flash_rejected);
	RUN_TEST(test_ram_pointer_out_of_range);
	RUN_TEST(test_ram_constants);

	/* CS conflict */
	RUN_TEST(test_cs_acquire_release);
	RUN_TEST(test_cs_conflict_rejected);
	RUN_TEST(test_cs_different_pins_ok);
	RUN_TEST(test_cs_release_allows_reacquire);
	RUN_TEST(test_cs_is_leased);
	RUN_TEST(test_cs_table_full);
	RUN_TEST(test_cs_release_not_leased);
	RUN_TEST(test_cs_release_all);

	/* I2C transfer result */
	RUN_TEST(test_i2c_result_ok_full);
	RUN_TEST(test_i2c_result_ok_zero_zero);
	RUN_TEST(test_i2c_result_nack_during_write);
	RUN_TEST(test_i2c_result_nack_during_read);
	RUN_TEST(test_i2c_result_nack_on_address);
	RUN_TEST(test_i2c_result_timeout);
	RUN_TEST(test_i2c_result_null_out);

	/* SPIM3 pins */
	RUN_TEST(test_spim3_pins_fixed);
	RUN_TEST(test_spim3_pin_constants);

	/* Force lifecycle */
	RUN_TEST(test_force_init_all_free);
	RUN_TEST(test_force_request_quiesce_release_restore);
	RUN_TEST(test_force_request_already_displaced);
	RUN_TEST(test_force_request_not_cancellable);
	RUN_TEST(test_force_request_cancellable_after_unset);
	RUN_TEST(test_force_restore_failure_to_fault);
	RUN_TEST(test_force_get_fault_service);
	RUN_TEST(test_force_clear_fault);
	RUN_TEST(test_force_wrong_state_quiesce_complete);
	RUN_TEST(test_force_wrong_state_release);
	RUN_TEST(test_force_wrong_state_restore_complete);
	RUN_TEST(test_force_invalid_param);
	RUN_TEST(test_force_displaced_mask_empty);
	RUN_TEST(test_force_displaced_mask_multiple);
	RUN_TEST(test_force_all_six_services);
	RUN_TEST(test_force_service_count);
	RUN_TEST(test_force_request_on_fault_rejected);

	/* Whole-service force simulation */
	RUN_TEST(test_force_display_quiesce_restore);
	RUN_TEST(test_force_sensor_bus_non_cancellable_when_streaming);
	RUN_TEST(test_force_qspi_quiesce_pause_resume);
	RUN_TEST(test_force_pdm_non_cancellable_when_capturing);
	RUN_TEST(test_force_neopixel_quiesce_restore);
	RUN_TEST(test_force_buzzer_quiesce_stop_restore);
	RUN_TEST(test_force_multiple_simultaneous);

	return test_framework_finish();
}
