#include "led.h"

#ifdef BOARD_CLUE

/*
 * CLUE LED module: NeoPixel (P0.16) via PWM0 EasyDMA, white (P0.10),
 * and red (P1.01) — all active-high.
 *
 * LED index mapping:
 *   LED1 (0) = red status    P1.01  (GPIO, active-high)
 *   LED2 (1) = NeoPixel      P0.16  (PWM0 EasyDMA, GRB, 1 pixel)
 *   LED3 (2) = white LEDs    P0.10  (GPIO, active-high)
 *
 * setColor() drives the NeoPixel when LED2 is "on".
 */

#include "custom_board.h"
#include "nrf_gpio.h"
#include "nrfx_pwm.h"
#include "neopixel_encode.h"

/* PWM duty sequence buffer — file-scope static → .bss (Data RAM, not flash). */
static nrf_pwm_values_common_t neopixel_seq_buf[NEOPIXEL_SEQ_LEN];

static nrfx_pwm_t       neopixel_pwm_inst = NRFX_PWM_INSTANCE(0);
static bool             neopixel_pwm_ready = false;

static void neopixel_init_pwm(void)
{
	if (neopixel_pwm_ready) return;

	nrfx_pwm_config_t cfg = {};
	cfg.output_pins[0] = CLUE_NEOPIXEL;
	cfg.output_pins[1] = NRFX_PWM_PIN_NOT_USED;
	cfg.output_pins[2] = NRFX_PWM_PIN_NOT_USED;
	cfg.output_pins[3] = NRFX_PWM_PIN_NOT_USED;
	cfg.irq_priority   = 6;
	cfg.base_clock     = NRF_PWM_CLK_16MHz;
	cfg.count_mode     = NRF_PWM_MODE_UP;
	cfg.top_value      = NEOPIXEL_COUNTERTOP;
	cfg.load_mode      = NRF_PWM_LOAD_COMMON;
	cfg.step_mode      = NRF_PWM_STEP_AUTO;

	nrfx_pwm_init(&neopixel_pwm_inst, &cfg, NULL);
	neopixel_pwm_ready = true;
}

static void neopixel_show(uint8_t r, uint8_t g, uint8_t b)
{
	neopixel_init_pwm();

	neopixel_encode_grb(neopixel_seq_buf, r, g, b);
	neopixel_encode_reset(neopixel_seq_buf + NEOPIXEL_BITS_PER_PIXEL,
	                      NEOPIXEL_RESET_PERIODS_MIN);

	nrf_pwm_sequence_t seq;
	seq.values.p_common = neopixel_seq_buf;
	seq.length          = NEOPIXEL_SEQ_LEN;
	seq.repeats         = 0;
	seq.end_delay       = 0;

	nrfx_pwm_simple_playback(&neopixel_pwm_inst, &seq, 1,
	                         NRFX_PWM_FLAG_STOP);
}

static void neopixel_off(void)
{
	neopixel_show(0, 0, 0);
}

static void ledColor_to_rgb(LedColor color, uint8_t *r, uint8_t *g, uint8_t *b)
{
	switch (color) {
		case RED:    *r = 255; *g = 0;   *b = 0;   break;
		case GREEN:  *r = 0;   *g = 255; *b = 0;   break;
		case BLUE:   *r = 0;   *g = 0;   *b = 255; break;
		case YELLOW: *r = 255; *g = 255; *b = 0;   break;
		case PURPLE: *r = 128; *g = 0;   *b = 128; break;
		case CYAN:   *r = 0;   *g = 255; *b = 255; break;
		default:     *r = 0;   *g = 0;   *b = 0;   break;
	}
}

LedModule::LedModule()
{
	nrf_gpio_cfg_output(LED_1);   /* P1.01 red */
	nrf_gpio_pin_clear(LED_1);
	nrf_gpio_cfg_output(LED_2);   /* P0.10 white */
	nrf_gpio_pin_clear(LED_2);

	this->state[LED1] = false;
	this->state[LED2] = false;
	this->state[LED3] = false;
	this->ledColor = BLUE;
	neopixel_off();
}

