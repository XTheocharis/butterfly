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

/* Completion is polled from i2c_twim_poll() in the main loop, not via ISR. */

/* ---- start_xfer: enqueue a TWIM transfer ----------------------------- */

static int twim_start_xfer(uint8_t addr,
                           const uint8_t *write_buf, size_t write_len,
                           uint8_t *read_buf, size_t read_len)
{
	if (!s_ready) return -1;
	NRF_TWIM_Type *p = s_twim.p_twim;
	i2cbus_result_t r = I2CBUS_OK;

	p->ADDRESS = addr;
	p->TASKS_RESUME = 1;

	if (write_len > 0)
	{
		p->TXD.PTR = (uint32_t)write_buf;
		p->TXD.MAXCNT = (uint16_t)write_len;
		p->EVENTS_TXSTARTED = 0;
		p->EVENTS_LASTTX = 0;
		p->EVENTS_ERROR = 0;
		p->SHORTS = 0;
		p->TASKS_STARTTX = 1;
		for (int i = 0; i < 50000 && !p->EVENTS_LASTTX && !p->EVENTS_ERROR; i++) { __NOP(); }
		if (p->EVENTS_ERROR) { r = I2CBUS_ERR_NACK; goto done; }
		p->EVENTS_LASTTX = 0;
		p->TASKS_SUSPEND = 1;
		for (int i = 0; i < 5000 && !p->EVENTS_SUSPENDED; i++) { __NOP(); }
		p->EVENTS_SUSPENDED = 0;
	}

	if (read_len > 0)
	{
		p->RXD.PTR = (uint32_t)read_buf;
		p->RXD.MAXCNT = (uint16_t)read_len;
		p->EVENTS_RXSTARTED = 0;
		p->EVENTS_LASTRX = 0;
		p->EVENTS_ERROR = 0;
		p->SHORTS = 0;
		p->TASKS_RESUME = 1;
		p->TASKS_STARTRX = 1;
		for (int i = 0; i < 50000 && !p->EVENTS_LASTRX && !p->EVENTS_ERROR; i++) { __NOP(); }
		if (p->EVENTS_ERROR) { r = I2CBUS_ERR_NACK; goto done; }
		p->EVENTS_LASTRX = 0;
	}

done:
	p->TASKS_STOP = 1;
	for (int i = 0; i < 5000 && !p->EVENTS_STOPPED; i++) { __NOP(); }
	p->EVENTS_STOPPED = 0;
	uint32_t errsrc = p->ERRORSRC;
	p->EVENTS_ERROR = 0;
	p->ERRORSRC = errsrc;
	i2cbus_report_xfer_complete(r);
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
	NRF_TWIM_Type *p = s_twim.p_twim;
	p->TASKS_STOP = 1;
	for (int i = 0; i < 1000; i++) { __NOP(); }
	p->ENABLE = 0;
	p->ENABLE = TWIM_ENABLE_ENABLE_Enabled << TWIM_ENABLE_ENABLE_Pos;
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

	nrfx_err_t err = nrfx_twim_init(&s_twim, &cfg, NULL, NULL);
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

void i2c_twim_poll(void)
{
	if (!s_ready) return;
	NRF_TWIM_Type *p = s_twim.p_twim;
	if (p->EVENTS_ERROR)
	{
		uint32_t err = p->ERRORSRC;
		p->EVENTS_ERROR = 0;
		p->ERRORSRC = err;
		i2cbus_result_t r = (err & TWIM_ERRORSRC_DNACK_Msk) ?
			I2CBUS_ERR_NACK : I2CBUS_ERR_BUS;
		i2cbus_report_xfer_complete(r);
		return;
	}
	if (p->EVENTS_STOPPED)
	{
		p->EVENTS_STOPPED = 0;
		i2cbus_report_xfer_complete(I2CBUS_OK);
		return;
	}
	if (p->EVENTS_LASTRX)
	{
		p->EVENTS_LASTRX = 0;
		p->TASKS_STOP = 1;
		return;
	}
	if (p->EVENTS_LASTTX)
	{
		p->EVENTS_LASTTX = 0;
		return;
	}
}

#endif /* BOARD_CLUE */
