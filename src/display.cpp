#include "display.h"

#ifdef BOARD_CLUE

#include <string.h>

#ifndef DISPLAY_HOST_TEST
#include "nrf.h"
#include "nrf_delay.h"
#include "nrfx_spim.h"
#include "boards.h"
#endif

#include "font.h"

#ifdef DISPLAY_HOST_TEST
#define CLUE_TFT_WIDTH  240
#define CLUE_TFT_HEIGHT 240
#define CLUE_TFT_X_OFFSET 80
#define LED_1 1
#else
#define DC_PIN      CLUE_TFT_DC
#define CS_PIN      CLUE_TFT_CS
#define RST_PIN     CLUE_TFT_RST
#define BL_PIN      CLUE_TFT_BL
#define SCK_PIN     CLUE_TFT_SCK
#define MOSI_PIN    CLUE_TFT_MOSI

static const nrfx_spim_t spi = NRFX_SPIM_INSTANCE(CLUE_TFT_SPIM_INSTANCE);
static bool spi_ready;

static void spim_handler(nrfx_spim_evt_t const *event, void *context) {
    DisplayModule *display = static_cast<DisplayModule *>(context);
    if (display != NULL) {
        display->handleTransferComplete(event->type == NRFX_SPIM_EVENT_DONE);
    }
}
#endif

static void display_delay_ms(uint32_t ms) {
#ifndef DISPLAY_HOST_TEST
    nrf_delay_ms(ms);
#else
    (void)ms;
#endif
}

static bool positive_rect(const DisplayRect &rect) {
    return rect.w > 0 && rect.h > 0;
}

DisplayModule::DisplayModule() {
    memset(m_widgets, 0, sizeof m_widgets);
    memset(m_dirty, 0, sizeof m_dirty);
    memset(m_repaint, 0, sizeof m_repaint);
    memset(m_jobs, 0, sizeof m_jobs);
    memset(&m_current_job, 0, sizeof m_current_job);
    memset(m_dma_buf, 0, sizeof m_dma_buf);

    m_dirty_count = 0;
    m_job_head = 0;
    m_job_count = 0;
    m_active_page = 0;
    m_next_generation = 1;
    m_last_refresh_ms = 0;
    m_has_refreshed = false;
    m_sync_allowed = false;
    m_rendering = false;
    m_clip_active = false;
    m_render_clip = { 0, 0, 0, 0 };
    m_has_current_job = false;
    m_transfer_phase = TRANSFER_IDLE;
    m_pending_phase = TRANSFER_IDLE;
    m_transfer_pending = false;
    m_pending_payload_bytes = 0;
    m_pending_payload_pixels = 0;
    m_spi_sink = { NULL, NULL };
    m_scheduler_gate = { NULL, NULL };

    for (uint8_t i = 0; i < DISPLAY_MAX_WIDGETS; ++i) {
        m_widgets[i].generation = 1;
    }
}

#ifndef DISPLAY_HOST_TEST
static bool spi_init(DisplayModule *display) {
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

    spi_ready = (nrfx_spim_init(&spi, &config, spim_handler, display) == NRFX_SUCCESS);
    if (!spi_ready) {
        nrf_gpio_pin_set(LED_1);
    }
    return spi_ready;
}
#else
static bool spi_init(DisplayModule *display) {
    (void)display;
    return true;
}
#endif

void DisplayModule::select() {
#ifndef DISPLAY_HOST_TEST
    nrf_gpio_pin_clear(CS_PIN);
#endif
}

void DisplayModule::deselect() {
#ifndef DISPLAY_HOST_TEST
    nrf_gpio_pin_set(CS_PIN);
#endif
}

void DisplayModule::dcCommand() {
#ifndef DISPLAY_HOST_TEST
    nrf_gpio_pin_clear(DC_PIN);
#endif
}

void DisplayModule::dcData() {
#ifndef DISPLAY_HOST_TEST
    nrf_gpio_pin_set(DC_PIN);
#endif
}

