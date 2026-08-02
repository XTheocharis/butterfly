/*
 * neopixel_encode.h - WS2812B (NeoPixel) PWM duty cycle encoder for nRF52.
 *
 * Pure C, no SDK dependencies. Includable by firmware (led.cpp) and host
 * tests (test_neopixel.c).
 *
 * PWM configuration:
 *   - Instance: PWM0, base clock 16 MHz, COUNTERTOP 20
 *   - Load mode: NRF_PWM_LOAD_COMMON (single channel drives P0.16)
 *   - Period: 20 / 16 MHz = 1.25 us per bit
 *
 * Duty cycle value encoding (15-bit compare + bit-15 polarity):
 *   - Polarity bit (15) = 1: output starts HIGH, goes LOW at compare.
 *     This gives compare counts of HIGH time per period.
 *   - 0-bit: compare = 5  -> 0.3125 us HIGH (WS2812 T0H 0.25-0.55 us)
 *   - 1-bit: compare = 13 -> 0.8125 us HIGH (WS2812 T1H 0.65-0.95 us)
 *   - Reset: compare = 0  -> entire period LOW
 *
 * Reset requirement: >= 50 us low (WS2812B datasheet).
 * 80 us / 1.25 us = 64 periods minimum (conservative margin).
 *
 * GRB color order: green byte first, then red, then blue.
 * MSB-first within each byte.
 */
#ifndef NEOPIXEL_ENCODE_H
#define NEOPIXEL_ENCODE_H

#include <stdint.h>

/* PWM timing constants. */
#define NEOPIXEL_COUNTERTOP         20u
#define NEOPIXEL_CLK_HZ             16000000u
#define NEOPIXEL_PERIOD_US          (double)NEOPIXEL_COUNTERTOP / (double)NEOPIXEL_CLK_HZ * 1e6

/*
 * 16-bit PWM duty cycle values for each bit type.
 * Bit 15 (0x8000) = inverted polarity: output HIGH for `compare` counts.
 */
#define NEOPIXEL_DUTY_0             ((uint16_t)(0x8000u | 5u))
#define NEOPIXEL_DUTY_1             ((uint16_t)(0x8000u | 13u))
#define NEOPIXEL_DUTY_RESET         ((uint16_t)(0x8000u | 0u))

#define NEOPIXEL_BITS_PER_PIXEL     24u
#define NEOPIXEL_RESET_PERIODS_MIN  64u   /* 80 us / 1.25 us */
#define NEOPIXEL_SEQ_LEN            (NEOPIXEL_BITS_PER_PIXEL + NEOPIXEL_RESET_PERIODS_MIN)

/*
 * Encode one WS2812B pixel into a PWM duty value buffer.
 * GRB order, MSB-first within each byte.
 * buf must hold at least NEOPIXEL_BITS_PER_PIXEL entries.
 * Returns NEOPIXEL_BITS_PER_PIXEL (always 24).
 */
static inline uint32_t neopixel_encode_grb(uint16_t *buf,
                                           uint8_t r, uint8_t g, uint8_t b)
{
    const uint8_t grb[3] = { g, r, b };
    uint32_t idx = 0;
    for (uint32_t byte_n = 0; byte_n < 3u; byte_n++) {
        for (int bit = 7; bit >= 0; bit--) {
            buf[idx++] = (grb[byte_n] & (uint8_t)(1u << bit))
                       ? NEOPIXEL_DUTY_1
                       : NEOPIXEL_DUTY_0;
        }
    }
    return idx;
}

/*
 * Fill reset (all-low) values after pixel data.
 * buf must hold at least `periods` entries.
 * Returns the number of entries written.
 */
static inline uint32_t neopixel_encode_reset(uint16_t *buf, uint32_t periods)
{
    for (uint32_t i = 0; i < periods; i++) {
        buf[i] = NEOPIXEL_DUTY_RESET;
    }
    return periods;
}

#endif /* NEOPIXEL_ENCODE_H */
