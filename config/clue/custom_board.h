#ifndef CUSTOM_BOARD_H
#define CUSTOM_BOARD_H

#ifdef __cplusplus
extern "C" {
#endif

#include "nrf_gpio.h"

#define LEDS_NUMBER    2

#define LED_1          NRF_GPIO_PIN_MAP(1,1)   /* P1.01 red status LED */
#define LED_2          NRF_GPIO_PIN_MAP(0,10)  /* P0.10 white LEDs */
#define LED_START      LED_1
#define LED_STOP       LED_2

#define LEDS_ACTIVE_STATE 1

#define LEDS_LIST { LED_1, LED_2 }
#define LEDS_INV_MASK  LEDS_MASK

#define BSP_LED_0      NRF_GPIO_PIN_MAP(1,1)
#define BSP_LED_1      NRF_GPIO_PIN_MAP(0,10)

#define BUTTONS_NUMBER 2

#define BUTTON_1       NRF_GPIO_PIN_MAP(1,2)   /* P1.02 Button A */
#define BUTTON_2       NRF_GPIO_PIN_MAP(1,10)  /* P1.10 Button B */
#define BUTTON_PULL    NRF_GPIO_PIN_PULLUP

#define BUTTONS_ACTIVE_STATE 0

#define BUTTONS_LIST { BUTTON_1, BUTTON_2 }

#define BSP_BUTTON_0   BUTTON_1
#define BSP_BUTTON_1   BUTTON_2

#define CLUE_BUTTON_DEBOUNCE_TICKS 50000UL

/*
 * UART pin assignments were RX_PIN_NUMBER=9, TX_PIN_NUMBER=10 on the
 * symlinked MDK config. Those pins are claimed on the CLUE board by
 * APDS9960 IRQ (P0.09) and the white status LEDs (P0.10, also LED_2).
 * The CLUE platform uses USB CDC ACM as its only WHAD/CLI transport, so
 * the legacy UART pin macros are intentionally NOT defined here. Any
 * future caller that needs UARTE must explicitly pick free pins.
 */

/* ST7789 TFT display (240x240) */
#define CLUE_TFT_SCK   NRF_GPIO_PIN_MAP(0,14)
#define CLUE_TFT_MOSI  NRF_GPIO_PIN_MAP(0,15)
#define CLUE_TFT_DC    NRF_GPIO_PIN_MAP(0,13)
#define CLUE_TFT_CS    NRF_GPIO_PIN_MAP(0,12)
#define CLUE_TFT_RST   NRF_GPIO_PIN_MAP(1,3)
#define CLUE_TFT_BL    NRF_GPIO_PIN_MAP(1,5)
#define CLUE_TFT_WIDTH  240
#define CLUE_TFT_HEIGHT 240
#define CLUE_TFT_X_OFFSET 80

/* SPIM instance for TFT */
#define CLUE_TFT_SPIM_INSTANCE  2

#define CLUE_I2C_SDA            NRF_GPIO_PIN_MAP(0,24)
#define CLUE_I2C_SCL            NRF_GPIO_PIN_MAP(0,25)
#define CLUE_I2C_INSTANCE       1
#define CLUE_I2C_FREQUENCY      NRF_TWIM_FREQ_400K

/* CLUE has no LF crystal; P0.00/P0.01 are PDM, NOT crystal pins. LFRC must be used. */
#define CLUE_PDM_DATA           NRF_GPIO_PIN_MAP(0,0)
#define CLUE_PDM_CLK            NRF_GPIO_PIN_MAP(0,1)
#define CLUE_PDM_INSTANCE       0
#define CLUE_PDM_GAIN_DEFAULT   0x50   /* +20 dB: register = (dB*2)+40 */

#define CLUE_QSPI_SCK           NRF_GPIO_PIN_MAP(0,19)
#define CLUE_QSPI_CSN           NRF_GPIO_PIN_MAP(0,20)
#define CLUE_QSPI_IO0           NRF_GPIO_PIN_MAP(0,17)
#define CLUE_QSPI_IO1           NRF_GPIO_PIN_MAP(0,22)
#define CLUE_QSPI_IO2           NRF_GPIO_PIN_MAP(0,23)
#define CLUE_QSPI_IO3           NRF_GPIO_PIN_MAP(0,21)

#define CLUE_NEOPIXEL           NRF_GPIO_PIN_MAP(0,16)

#define CLUE_SPEAKER            NRF_GPIO_PIN_MAP(1,0)
#define CLUE_SPEAKER_PWM_INSTANCE 0

#define CLUE_LSM6DS33_IRQ       NRF_GPIO_PIN_MAP(1,6)
#define CLUE_APDS9960_IRQ       NRF_GPIO_PIN_MAP(0,9)

#define CLUE_LSM6DS33_ADDR      0x6AU
#define CLUE_LIS3MDL_ADDR       0x1CU
#define CLUE_APDS9960_ADDR      0x39U
#define CLUE_SHT31D_ADDR        0x44U
#define CLUE_BMP280_ADDR        0x77U

#ifdef __cplusplus
}
#endif

#endif