bool DisplayModule::beginSpiTransfer(DisplaySpiPhase phase, const uint8_t *data,
                                     uint16_t len, bool nonblocking) {
    if (len == 0 || len > DISPLAY_MAX_PIXEL_BYTES) {
        return false;
    }
    memcpy(m_dma_buf, data, len);

    if (m_spi_sink.transfer != NULL) {
        bool ok = m_spi_sink.transfer(m_spi_sink.context, phase, m_dma_buf, len,
                                      nonblocking);
        if (ok && nonblocking) {
            m_transfer_pending = true;
            m_pending_phase = m_transfer_phase;
        }
        return ok;
    }

#ifdef DISPLAY_HOST_TEST
    (void)phase;
    (void)nonblocking;
    return false;
#else
    if (!spi_ready) {
        return false;
    }

    if (phase == DISPLAY_SPI_COMMAND) {
        dcCommand();
    } else {
        dcData();
    }
    select();

    nrfx_spim_xfer_desc_t transfer = NRFX_SPIM_XFER_TX(m_dma_buf, (size_t)len);
    if (nonblocking) {
        m_transfer_pending = true;
        m_pending_phase = m_transfer_phase;
        nrfx_err_t err = nrfx_spim_xfer(&spi, &transfer, 0);
        if (err != NRFX_SUCCESS) {
            m_transfer_pending = false;
            m_pending_phase = TRANSFER_IDLE;
            deselect();
            spi_ready = false;
            nrf_gpio_pin_set(LED_1);
            return false;
        }
        return true;
    }

    spi.p_reg->EVENTS_END = 0;
    nrfx_err_t err = nrfx_spim_xfer(&spi, &transfer,
                                    NRFX_SPIM_FLAG_NO_XFER_EVT_HANDLER);
    if (err != NRFX_SUCCESS) {
        deselect();
        spi_ready = false;
        nrf_gpio_pin_set(LED_1);
        return false;
    }
    while (spi.p_reg->EVENTS_END == 0) {
    }
    deselect();
    return true;
#endif
}

void DisplayModule::finishSpiTransfer() {
    if (m_spi_sink.transfer == NULL) {
        deselect();
    }
}

void DisplayModule::writeCommand(uint8_t cmd) {
    (void)beginSpiTransfer(DISPLAY_SPI_COMMAND, &cmd, 1, false);
}

void DisplayModule::writeCommand(uint8_t cmd, const uint8_t *data, int len) {
    writeCommand(cmd);
    if (len <= 0 || data == NULL) {
        return;
    }
    int bounded_len = len;
    if (bounded_len > (int)DISPLAY_MAX_PIXEL_BYTES) {
        bounded_len = (int)DISPLAY_MAX_PIXEL_BYTES;
    }
    (void)beginSpiTransfer(DISPLAY_SPI_DATA, data, (uint16_t)bounded_len, false);
}

void DisplayModule::setAddrWindow(int x, int y, int w, int h) {
    int x1 = x + CLUE_TFT_X_OFFSET;
    int x2 = x + w - 1 + CLUE_TFT_X_OFFSET;
    int y1 = y;
    int y2 = y + h - 1;

    uint8_t col[4] = {
        (uint8_t)((uint16_t)x1 >> 8), (uint8_t)x1,
        (uint8_t)((uint16_t)x2 >> 8), (uint8_t)x2,
    };
    uint8_t row[4] = {
        (uint8_t)((uint16_t)y1 >> 8), (uint8_t)y1,
        (uint8_t)((uint16_t)y2 >> 8), (uint8_t)y2,
    };

    writeCommand(0x2A, col, 4);
    writeCommand(0x2B, row, 4);
    writeCommand(0x2C);
}

void DisplayModule::init() {
    m_sync_allowed = true;
    (void)spi_init(this);

#ifndef DISPLAY_HOST_TEST
    nrf_gpio_pin_clear(RST_PIN);
#endif
    display_delay_ms(10);
#ifndef DISPLAY_HOST_TEST
    nrf_gpio_pin_set(RST_PIN);
#endif
    display_delay_ms(120);

    static const uint8_t madctl[]  = { 0xA0 };
    static const uint8_t colmod[]  = { 0x55 };
    static const uint8_t frctrl2[] = { 0x01 };

    writeCommand(0x01);
    display_delay_ms(150);
    writeCommand(0x11);
    display_delay_ms(255);
    writeCommand(0x36, madctl, 1);
    writeCommand(0x3A, colmod, 1);
    writeCommand(0xC6, frctrl2, 1);
    writeCommand(0x21);
    display_delay_ms(10);
    writeCommand(0x13);
    display_delay_ms(10);
    writeCommand(0x29);
    display_delay_ms(255);

    setBacklight(true);
    fill(COLOR_BLACK);
}

