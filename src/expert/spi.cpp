/*
 * spi.cpp - External SPI expert API firmware implementation.
 *
 * Wraps io_eval.{h,c} with nrfx_spim instance 3 (SPIM3) hardware.
 * Fixed edge-connector pins: SCK=P0.08, MISO=P0.06, MOSI=P0.26.
 * CS is a separately leased GPIO, driven active-low.
 *
 * CS lifecycle: assert → setup delay → transfer → hold delay → deassert.
 * On any error: CS is deasserted immediately. Never left asserted.
 *
 * Compiled only under BOARD_CLUE.
 */
#ifdef BOARD_CLUE

#include "spi.h"
#include "custom_board.h"

#include "nrf.h"
#include "nrf_gpio.h"
#include "nrfx_spim.h"
#include "nrf_delay.h"
#include "app_util_platform.h"
#include <cstring>

extern "C" {
#include "io_eval.h"
}

/* ---- nrfx_spim3 backend ---- */

static nrfx_spim_t g_spim3 = NRFX_SPIM_INSTANCE(3);
static bool g_spim3_initialized;

static nrf_spim_frequency_t freq_to_nrf(uint32_t hz)
{
	switch (hz) {
	case 125000:  return NRF_SPIM_FREQ_125K;
	case 250000:  return NRF_SPIM_FREQ_250K;
	case 500000:  return NRF_SPIM_FREQ_500K;
	case 1000000: return NRF_SPIM_FREQ_1M;
	case 2000000: return NRF_SPIM_FREQ_2M;
	case 4000000: return NRF_SPIM_FREQ_4M;
	case 8000000: return NRF_SPIM_FREQ_8M;
	default:      return NRF_SPIM_FREQ_1M;
	}
}

static nrf_spim_mode_t mode_to_nrf(io_spi_mode_t mode)
{
	switch (mode) {
	case IO_EVAL_SPI_MODE_0: return NRF_SPIM_MODE_0;
	case IO_EVAL_SPI_MODE_1: return NRF_SPIM_MODE_1;
	case IO_EVAL_SPI_MODE_2: return NRF_SPIM_MODE_2;
	case IO_EVAL_SPI_MODE_3: return NRF_SPIM_MODE_3;
	default:                 return NRF_SPIM_MODE_0;
	}
}

static void ensure_spim3_init(uint32_t frequency_hz, io_spi_mode_t mode)
{
	if (g_spim3_initialized) return;

	nrfx_spim_config_t cfg = {
		.sck_pin        = IO_EVAL_SPIM3_SCK_PIN,
		.mosi_pin       = IO_EVAL_SPIM3_MOSI_PIN,
		.miso_pin       = IO_EVAL_SPIM3_MISO_PIN,
		.ss_pin         = NRFX_SPIM_PIN_NOT_USED,
		.ss_active_high = false,
		.irq_priority   = 6,
		.orc            = 0xFF,
		.frequency      = freq_to_nrf(frequency_hz),
		.mode           = mode_to_nrf(mode),
		.bit_order      = NRF_SPIM_BIT_ORDER_MSB_FIRST,
	};

	nrfx_spim_init(&g_spim3, &cfg, NULL, NULL);
	g_spim3_initialized = true;
}

static void hw_cs_assert(uint32_t cs_pin)
{
	nrf_gpio_pin_clear(cs_pin);
}

static void hw_cs_deassert(uint32_t cs_pin)
{
	nrf_gpio_pin_set(cs_pin);
}

static int hw_transfer(uint32_t frequency_hz, io_spi_mode_t mode,
                       const uint8_t *tx_buf, size_t tx_len,
                       uint8_t *rx_buf, size_t rx_len)
{
	ensure_spim3_init(frequency_hz, mode);

	/* nrfx_spim requires at least a 1-byte buffer. If tx_buf is NULL,
	 * send 0x00. If rx_buf is NULL, use a dummy sink. */
	static uint8_t dummy_tx[256];
	static uint8_t dummy_rx[256];

	const uint8_t *tx = tx_buf;
	if (!tx) {
		memset(dummy_tx, 0, tx_len > 256 ? 256 : tx_len);
		tx = dummy_tx;
	}

	uint8_t *rx = rx_buf;
	if (!rx) {
		rx = dummy_rx;
	}

	size_t len = tx_len > rx_len ? tx_len : rx_len;
	if (len == 0) return 0;
	if (len > 256) return -1; /* bounded by io_eval before reaching here */

	nrfx_spim_xfer_desc_t xfer = NRFX_SPIM_XFER_TRX(tx, tx_len, rx, rx_len);
	nrfx_err_t err = nrfx_spim_xfer(&g_spim3, &xfer, 0);
	return (err == NRFX_SUCCESS) ? 0 : -1;
}

