/*
 * i2c_twim_backend.cpp - TWIM1 production backend implementation.
 *
 * allow: SIZE_OK — single responsibility (wire nrfx_twim to i2cbus_backend_t
 * + provide recovery primitives). Splitting recovery from the bus backend
 * would scatter TWIM hardware knowledge across files.
 */
#include "i2c_twim_backend.h"

#ifdef BOARD_CLUE

#include <string.h>
#include "nrfx_twim.h"
#include "nrf_gpio.h"
#include "custom_board.h"
#include "timebase.h"

/* ---- Static instance + state ----------------------------------------- */

/* NRFX_TWIM_INSTANCE uses NRFX_CONCAT_3 which doesn't expand its arg,
 * so the literal instance id (1) must be passed, not CLUE_I2C_INSTANCE. */
static nrfx_twim_t s_twim = NRFX_TWIM_INSTANCE(1);
static bool        s_ready;

/* Pending transfer buffers (kept valid until completion).
 * i2cBus guarantees at most one outstanding hardware transfer at a time
 * (state==BUSY), so a single pair of pointer/length fields suffices. */
static uint8_t *s_cur_read_buf;
static size_t   s_cur_read_len;

/* ---- ISR event handler ----------------------------------------------- */

static void twim_event_handler(nrfx_twim_evt_t const *event,
                               void *                 /*p_context*/)
{
	i2cbus_result_t r;
	switch (event->type) {
	case NRFX_TWIM_EVT_DONE:
		r = I2CBUS_OK;
		break;
	case NRFX_TWIM_EVT_ADDRESS_NACK:
	case NRFX_TWIM_EVT_DATA_NACK:
		r = I2CBUS_ERR_NACK;
		break;
	case NRFX_TWIM_EVT_OVERRUN:
		r = I2CBUS_ERR_BUS;
		break;
	case NRFX_TWIM_EVT_BUS_ERROR:
	default:
		r = I2CBUS_ERR_BUS;
		break;
	}
	i2cbus_report_xfer_complete(r);
}

/* ---- start_xfer: enqueue a TWIM transfer ----------------------------- */

static int twim_start_xfer(uint8_t addr,
                           const uint8_t *write_buf, size_t write_len,
                           uint8_t *read_buf, size_t read_len)
{
	if (!s_ready) {
		return -1;
	}

	nrfx_twim_xfer_desc_t desc;
	memset(&desc, 0, sizeof desc);
	desc.address = addr;

	if (write_len > 0 && read_len > 0) {
		desc.type             = NRFX_TWIM_XFER_TXRX;
		desc.primary_length   = write_len;
		desc.secondary_length = read_len;
		desc.p_primary_buf    = (uint8_t *)write_buf;
		desc.p_secondary_buf  = read_buf;
	} else if (write_len > 0) {
		desc.type             = NRFX_TWIM_XFER_TX;
		desc.primary_length   = write_len;
		desc.p_primary_buf    = (uint8_t *)write_buf;
	} else if (read_len > 0) {
		desc.type             = NRFX_TWIM_XFER_RX;
		desc.primary_length   = read_len;
		desc.p_primary_buf    = read_buf;
	} else {
		/* Nothing to do — caller error. Report OK so the i2cBus FSM
		 * clears the active transfer without triggering recovery. */
		i2cbus_report_xfer_complete(I2CBUS_OK);
		return 0;
	}

	s_cur_read_buf = read_buf;
	s_cur_read_len = read_len;

	nrfx_err_t err = nrfx_twim_xfer(&s_twim, &desc, 0);
	if (err != NRFX_SUCCESS) {
		/* Could not start: report BUS error so i2cBus enters recovery. */
		i2cbus_report_xfer_complete(I2CBUS_ERR_BUS);
		return -1;
	}
	return 0;
}

/* ---- Recovery primitives (open-drain GPIO, never push-pull) --------- */

static void twim_release_sda(void)
{
	/* Switch SDA to input nopull so the slave can drive it. */
	nrf_gpio_cfg_input(CLUE_I2C_SDA, NRF_GPIO_PIN_NOPULL);
}

static void twim_toggle_scl(void)
{
	/* Open-drain: output low for one half-cycle, then release.
	 * Drive low → high-Z is the standard I2C bit-bang SCL toggle. */
	nrf_gpio_cfg_output(CLUE_I2C_SCL);
	nrf_gpio_pin_clear(CLUE_I2C_SCL);
	for (int i = 0; i < 50; i++) {  /* ~1us at 64MHz */
		__NOP();
	}
	__asm__ volatile("" ::: "memory");
	nrf_gpio_cfg_input(CLUE_I2C_SCL, NRF_GPIO_PIN_NOPULL);
	for (int i = 0; i < 50; i++) {
		__NOP();
	}
	__asm__ volatile("" ::: "memory");
}

static int twim_read_sda(void)
{
	return (int)nrf_gpio_pin_read(CLUE_I2C_SDA);
}

static void twim_gen_stop(void)
{
	/* Drive SDA low while SCL is released high → STOP condition. */
	nrf_gpio_cfg_output(CLUE_I2C_SDA);
	nrf_gpio_pin_clear(CLUE_I2C_SDA);
	for (int i = 0; i < 50; i++) { __NOP(); }
	__asm__ volatile("" ::: "memory");
	nrf_gpio_cfg_input(CLUE_I2C_SCL, NRF_GPIO_PIN_NOPULL);
	for (int i = 0; i < 50; i++) { __NOP(); }
	__asm__ volatile("" ::: "memory");
	nrf_gpio_cfg_input(CLUE_I2C_SDA, NRF_GPIO_PIN_NOPULL);
}

static void twim_reinit_twim(void)
{
	nrfx_twim_disable(&s_twim);
	nrfx_twim_enable(&s_twim);
}

static uint64_t twim_now_us(void)
{
	return timebase_now_us();
}

/* ---- Backend struct + getter ----------------------------------------- */

static const i2cbus_backend_t s_backend = {
	twim_start_xfer,
	twim_release_sda,
	twim_toggle_scl,
	twim_read_sda,
	twim_gen_stop,
	twim_reinit_twim,
	twim_now_us,
};

const i2cbus_backend_t *i2c_twim_backend_get(void)
{
	if (s_ready) {
		return &s_backend;
	}

	nrfx_twim_config_t cfg;
	memset(&cfg, 0, sizeof cfg);
	cfg.scl                = CLUE_I2C_SCL;
	cfg.sda                = CLUE_I2C_SDA;
	cfg.frequency          = (nrf_twim_frequency_t)CLUE_I2C_FREQUENCY;
	cfg.interrupt_priority = I2CBUS_IRQ_PRIORITY;
	cfg.hold_bus_uninit    = false;

	nrfx_err_t err = nrfx_twim_init(&s_twim, &cfg, twim_event_handler, NULL);
	if (err != NRFX_SUCCESS) {
		return NULL;
	}
	nrfx_twim_enable(&s_twim);
	s_ready = true;
	return &s_backend;
}

int i2c_twim_backend_is_ready(void)
{
	return s_ready ? 1 : 0;
}

#endif /* BOARD_CLUE */