void DisplayModule::endBootSplash() {
    m_sync_allowed = false;
}

void DisplayModule::setBacklight(bool on) {
#ifndef DISPLAY_HOST_TEST
    if (on) {
        nrf_gpio_pin_set(BL_PIN);
    } else {
        nrf_gpio_pin_clear(BL_PIN);
    }
#else
    (void)on;
#endif
}

bool DisplayModule::clipToScreen(const DisplayRect &rect, DisplayRect *out) const {
    if (!positive_rect(rect)) {
        return false;
    }

    int x1 = rect.x < 0 ? 0 : rect.x;
    int y1 = rect.y < 0 ? 0 : rect.y;
    int x2 = (int)rect.x + (int)rect.w;
    int y2 = (int)rect.y + (int)rect.h;
    if (x2 > CLUE_TFT_WIDTH) {
        x2 = CLUE_TFT_WIDTH;
    }
    if (y2 > CLUE_TFT_HEIGHT) {
        y2 = CLUE_TFT_HEIGHT;
    }
    if (x1 >= x2 || y1 >= y2) {
        return false;
    }

    out->x = (int16_t)x1;
    out->y = (int16_t)y1;
    out->w = (int16_t)(x2 - x1);
    out->h = (int16_t)(y2 - y1);
    return true;
}

bool DisplayModule::intersectRects(const DisplayRect &a, const DisplayRect &b,
                                   DisplayRect *out) const {
    if (!positive_rect(a) || !positive_rect(b)) {
        return false;
    }

    int x1 = a.x > b.x ? a.x : b.x;
    int y1 = a.y > b.y ? a.y : b.y;
    int ax2 = (int)a.x + (int)a.w;
    int ay2 = (int)a.y + (int)a.h;
    int bx2 = (int)b.x + (int)b.w;
    int by2 = (int)b.y + (int)b.h;
    int x2 = ax2 < bx2 ? ax2 : bx2;
    int y2 = ay2 < by2 ? ay2 : by2;
    if (x1 >= x2 || y1 >= y2) {
        return false;
    }

    out->x = (int16_t)x1;
    out->y = (int16_t)y1;
    out->w = (int16_t)(x2 - x1);
    out->h = (int16_t)(y2 - y1);
    return true;
}

DisplayRect DisplayModule::unionRects(const DisplayRect &a,
                                      const DisplayRect &b) const {
    int x1 = a.x < b.x ? a.x : b.x;
    int y1 = a.y < b.y ? a.y : b.y;
    int ax2 = (int)a.x + (int)a.w;
    int ay2 = (int)a.y + (int)a.h;
    int bx2 = (int)b.x + (int)b.w;
    int by2 = (int)b.y + (int)b.h;
    int x2 = ax2 > bx2 ? ax2 : bx2;
    int y2 = ay2 > by2 ? ay2 : by2;
    return { (int16_t)x1, (int16_t)y1, (int16_t)(x2 - x1),
             (int16_t)(y2 - y1) };
}

bool DisplayModule::rectsTouchOrOverlap(const DisplayRect &a,
                                        const DisplayRect &b) const {
    int ax2 = (int)a.x + (int)a.w;
    int ay2 = (int)a.y + (int)a.h;
    int bx2 = (int)b.x + (int)b.w;
    int by2 = (int)b.y + (int)b.h;
    return !((ax2 < b.x) || (bx2 < a.x) || (ay2 < b.y) || (by2 < a.y));
}

void DisplayModule::mergeDirtyRect(const DisplayRect &rect) {
    for (uint8_t i = 0; i < m_dirty_count; ++i) {
        if (rectsTouchOrOverlap(m_dirty[i], rect)) {
            m_dirty[i] = unionRects(m_dirty[i], rect);
            return;
        }
    }

    if (m_dirty_count < DISPLAY_MAX_DIRTY_RECTS) {
        m_dirty[m_dirty_count++] = rect;
        return;
    }

    m_dirty[0] = unionRects(m_dirty[0], rect);
}

