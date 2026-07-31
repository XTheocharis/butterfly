#include "display.h"

#ifdef BOARD_CLUE

#include <string.h>
#include "nrf.h"
#include "nrf_delay.h"
#include "nrfx_spim.h"
#include "boards.h"
#include "font.h"

#define DC_PIN      CLUE_TFT_DC
#define CS_PIN      CLUE_TFT_CS
#define RST_PIN     CLUE_TFT_RST
#define BL_PIN      CLUE_TFT_BL
#define SCK_PIN     CLUE_TFT_SCK
#define MOSI_PIN    CLUE_TFT_MOSI

static const nrfx_spim_t spi = NRFX_SPIM_INSTANCE(CLUE_TFT_SPIM_INSTANCE);
static bool spi_ready;

static void spi_init(void) {
    nrf_gpio_cfg_output(CS_PIN);
    nrf_gpio_cfg_output(DC_PIN);
    nrf_gpio_cfg_output(RST_PIN);
    nrf_gpio_cfg_output(BL_PIN);

    nrf_gpio_pin_set(CS_PIN);
    nrf_gpio_pin_set(DC_PIN);
    nrf_gpio_pin_set(RST_PIN);
    nrf_gpio_pin_clear(BL_PIN);

    nrfx_spim_config_t config = NRFX_SPIM_DEFAULT_CONFIG;
    config.sck_pin = SCK_PIN;
    config.mosi_pin = MOSI_PIN;
    config.miso_pin = NRFX_SPIM_PIN_NOT_USED;
    config.ss_pin = NRFX_SPIM_PIN_NOT_USED;
    config.frequency = NRF_SPIM_FREQ_4M;
    config.mode = NRF_SPIM_MODE_0;
    config.bit_order = NRF_SPIM_BIT_ORDER_MSB_FIRST;

    spi_ready = (nrfx_spim_init(&spi, &config, NULL, NULL) == NRFX_SUCCESS);
    if (!spi_ready) {
        // Keep the USB/radio firmware alive if the peripheral is unavailable.
        // The red LED is also a visible indication of an init failure.
        nrf_gpio_pin_set(LED_1);
    }
}

static void spi_write(const uint8_t *data, int len) {
    if (!spi_ready || len <= 0) {
        return;
    }

    nrfx_spim_xfer_desc_t transfer = NRFX_SPIM_XFER_TX(data, (size_t)len);
    if (nrfx_spim_xfer(&spi, &transfer, 0) != NRFX_SUCCESS) {
        spi_ready = false;
        nrf_gpio_pin_set(LED_1);
    }
}

void DisplayModule::select()    { nrf_gpio_pin_clear(CS_PIN); }
void DisplayModule::deselect()  { nrf_gpio_pin_set(CS_PIN); }
void DisplayModule::dcCommand() { nrf_gpio_pin_clear(DC_PIN); }
void DisplayModule::dcData()    { nrf_gpio_pin_set(DC_PIN); }

void DisplayModule::writeCommand(uint8_t cmd) {
    dcCommand();
    select();
    spi_write(&cmd, 1);
    deselect();
}

void DisplayModule::writeCommand(uint8_t cmd, const uint8_t *data, int len) {
    dcCommand();
    select();
    spi_write(&cmd, 1);
    deselect();
    if (len > 0) {
        uint8_t buf[16];
        if (len > 16) len = 16;
        memcpy(buf, data, len);
        dcData();
        select();
        spi_write(buf, len);
        deselect();
    }
}

void DisplayModule::setAddrWindow(int x, int y, int w, int h) {
    int x1 = x + 80;
    int x2 = x + w - 1 + 80;
    int y1 = y;
    int y2 = y + h - 1;

    uint8_t col[4] = {(uint8_t)(x1 >> 8), (uint8_t)x1, (uint8_t)(x2 >> 8), (uint8_t)x2};
    uint8_t row[4] = {(uint8_t)(y1 >> 8), (uint8_t)y1, (uint8_t)(y2 >> 8), (uint8_t)y2};

    writeCommand(0x2A, col, 4);
    writeCommand(0x2B, row, 4);
    writeCommand(0x2C);
}

DisplayModule::DisplayModule() {
}

void DisplayModule::init() {
    spi_init();

    nrf_gpio_pin_clear(RST_PIN);
    nrf_delay_ms(10);
    nrf_gpio_pin_set(RST_PIN);
    nrf_delay_ms(120);

    static const uint8_t swreset[] = {};
    static const uint8_t slpout[]  = {};
    static const uint8_t madctl[]  = {0xA0};
    static const uint8_t colmod[]  = {0x55};
    static const uint8_t invon[]   = {};
    static const uint8_t noron[]   = {};
    static const uint8_t dispon[]  = {};

    writeCommand(0x01, swreset, 0);
    nrf_delay_ms(150);
    writeCommand(0x11, slpout, 0);
    nrf_delay_ms(255);
    writeCommand(0x36, madctl, 1);
    writeCommand(0x3A, colmod, 1);
    writeCommand(0x21, invon, 0);
    nrf_delay_ms(10);
    writeCommand(0x13, noron, 0);
    nrf_delay_ms(10);
    writeCommand(0x29, dispon, 0);
    nrf_delay_ms(255);

    setBacklight(true);
    fill(COLOR_BLACK);
}

void DisplayModule::setBacklight(bool on) {
    if (on) nrf_gpio_pin_set(BL_PIN);
    else    nrf_gpio_pin_clear(BL_PIN);
}

void DisplayModule::fill(uint16_t color) {
    fillRect(0, 0, CLUE_TFT_WIDTH, CLUE_TFT_HEIGHT, color);
}

void DisplayModule::fillRect(int x, int y, int w, int h, uint16_t color) {
    setAddrWindow(x, y, w, h);

    dcData();
    select();

    int total = w * h;
    uint8_t hi = color >> 8;
    uint8_t lo = color & 0xFF;
    uint8_t buf[64];
    for (int i = 0; i < 32; i++) {
        buf[i * 2]     = hi;
        buf[i * 2 + 1] = lo;
    }
    while (total > 0) {
        int chunk = total < 32 ? total : 32;
        spi_write(buf, chunk * 2);
        total -= chunk;
    }
    deselect();
}

void DisplayModule::drawText(int x, int y, const char *str, uint16_t fg, uint16_t bg) {
    int cx = x;
    for (const char *p = str; *p; p++) {
        const uint8_t *glyph = font_get(*p);
        uint8_t buf[FONT_WIDTH * FONT_HEIGHT * 2];
        for (int row = 0; row < FONT_HEIGHT; row++) {
            for (int col = 0; col < FONT_WIDTH; col++) {
                uint8_t bits = glyph[col];
                uint16_t color = (bits & (1 << row)) ? fg : bg;
                int idx = (row * FONT_WIDTH + col) * 2;
                buf[idx] = color >> 8;
                buf[idx + 1] = color & 0xFF;
            }
        }
        setAddrWindow(cx, y, FONT_WIDTH, FONT_HEIGHT);
        dcData();
        select();
        spi_write(buf, sizeof(buf));
        deselect();
        cx += FONT_CELL_W;
    }
}

void DisplayModule::drawText(int x, int y, const char *str, uint16_t fg) {
    drawText(x, y, str, fg, COLOR_BLACK);
}

#endif
