/*
 * io_eval.h - Pure-C I2C/SPI expert I/O validation (host-testable).
 *
 * Contains ALL decision logic for the CLUE expert I2C and SPI APIs:
 *   - I2C onboard address classification (5 CLUE sensor addresses)
 *   - Buffer bounds enforcement (<=256 bytes per direction)
 *   - SPI mode validation (0-3, reject UNKNOWN)
 *   - SPI discrete frequency table (125k-8M, clamp-down, reject >8M)
 *   - EasyDMA RAM pointer validation (reject flash addresses)
 *   - CS pin conflict detection (two SPI users, same CS pin)
 *   - I2C transfer result computation (partial counts from NACK/timeout)
 *
 * No SDK deps. Compiled from both firmware wrappers (i2c.cpp, spi.cpp)
 * and host tests (test_expert_io.cpp).
 *
 * Onboard I2C addresses require force=true AND quiescence of the
 * entire TWIM1 sensor-bus group (PINREG_GROUP_TWIM1). External
 * addresses do not require force.
 */
#ifndef IO_EVAL_H
#define IO_EVAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Buffer bounds ---------------------------------------------------
 *
 * The proto allows PB_BYTES_ARRAY_T(512) but the expert layer enforces
 * a stricter 256-byte limit per direction. Larger buffers are rejected
 * as INVALID_ARGUMENT before reaching the hardware.
 */
#define IO_EVAL_MAX_BUFFER 256u

bool io_eval_buffer_size_valid(uint32_t size);

/* ---- I2C onboard address classification ------------------------------ */

/* The five CLUE onboard sensor addresses on TWIM1. Must match
 * i2cBus.h I2CBUS_ONBOARD_ADDRESSES — single source of truth here. */
#define IO_EVAL_ONBOARD_ADDR_COUNT 5u

extern const uint8_t IO_EVAL_ONBOARD_ADDRESSES[IO_EVAL_ONBOARD_ADDR_COUNT];

/* Check if a 7-bit address is onboard. */
bool io_eval_is_onboard_i2c(uint8_t address);

/* Validate 7-bit address range (0x08-0x77, reserved range excluded). */
bool io_eval_i2c_address_valid(uint8_t address);

/* ---- SPI mode validation --------------------------------------------- */

typedef enum {
	IO_EVAL_SPI_MODE_0 = 0,  /* CPOL=0, CPHA=0 */
	IO_EVAL_SPI_MODE_1 = 1,  /* CPOL=0, CPHA=1 */
	IO_EVAL_SPI_MODE_2 = 2,  /* CPOL=1, CPHA=0 */
	IO_EVAL_SPI_MODE_3 = 3,  /* CPOL=1, CPHA=1 */
} io_spi_mode_t;

#define IO_EVAL_SPI_MODE_COUNT 4u

/* Validate mode enum (0-3). Reject UNKNOWN (-1 / 0 from proto). */
bool io_eval_spi_mode_valid(int32_t proto_mode, io_spi_mode_t *out_mode);

/* ---- SPI discrete frequency table ------------------------------------
 *
 * nRF52840 SPIM supports 7 discrete frequencies. Requests are
 * clamped DOWN to the nearest supported frequency that does not
 * exceed the request. Requests above 8 MHz are rejected.
 */
#define IO_EVAL_SPI_FREQ_COUNT 7u

extern const uint32_t IO_EVAL_SPI_FREQS[IO_EVAL_SPI_FREQ_COUNT];

#define IO_EVAL_SPI_FREQ_MIN  125000u
#define IO_EVAL_SPI_FREQ_MAX  8000000u

/* Validate and clamp frequency. Returns true if the frequency is
 * within the supported range [125k, 8M]. Sets *out_actual to the
 * nearest discrete frequency <= the request. Returns false for
 * requests above 8 MHz (INVALID_ARGUMENT). */
bool io_eval_spi_freq_clamp(uint32_t requested_hz, uint32_t *out_actual);

/* ---- EasyDMA RAM pointer validation ----------------------------------
 *
 * nRF52840 EasyDMA requires data in RAM (0x20000000-0x20040000).
 * Flash pointers cause a HardFault. The expert layer must reject
 * non-RAM pointers before the hardware sees them.
 */
#define IO_EVAL_RAM_BASE  0x20000000u
#define IO_EVAL_RAM_END   0x20040000u

bool io_eval_is_ram_pointer(const void *ptr);

/* ---- CS pin conflict detection ---------------------------------------
 *
 * Multiple concurrent SPI transfers on the same CS pin are rejected.
 * The CS lease table tracks which CS pins are currently in use.
 */
#define IO_EVAL_MAX_CS_LEASES 4u

bool io_eval_cs_acquire(uint32_t cs_pin);
bool io_eval_cs_release(uint32_t cs_pin);
bool io_eval_cs_is_leased(uint32_t cs_pin);
void io_eval_cs_release_all(void);

/* ---- I2C transfer result computation ---------------------------------
 *
 * Given a bus result (OK/NACK/TIMEOUT) and the requested lengths,
 * compute the partial bytes_written and bytes_read for the response.
 * On NACK during write: bytes_written = bytes acknowledged before NACK.
 * On NACK during read: bytes_read = bytes received before NACK.
 * On TIMEOUT: 0 bytes (indeterminate).
 */
typedef enum {
	IO_EVAL_BUS_OK      = 0,
	IO_EVAL_BUS_NACK    = 1,
	IO_EVAL_BUS_TIMEOUT = 2,
	IO_EVAL_BUS_ERROR   = 3,
} io_bus_result_t;

typedef struct {
	uint32_t bytes_written;
	uint32_t bytes_read;
	io_bus_result_t bus_result;
} io_transfer_result_t;

/* Compute partial transfer result. nack_after_write = how many write
 * bytes were acknowledged before the NACK (0 if NACK on address). */
void io_eval_i2c_compute_result(io_bus_result_t bus_result,
                                uint32_t write_len, uint32_t read_len,
                                uint32_t nack_after_write,
                                io_transfer_result_t *out);

/* ---- SPIM3 fixed edge-connector pins ---------------------------------
 *
 * SCK=D13/P0.08, MISO=D14/P0.06, MOSI=D15/P0.26.
 * CS is a separately leased GPIO (not part of SPIM3 pins).
 */
#define IO_EVAL_SPIM3_SCK_PIN  8u   /* P0.08 = D13 */
#define IO_EVAL_SPIM3_MISO_PIN 6u   /* P0.06 = D14 */
#define IO_EVAL_SPIM3_MOSI_PIN 26u  /* P0.26 = D15 */

typedef struct {
	uint8_t sck;
	uint8_t miso;
	uint8_t mosi;
} io_spim3_pins_t;

extern const io_spim3_pins_t IO_EVAL_SPIM3_PINS;

#ifdef __cplusplus
}
#endif
#endif /* IO_EVAL_H */