void DisplayModule::markDirty(const DisplayRect &rect) {
    DisplayRect clipped;
    if (clipToScreen(rect, &clipped)) {
        mergeDirtyRect(clipped);
    }
}

bool DisplayModule::enqueueSolidRect(const DisplayRect &rect, uint16_t color) {
    DisplayRect clipped;
    if (!clipToScreen(rect, &clipped)) {
        return false;
    }
    if (m_clip_active && !intersectRects(clipped, m_render_clip, &clipped)) {
        return false;
    }

    if (m_sync_allowed) {
        writeSolidRectSync(clipped, color);
        return true;
    }

    if (m_job_count >= DISPLAY_MAX_DRAW_JOBS) {
        markDirty(clipped);
        return false;
    }

    uint8_t slot = (uint8_t)((m_job_head + m_job_count) % DISPLAY_MAX_DRAW_JOBS);
    m_jobs[slot].kind = DRAW_JOB_SOLID;
    m_jobs[slot].rect = clipped;
    m_jobs[slot].color = color;
    m_jobs[slot].pixel_bytes = 0;
    m_jobs[slot].pixels_sent = 0;
    m_job_count++;

    if (!m_rendering) {
        (void)advanceTransfers(DISPLAY_DEFAULT_QUANTUM);
    }
    return true;
}

bool DisplayModule::enqueuePixelRect(const DisplayRect &rect, const uint8_t *pixels,
                                     uint16_t pixel_bytes) {
    DisplayRect clipped;
    if (!clipToScreen(rect, &clipped) || pixels == NULL || pixel_bytes == 0 ||
        pixel_bytes > DISPLAY_MAX_PIXEL_BYTES) {
        return false;
    }
    if (m_job_count >= DISPLAY_MAX_DRAW_JOBS) {
        markDirty(clipped);
        return false;
    }

    uint8_t slot = (uint8_t)((m_job_head + m_job_count) % DISPLAY_MAX_DRAW_JOBS);
    m_jobs[slot].kind = DRAW_JOB_PIXELS;
    m_jobs[slot].rect = clipped;
    m_jobs[slot].color = 0;
    m_jobs[slot].pixel_bytes = pixel_bytes;
    m_jobs[slot].pixels_sent = 0;
    memcpy(m_jobs[slot].pixels, pixels, pixel_bytes);
    m_job_count++;

    if (!m_rendering) {
        (void)advanceTransfers(DISPLAY_DEFAULT_QUANTUM);
    }
    return true;
}

bool DisplayModule::dequeueJob(DrawJob *job) {
    if (m_job_count == 0 || job == NULL) {
        return false;
    }
    *job = m_jobs[m_job_head];
    m_jobs[m_job_head].kind = DRAW_JOB_NONE;
    m_job_head = (uint8_t)((m_job_head + 1U) % DISPLAY_MAX_DRAW_JOBS);
    m_job_count--;
    return true;
}

bool DisplayModule::hasQueuedWork() const {
    return m_transfer_pending || m_has_current_job || m_job_count > 0;
}

void DisplayModule::writeSolidRectSync(const DisplayRect &rect, uint16_t color) {
    setAddrWindow(rect.x, rect.y, rect.w, rect.h);

    int total = (int)rect.w * (int)rect.h;
    uint8_t hi = (uint8_t)(color >> 8);
    uint8_t lo = (uint8_t)(color & 0xFFU);
    uint8_t buf[DISPLAY_TILE_PIXELS * 2U];
    for (uint8_t i = 0; i < DISPLAY_TILE_PIXELS; ++i) {
        buf[i * 2U] = hi;
        buf[(i * 2U) + 1U] = lo;
    }
    while (total > 0) {
        int chunk = total < (int)DISPLAY_TILE_PIXELS ? total : (int)DISPLAY_TILE_PIXELS;
        (void)beginSpiTransfer(DISPLAY_SPI_DATA, buf, (uint16_t)(chunk * 2), false);
        total -= chunk;
    }
}

