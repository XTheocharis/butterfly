/*
 * spi.h - External SPI expert API for CLUE edge connector.
 *
 * Provides Board protocol SpiTransfer over SPIM3 (the edge-connector
 * SPI bus). Fixed pins: SCK=D13/P0.08, MISO=D14/P0.06, MOSI=D15/P0.26.
 * Modes 0-3 (CPOL/CPHA). Discrete frequencies 125kHz-8MHz.
 *
 * CS is a separately leased GPIO (via Todo 31's gpio expert). CS
 * polarity is active-low (standard). Setup/hold timing enforced.
 * CS is ALWAYS deasserted on error — never left asserted.
 *
 * Buffers <=256 bytes. EasyDMA requires RAM (not flash) — non-RAM
 * pointers are rejected with INVALID_ARGUMENT.
 *
 * allow: SIZE_OK — thin firmware wrapper. All decision logic is in
 * io_eval.{h,c}; this file translates between the Board protocol
 * and SPIM3 hardware via the backend struct.
 *
 * Compiled only under BOARD_CLUE.
 */
#ifndef EXPERT_SPI_H
#define EXPERT_SPI_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "io_eval.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Transfer descriptor ---- */

typedef struct {
	uint32_t           cs_pin;     /* leased GPIO resource (P0.NN)    */
	uint32_t           frequency_hz;
	io_spi_mode_t      mode;
	const uint8_t     *tx_buf;     /* RAM (EasyDMA). NULL if tx_len=0 */
	size_t             tx_len;
	uint8_t           *rx_buf;     /* RAM (EasyDMA). NULL if rx_len=0 */
	size_t             rx_len;
} spi_expert_transfer_t;

/* ---- Transfer result ---- */

typedef struct {
	io_bus_result_t  bus_result;
	uint32_t         bytes_written;
	uint32_t         bytes_read;
} spi_expert_result_t;

/* ---- Result codes (map to BoardResultCode) ---- */

typedef enum {
	SPI_EXPERT_OK              = 0,
	SPI_EXPERT_ERR_INVALID_ARG = 2,
	SPI_EXPERT_ERR_PERMISSION  = 3,
	SPI_EXPERT_ERR_BUSY        = 6,
	SPI_EXPERT_ERR_CS_CONFLICT = 9,  /* custom: CS already in use */
	SPI_EXPERT_ERR_NOT_IMPL    = 14,
} spi_expert_code_t;

/* ---- CS timing (microseconds) ---- */

#define SPI_CS_SETUP_US  1u
#define SPI_CS_HOLD_US   1u

/* ---- Backend (hardware abstraction) ----
 *
 * Wraps nrfx_spim instance 3. The backend performs a blocking
 * full-duplex transfer with CS asserted/deasserted by the wrapper.
 */
typedef struct {
	/* Assert CS (active-low: drive low). */
	void (*cs_assert)(uint32_t cs_pin);
	/* Deassert CS (drive high). */
	void (*cs_deassert)(uint32_t cs_pin);
	/* Blocking full-duplex SPI transfer.
	 * tx_buf/rx_buf must be RAM. If rx_buf=NULL, discard MISO.
	 * If tx_buf=NULL, send 0x00 for rx_len bytes.
	 * Returns 0 on success, !0 on hardware error. */
	int (*transfer)(uint32_t frequency_hz, io_spi_mode_t mode,
	                const uint8_t *tx_buf, size_t tx_len,
	                uint8_t *rx_buf, size_t rx_len);
	/* Microsecond delay (for CS setup/hold). */
	void (*delay_us)(uint32_t us);
} spi_backend_t;

/* ---- Public API ---- */

void spi_expert_init(const spi_backend_t *backend);

/* Execute an SPI transfer.
 * Validates mode, frequency, buffer sizes, RAM pointers.
 * Acquires CS (rejects conflict), asserts CS, transfers,
 * deasserts CS. On any error, CS is deasserted. */
spi_expert_code_t spi_expert_transfer(const spi_expert_transfer_t *xfer,
                                      spi_expert_result_t *out_result);

/* Release a CS pin lease (for multi-transfer sessions). */
spi_expert_code_t spi_expert_release_cs(uint32_t cs_pin);

#ifdef __cplusplus
}
#endif
#endif /* EXPERT_SPI_H */
