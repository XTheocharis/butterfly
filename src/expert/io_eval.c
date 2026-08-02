/*
 * io_eval.c - Pure-C I2C/SPI expert I/O validation (host-testable).
 *
 * No SDK deps. All decision logic for the CLUE expert I2C and SPI APIs.
 * Compiled from both firmware wrappers (i2c.cpp, spi.cpp) and host tests.
 */
#include "io_eval.h"

/* ---- Buffer bounds --------------------------------------------------- */

bool io_eval_buffer_size_valid(uint32_t size)
{
	return size <= IO_EVAL_MAX_BUFFER;
}

/* ---- I2C onboard address classification -------------------------------
 *
 * Must match i2cBus.h I2CBUS_ONBOARD_ADDRESSES:
 *   0x6A (LSM6DS33/DS3TR-C), 0x1C (LIS3MDL), 0x39 (APDS9960),
 *   0x44 (SHT31D), 0x77 (BMP280).
 */
const uint8_t IO_EVAL_ONBOARD_ADDRESSES[IO_EVAL_ONBOARD_ADDR_COUNT] = {
	0x6A, 0x1C, 0x39, 0x44, 0x77,
};

bool io_eval_is_onboard_i2c(uint8_t address)
{
	for (uint8_t i = 0; i < IO_EVAL_ONBOARD_ADDR_COUNT; i++) {
		if (IO_EVAL_ONBOARD_ADDRESSES[i] == address)
			return true;
	}
	return false;
}

bool io_eval_i2c_address_valid(uint8_t address)
{
	/* 7-bit address space: 0x08-0x77 is usable.
	 * 0x00-0x07: reserved (general call).
	 * 0x78-0x7F: reserved (10-bit extensions). */
	return address >= 0x08u && address <= 0x77u;
}

/* ---- SPI mode validation --------------------------------------------- */

bool io_eval_spi_mode_valid(int32_t proto_mode, io_spi_mode_t *out_mode)
{
	/* Proto SpiMode: UNKNOWN=0, MODE_0=1, MODE_1=2, MODE_2=3, MODE_3=4.
	 * So valid proto values are 1-4, mapping to io_spi_mode_t 0-3. */
	if (proto_mode < 1 || proto_mode > 4)
		return false;
	if (out_mode)
		*out_mode = (io_spi_mode_t)(proto_mode - 1);
	return true;
}

/* ---- SPI discrete frequency table ------------------------------------ */

const uint32_t IO_EVAL_SPI_FREQS[IO_EVAL_SPI_FREQ_COUNT] = {
	125000u,   /* 125 kHz */
	250000u,   /* 250 kHz */
	500000u,   /* 500 kHz */
	1000000u,  /* 1 MHz */
	2000000u,  /* 2 MHz */
	4000000u,  /* 4 MHz */
	8000000u,  /* 8 MHz */
};

bool io_eval_spi_freq_clamp(uint32_t requested_hz, uint32_t *out_actual)
{
	if (out_actual) *out_actual = 0;

	if (requested_hz > IO_EVAL_SPI_FREQ_MAX)
		return false;

	/* Clamp down: find the highest discrete frequency <= requested. */
	uint32_t best = IO_EVAL_SPI_FREQS[0];
	for (uint8_t i = 0; i < IO_EVAL_SPI_FREQ_COUNT; i++) {
		if (IO_EVAL_SPI_FREQS[i] <= requested_hz)
			best = IO_EVAL_SPI_FREQS[i];
	}

	if (out_actual) *out_actual = best;
	return true;
}

/* ---- EasyDMA RAM pointer validation ---------------------------------- */

bool io_eval_is_ram_pointer(const void *ptr)
{
	uint32_t addr = (uint32_t)(uintptr_t)ptr;
	return addr >= IO_EVAL_RAM_BASE && addr < IO_EVAL_RAM_END;
}

/* ---- CS pin conflict detection --------------------------------------- */

typedef struct {
	uint32_t cs_pin;
	bool     active;
} io_cs_lease_entry_t;

static io_cs_lease_entry_t g_cs_leases[IO_EVAL_MAX_CS_LEASES];

bool io_eval_cs_acquire(uint32_t cs_pin)
{
	/* Check for conflict first. */
	for (uint8_t i = 0; i < IO_EVAL_MAX_CS_LEASES; i++) {
		if (g_cs_leases[i].active && g_cs_leases[i].cs_pin == cs_pin)
			return false; /* already leased */
	}

	/* Find free slot. */
	for (uint8_t i = 0; i < IO_EVAL_MAX_CS_LEASES; i++) {
		if (!g_cs_leases[i].active) {
			g_cs_leases[i].cs_pin  = cs_pin;
			g_cs_leases[i].active  = true;
			return true;
		}
	}
	return false; /* table full */
}

bool io_eval_cs_release(uint32_t cs_pin)
{
	for (uint8_t i = 0; i < IO_EVAL_MAX_CS_LEASES; i++) {
		if (g_cs_leases[i].active && g_cs_leases[i].cs_pin == cs_pin) {
			g_cs_leases[i].active = false;
			return true;
		}
	}
	return false;
}

bool io_eval_cs_is_leased(uint32_t cs_pin)
{
	for (uint8_t i = 0; i < IO_EVAL_MAX_CS_LEASES; i++) {
		if (g_cs_leases[i].active && g_cs_leases[i].cs_pin == cs_pin)
			return true;
	}
	return false;
}

void io_eval_cs_release_all(void)
{
	for (uint8_t i = 0; i < IO_EVAL_MAX_CS_LEASES; i++)
		g_cs_leases[i].active = false;
}

/* ---- I2C transfer result computation --------------------------------- */

void io_eval_i2c_compute_result(io_bus_result_t bus_result,
                                uint32_t write_len, uint32_t read_len,
                                uint32_t nack_after_write,
                                io_transfer_result_t *out)
{
	if (!out) return;

	out->bus_result = bus_result;

	switch (bus_result) {
	case IO_EVAL_BUS_OK:
		out->bytes_written = write_len;
		out->bytes_read    = read_len;
		break;

	case IO_EVAL_BUS_NACK:
		/* If NACK happened during write phase, bytes_written reflects
		 * how many bytes were acknowledged. bytes_read = 0.
		 * If nack_after_write >= write_len, NACK was during read phase:
		 * all write bytes were sent, partial read. But we don't track
		 * partial read bytes from the bus — set bytes_read = 0. */
		if (nack_after_write >= write_len) {
			out->bytes_written = write_len;
			out->bytes_read    = 0;
		} else {
			out->bytes_written = nack_after_write;
			out->bytes_read    = 0;
		}
		break;

	case IO_EVAL_BUS_TIMEOUT:
		/* Indeterminate: report 0 bytes transferred. */
		out->bytes_written = 0;
		out->bytes_read    = 0;
		break;

	case IO_EVAL_BUS_ERROR:
		out->bytes_written = 0;
		out->bytes_read    = 0;
		break;
	}
}

/* ---- SPIM3 fixed pins ------------------------------------------------ */

const io_spim3_pins_t IO_EVAL_SPIM3_PINS = {
	IO_EVAL_SPIM3_SCK_PIN,
	IO_EVAL_SPIM3_MISO_PIN,
	IO_EVAL_SPIM3_MOSI_PIN,
};