void DisplayModule::writePixelsSync(const DisplayRect &rect, const uint8_t *pixels,
                                    uint16_t pixel_bytes) {
    setAddrWindow(rect.x, rect.y, rect.w, rect.h);
    uint16_t sent = 0;
    while (sent < pixel_bytes) {
        uint16_t remaining = (uint16_t)(pixel_bytes - sent);
        uint16_t chunk = remaining < DISPLAY_MAX_PIXEL_BYTES ? remaining : DISPLAY_MAX_PIXEL_BYTES;
        (void)beginSpiTransfer(DISPLAY_SPI_DATA, &pixels[sent], chunk, false);
        sent = (uint16_t)(sent + chunk);
    }
}

void DisplayModule::fill(uint16_t color) {
    fillRect(0, 0, CLUE_TFT_WIDTH, CLUE_TFT_HEIGHT, color);
}

void DisplayModule::fillRect(int x, int y, int w, int h, uint16_t color) {
    DisplayRect rect = { (int16_t)x, (int16_t)y, (int16_t)w, (int16_t)h };
    (void)enqueueSolidRect(rect, color);
}

void DisplayModule::drawGlyph(int x, int y, char c, uint16_t fg, uint16_t bg) {
    DisplayRect glyph_rect = { (int16_t)x, (int16_t)y, FONT_WIDTH, FONT_HEIGHT };
    DisplayRect clipped;
    if (!clipToScreen(glyph_rect, &clipped)) {
        return;
    }
    if (m_clip_active && !intersectRects(clipped, m_render_clip, &clipped)) {
        return;
    }

    const uint8_t *glyph = font_get(c);
    uint8_t pixels[DISPLAY_MAX_PIXEL_BYTES];
    uint16_t idx = 0;
    for (int row = 0; row < clipped.h; ++row) {
        int source_row = (int)clipped.y - y + row;
        for (int col = 0; col < clipped.w; ++col) {
            int source_col = (int)clipped.x - x + col;
            uint8_t bits = glyph[source_col];
            uint16_t color = ((bits & (uint8_t)(1U << source_row)) != 0U) ? fg : bg;
            pixels[idx++] = (uint8_t)(color >> 8);
            pixels[idx++] = (uint8_t)(color & 0xFFU);
        }
    }

    if (m_sync_allowed) {
        writePixelsSync(clipped, pixels, idx);
        return;
    }
    (void)enqueuePixelRect(clipped, pixels, idx);
}

void DisplayModule::drawText(int x, int y, const char *str, uint16_t fg,
                             uint16_t bg) {
    int cx = x;
    if (str == NULL) {
        return;
    }
    for (const char *p = str; *p != '\0'; ++p) {
        drawGlyph(cx, y, *p, fg, bg);
        cx += FONT_CELL_W;
    }
}

void DisplayModule::drawText(int x, int y, const char *str, uint16_t fg) {
    drawText(x, y, str, fg, COLOR_BLACK);
}

WidgetHandle DisplayModule::makeHandle(uint8_t index) const {
    return (WidgetHandle)((m_widgets[index].generation << 4U) | (index & 0x0FU));
}

bool DisplayModule::resolveHandle(WidgetHandle handle, uint8_t *index) const {
    if (handle == DISPLAY_WIDGET_INVALID || index == NULL) {
        return false;
    }
    uint8_t slot = (uint8_t)(handle & 0x0FU);
    uint8_t generation = (uint8_t)(handle >> 4U);
    if (slot >= DISPLAY_MAX_WIDGETS) {
        return false;
    }
    if (!m_widgets[slot].active || m_widgets[slot].generation != generation) {
        return false;
    }
    *index = slot;
    return true;
}

WidgetHandle DisplayModule::registerWidget(const DisplayWidgetConfig &config) {
    DisplayRect clipped;
    if (config.render == NULL || config.page >= DISPLAY_MAX_PAGES ||
        !clipToScreen(config.bounds, &clipped)) {
        return DISPLAY_WIDGET_INVALID;
    }

    for (uint8_t i = 0; i < DISPLAY_MAX_WIDGETS; ++i) {
        if (!m_widgets[i].active) {
            m_widgets[i].bounds = clipped;
            m_widgets[i].render = config.render;
            m_widgets[i].context = config.context;
            m_widgets[i].z = config.z;
            m_widgets[i].page_mask = (uint8_t)(1U << config.page);
            m_widgets[i].generation = m_next_generation;
            m_next_generation = (uint8_t)(m_next_generation + 1U);
            if (m_next_generation == 0 || m_next_generation > 15U) {
                m_next_generation = 1;
            }
            m_widgets[i].active = true;
            if (config.page == m_active_page) {
                markDirty(clipped);
            }
            return makeHandle(i);
        }
    }
    return DISPLAY_WIDGET_INVALID;
}