void LedModule::on(int num)
{
	if (num == LED1) {
		nrf_gpio_pin_set(LED_1);
		this->state[LED1] = true;
	}
	else if (num == LED3) {
		nrf_gpio_pin_set(LED_2);
		this->state[LED3] = true;
	}
	else {
		uint8_t r, g, b;
		ledColor_to_rgb(this->ledColor, &r, &g, &b);
		neopixel_show(r, g, b);
		this->state[LED2] = true;
	}
}

void LedModule::off(int num)
{
	if (num == LED1) {
		nrf_gpio_pin_clear(LED_1);
		this->state[LED1] = false;
	}
	else if (num == LED3) {
		nrf_gpio_pin_clear(LED_2);
		this->state[LED3] = false;
	}
	else {
		neopixel_off();
		this->state[LED2] = false;
	}
}

void LedModule::toggle(int num)
{
	if (num < 0 || num >= LED_COUNT) return;
	if (this->state[num]) this->off(num);
	else this->on(num);
}

bool LedModule::isOn(int num)
{
	if (num < 0 || num >= LED_COUNT) return false;
	return this->state[num];
}

void LedModule::setColor(LedColor color)
{
	this->ledColor = color;
	if (this->state[LED2]) {
		uint8_t r, g, b;
		ledColor_to_rgb(color, &r, &g, &b);
		neopixel_show(r, g, b);
	}
}

#else /* !BOARD_CLUE — PCA10059 / MDK_DONGLE bsp_board_led path */

LedModule::LedModule() {
	bsp_board_init(BSP_INIT_LEDS);
	this->state[LED1] = false;
	this->state[LED2] = false;
	this->ledColor = BLUE;
}

void LedModule::on(int num) {
	if (num == LED1) {
		bsp_board_led_on(LED1);
		this->state[LED1] = true;
	}
	else {
		if (this->ledColor == RED) {
			bsp_board_led_on(LED2_RED);
		}
		else if (this->ledColor == GREEN) {
			bsp_board_led_on(LED2_GREEN);
		}
		else if (this->ledColor == BLUE) {
			bsp_board_led_on(LED2_BLUE);
		}
		else if (this->ledColor == YELLOW) {
			bsp_board_led_on(LED2_RED);
			bsp_board_led_on(LED2_GREEN);
		}
		else if (this->ledColor == PURPLE) {
			bsp_board_led_on(LED2_RED);
			bsp_board_led_on(LED2_BLUE);
		}
		else if (this->ledColor == CYAN) {
			bsp_board_led_on(LED2_GREEN);
			bsp_board_led_on(LED2_BLUE);
		}
		this->state[LED2] = true;
	}
}

void LedModule::off(int num) {
	if (num == LED1) {
		bsp_board_led_off(LED1);
		this->state[LED1] = false;
	}
	else {
		bsp_board_led_off(LED2_RED);
		bsp_board_led_off(LED2_GREEN);
		bsp_board_led_off(LED2_BLUE);
		this->state[LED2] = false;
	}
}

void LedModule::toggle(int num) {
	if (num == LED1) {
		if (this->state[LED1]) this->off(LED1);
		else this->on(LED1);
	}
	else {
		if (this->state[LED2]) this->off(LED2);
		else this->on(LED2);
	}
}

bool LedModule::isOn(int num) {
	bool ret;
	if (num == LED1) ret = this->state[LED1];
	else ret = this->state[LED2];
	return ret;
}

void LedModule::setColor(LedColor color) {
	if (color != this->ledColor && this->state[LED2]) {
		this->off(LED2);
		this->ledColor = color;
		this->on(LED2);
	}
	this->ledColor = color;
}

#endif /* BOARD_CLUE */