static void hw_delay_us(uint32_t us)
{
	/* nRF52840: nrf_delay_us from app_util_platform.h */
	nrf_delay_us(us);
}

static const spi_backend_t HW_BACKEND = {
	.cs_assert   = hw_cs_assert,
	.cs_deassert = hw_cs_deassert,
	.transfer    = hw_transfer,
	.delay_us    = hw_delay_us,
};

/* ---- State ---- */

static const spi_backend_t *g_backend;
static bool g_initialized;

void spi_expert_init(const spi_backend_t *backend)
{
	g_backend = backend ? backend : &HW_BACKEND;
	g_initialized = true;
}

spi_expert_code_t spi_expert_transfer(const spi_expert_transfer_t *xfer,
                                      spi_expert_result_t *out_result)
{
	if (!g_initialized || !xfer)
		return SPI_EXPERT_ERR_INVALID_ARG;

	if (out_result) {
		out_result->bus_result    = IO_EVAL_BUS_OK;
		out_result->bytes_written = 0;
		out_result->bytes_read    = 0;
	}

	/* Validate buffer sizes. */
	if (!io_eval_buffer_size_valid(xfer->tx_len) ||
	    !io_eval_buffer_size_valid(xfer->rx_len))
		return SPI_EXPERT_ERR_INVALID_ARG;

	/* Validate RAM pointers for EasyDMA. */
	if (xfer->tx_len > 0 && xfer->tx_buf &&
	    !io_eval_is_ram_pointer(xfer->tx_buf))
		return SPI_EXPERT_ERR_INVALID_ARG;
	if (xfer->rx_len > 0 && xfer->rx_buf &&
	    !io_eval_is_ram_pointer(xfer->rx_buf))
		return SPI_EXPERT_ERR_INVALID_ARG;

	/* Validate and clamp frequency. */
	uint32_t actual_freq = 0;
	if (!io_eval_spi_freq_clamp(xfer->frequency_hz, &actual_freq))
		return SPI_EXPERT_ERR_INVALID_ARG;

	/* Acquire CS (reject conflict). */
	if (!io_eval_cs_acquire(xfer->cs_pin))
		return SPI_EXPERT_ERR_CS_CONFLICT;

	if (!g_backend->cs_assert || !g_backend->cs_deassert ||
	    !g_backend->transfer) {
		io_eval_cs_release(xfer->cs_pin);
		return SPI_EXPERT_ERR_NOT_IMPL;
	}

	/* CS lifecycle: assert → setup → transfer → hold → deassert. */
	g_backend->cs_assert(xfer->cs_pin);

	if (g_backend->delay_us)
		g_backend->delay_us(SPI_CS_SETUP_US);

	int hw_result = g_backend->transfer(
		actual_freq, xfer->mode,
		xfer->tx_buf, xfer->tx_len,
		xfer->rx_buf, xfer->rx_len);

	if (g_backend->delay_us)
		g_backend->delay_us(SPI_CS_HOLD_US);

	/* CS is ALWAYS deasserted — even on error. */
	g_backend->cs_deassert(xfer->cs_pin);

	/* Release CS lease (one-shot per transfer). */
	io_eval_cs_release(xfer->cs_pin);

	if (out_result) {
		if (hw_result == 0) {
			out_result->bus_result    = IO_EVAL_BUS_OK;
			out_result->bytes_written = xfer->tx_len;
			out_result->bytes_read    = xfer->rx_len;
		} else {
			out_result->bus_result    = IO_EVAL_BUS_ERROR;
			out_result->bytes_written = 0;
			out_result->bytes_read    = 0;
		}
	}

	return (hw_result == 0) ? SPI_EXPERT_OK : SPI_EXPERT_ERR_BUSY;
}

spi_expert_code_t spi_expert_release_cs(uint32_t cs_pin)
{
	if (io_eval_cs_release(cs_pin))
		return SPI_EXPERT_OK;
	return SPI_EXPERT_ERR_INVALID_ARG;
}

#endif /* BOARD_CLUE */