void DisplayModule::invalidateWidget(const WidgetSlot &slot) {
    if ((slot.page_mask & (uint8_t)(1U << m_active_page)) != 0U) {
        markDirty(slot.bounds);
    }
}

bool DisplayModule::unregisterWidget(WidgetHandle handle) {
    uint8_t index = 0;
    if (!resolveHandle(handle, &index)) {
        return false;
    }
    WidgetSlot old = m_widgets[index];
    m_widgets[index].active = false;
    m_widgets[index].render = NULL;
    m_widgets[index].context = NULL;
    m_widgets[index].page_mask = 0;
    m_widgets[index].generation = (uint8_t)(m_widgets[index].generation + 1U);
    if (m_widgets[index].generation == 0 || m_widgets[index].generation > 15U) {
        m_widgets[index].generation = 1;
    }
    invalidateWidget(old);
    return true;
}

bool DisplayModule::addWidgetToPage(uint8_t page, WidgetHandle handle) {
    uint8_t index = 0;
    if (page >= DISPLAY_MAX_PAGES || !resolveHandle(handle, &index)) {
        return false;
    }
    m_widgets[index].page_mask |= (uint8_t)(1U << page);
    if (page == m_active_page) {
        markDirty(m_widgets[index].bounds);
    }
    return true;
}

void DisplayModule::clearPage(uint8_t page) {
    if (page >= DISPLAY_MAX_PAGES) {
        return;
    }
    uint8_t mask = (uint8_t)~(uint8_t)(1U << page);
    for (uint8_t i = 0; i < DISPLAY_MAX_WIDGETS; ++i) {
        if (m_widgets[i].active) {
            m_widgets[i].page_mask &= mask;
        }
    }
    if (page == m_active_page) {
        markDirty({ 0, 0, CLUE_TFT_WIDTH, CLUE_TFT_HEIGHT });
    }
}

bool DisplayModule::showPage(uint8_t page) {
    if (page >= DISPLAY_MAX_PAGES || page == m_active_page) {
        return false;
    }
    m_active_page = page;
    markDirty({ 0, 0, CLUE_TFT_WIDTH, CLUE_TFT_HEIGHT });
    return true;
}

bool DisplayModule::flushDirty(uint32_t now_ms, uint8_t max_transfers,
                               bool urgent) {
    if (hasQueuedWork()) {
        return advanceTransfers(max_transfers);
    }
    if (m_dirty_count == 0) {
        return false;
    }
    if (!urgent && m_scheduler_gate.allow != NULL &&
        !m_scheduler_gate.allow(m_scheduler_gate.context, urgent)) {
        return false;
    }
    if (!urgent && m_has_refreshed &&
        (uint32_t)(now_ms - m_last_refresh_ms) < DISPLAY_REFRESH_PERIOD_MS) {
        return false;
    }

    uint8_t repaint_count = m_dirty_count;
    memcpy(m_repaint, m_dirty, sizeof(DisplayRect) * repaint_count);
    m_dirty_count = 0;

    m_rendering = true;
    for (uint8_t r = 0; r < repaint_count; ++r) {
        uint8_t order[DISPLAY_MAX_WIDGETS];
        uint8_t order_count = 0;
        for (uint8_t i = 0; i < DISPLAY_MAX_WIDGETS; ++i) {
            DisplayRect intersection;
            if (m_widgets[i].active &&
                (m_widgets[i].page_mask & (uint8_t)(1U << m_active_page)) != 0U &&
                intersectRects(m_widgets[i].bounds, m_repaint[r], &intersection)) {
                uint8_t pos = order_count;
                while (pos > 0 && m_widgets[order[pos - 1U]].z > m_widgets[i].z) {
                    order[pos] = order[pos - 1U];
                    pos--;
                }
                order[pos] = i;
                order_count++;
            }
        }

        for (uint8_t o = 0; o < order_count; ++o) {
            WidgetSlot *slot = &m_widgets[order[o]];
            DisplayRect clip;
            if (intersectRects(slot->bounds, m_repaint[r], &clip)) {
                m_clip_active = true;
                m_render_clip = clip;
                slot->render(*this, clip, slot->context);
                m_clip_active = false;
            }
        }
    }
    m_rendering = false;
    m_last_refresh_ms = now_ms;
    m_has_refreshed = true;

    if (max_transfers > 0) {
        (void)advanceTransfers(max_transfers);
    }
    return true;
}

bool DisplayModule::startCurrentPayloadTransfer() {
    if (!m_has_current_job) {
        return false;
    }

    if (m_current_job.kind == DRAW_JOB_SOLID) {
        uint32_t total_pixels = (uint32_t)m_current_job.rect.w *
                                (uint32_t)m_current_job.rect.h;
        uint32_t sent_pixels = m_current_job.pixels_sent;
        if (sent_pixels >= total_pixels) {
            return false;
        }
        uint32_t remaining = total_pixels - sent_pixels;
        uint16_t chunk_pixels = (uint16_t)(remaining < DISPLAY_TILE_PIXELS ?
                                          remaining : DISPLAY_TILE_PIXELS);
        uint8_t hi = (uint8_t)(m_current_job.color >> 8);
        uint8_t lo = (uint8_t)(m_current_job.color & 0xFFU);
        for (uint16_t i = 0; i < chunk_pixels; ++i) {
            m_dma_buf[i * 2U] = hi;
            m_dma_buf[(i * 2U) + 1U] = lo;
        }
        m_pending_payload_pixels = chunk_pixels;
        m_pending_payload_bytes = (uint16_t)(chunk_pixels * 2U);
        return beginSpiTransfer(DISPLAY_SPI_DATA, m_dma_buf,
                                m_pending_payload_bytes, true);
    }

    uint16_t sent_bytes = m_current_job.pixels_sent;
    uint16_t remaining = (uint16_t)(m_current_job.pixel_bytes - sent_bytes);
    uint16_t chunk = remaining < DISPLAY_MAX_PIXEL_BYTES ? remaining : DISPLAY_MAX_PIXEL_BYTES;
    m_pending_payload_pixels = chunk;
    m_pending_payload_bytes = chunk;
    return beginSpiTransfer(DISPLAY_SPI_DATA,
                            &m_current_job.pixels[sent_bytes], chunk, true);
}

bool DisplayModule::startNextTransfer() {
    if (m_transfer_pending) {
        return false;
    }
    if (!m_has_current_job) {
        if (!dequeueJob(&m_current_job)) {
            return false;
        }
        m_has_current_job = true;
        m_transfer_phase = TRANSFER_COL_CMD;
    }

    uint8_t data[4];
    switch (m_transfer_phase) {
    case TRANSFER_COL_CMD:
        data[0] = 0x2A;
        return beginSpiTransfer(DISPLAY_SPI_COMMAND, data, 1, true);
    case TRANSFER_COL_DATA: {
        int x1 = (int)m_current_job.rect.x + CLUE_TFT_X_OFFSET;
        int x2 = x1 + (int)m_current_job.rect.w - 1;
        data[0] = (uint8_t)((uint16_t)x1 >> 8);
        data[1] = (uint8_t)x1;
        data[2] = (uint8_t)((uint16_t)x2 >> 8);
        data[3] = (uint8_t)x2;
        return beginSpiTransfer(DISPLAY_SPI_DATA, data, 4, true);
    }
    case TRANSFER_ROW_CMD:
        data[0] = 0x2B;
        return beginSpiTransfer(DISPLAY_SPI_COMMAND, data, 1, true);
    case TRANSFER_ROW_DATA: {
        int y1 = m_current_job.rect.y;
        int y2 = y1 + (int)m_current_job.rect.h - 1;
        data[0] = (uint8_t)((uint16_t)y1 >> 8);
        data[1] = (uint8_t)y1;
        data[2] = (uint8_t)((uint16_t)y2 >> 8);
        data[3] = (uint8_t)y2;
        return beginSpiTransfer(DISPLAY_SPI_DATA, data, 4, true);
    }
    case TRANSFER_RAM_CMD:
        data[0] = 0x2C;
        return beginSpiTransfer(DISPLAY_SPI_COMMAND, data, 1, true);
    case TRANSFER_PAYLOAD:
        return startCurrentPayloadTransfer();
    case TRANSFER_IDLE:
    default:
        return false;
    }
}

bool DisplayModule::advanceTransfers(uint8_t max_transfers) {
    if (m_transfer_pending) {
        return true;
    }
    bool advanced = false;
    for (uint8_t i = 0; i < max_transfers; ++i) {
        if (!startNextTransfer()) {
            break;
        }
        advanced = true;
        if (m_transfer_pending) {
            break;
        }
    }
    return advanced || hasQueuedWork();
}

void DisplayModule::completeCurrentTransfer(bool ok) {
    if (!ok) {
        if (m_has_current_job) {
            markDirty(m_current_job.rect);
        }
        m_has_current_job = false;
        m_transfer_phase = TRANSFER_IDLE;
        m_pending_payload_bytes = 0;
        m_pending_payload_pixels = 0;
        return;
    }

    switch (m_pending_phase) {
    case TRANSFER_COL_CMD:
        m_transfer_phase = TRANSFER_COL_DATA;
        break;
    case TRANSFER_COL_DATA:
        m_transfer_phase = TRANSFER_ROW_CMD;
        break;
    case TRANSFER_ROW_CMD:
        m_transfer_phase = TRANSFER_ROW_DATA;
        break;
    case TRANSFER_ROW_DATA:
        m_transfer_phase = TRANSFER_RAM_CMD;
        break;
    case TRANSFER_RAM_CMD:
        m_transfer_phase = TRANSFER_PAYLOAD;
        break;
    case TRANSFER_PAYLOAD:
        if (m_current_job.kind == DRAW_JOB_SOLID) {
            m_current_job.pixels_sent =
                (uint16_t)(m_current_job.pixels_sent + m_pending_payload_pixels);
            uint32_t total = (uint32_t)m_current_job.rect.w *
                             (uint32_t)m_current_job.rect.h;
            if (m_current_job.pixels_sent >= total) {
                m_has_current_job = false;
                m_transfer_phase = TRANSFER_IDLE;
            }
        } else {
            m_current_job.pixels_sent =
                (uint16_t)(m_current_job.pixels_sent + m_pending_payload_bytes);
            if (m_current_job.pixels_sent >= m_current_job.pixel_bytes) {
                m_has_current_job = false;
                m_transfer_phase = TRANSFER_IDLE;
            }
        }
        break;
    case TRANSFER_IDLE:
    default:
        m_transfer_phase = TRANSFER_IDLE;
        break;
    }
    m_pending_payload_bytes = 0;
    m_pending_payload_pixels = 0;
}

bool DisplayModule::handleTransferComplete(bool ok) {
    if (!m_transfer_pending) {
        return false;
    }
    m_transfer_pending = false;
    finishSpiTransfer();
    completeCurrentTransfer(ok);
    m_pending_phase = TRANSFER_IDLE;

    /* Chain the next queued transfer from the SPIM ISR context.
     * Without this, the async transfer pipeline stalls after the
     * first frame and queued pixel data never reaches the display. */
    if (ok && (m_has_current_job || m_job_count > 0)) {
        advanceTransfers(DISPLAY_DEFAULT_QUANTUM);
    }
    return true;
}

void DisplayModule::setSpiSink(const DisplaySpiSink &sink) {
    m_spi_sink = sink;
}

void DisplayModule::setSchedulerGate(const DisplaySchedulerGate &gate) {
    m_scheduler_gate = gate;
}

bool DisplayModule::isTransferBusy() const {
    return m_transfer_pending;
}

uint8_t DisplayModule::dirtyRegionCount() const {
    return m_dirty_count;
}

uint8_t DisplayModule::pendingJobCount() const {
    return (uint8_t)(m_job_count + (m_has_current_job ? 1U : 0U));
}

size_t DisplayModule::dirtyMetadataBytes() const {
    return sizeof m_dirty + sizeof m_repaint + sizeof m_dirty_count;
}

#endif
