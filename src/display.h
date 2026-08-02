#ifndef DISPLAY_H
#define DISPLAY_H

#include <stddef.h>
#include <stdint.h>

#define COLOR_BLACK   0x0000
#define COLOR_WHITE   0xFFFF
#define COLOR_RED     0xF800
#define COLOR_GREEN   0x07E0
#define COLOR_BLUE    0x001F
#define COLOR_YELLOW  0xFFE0
#define COLOR_CYAN    0x07FF
#define COLOR_PURPLE  0xF81F
#define COLOR_GRAY    0x8410

struct DisplayRect {
    int16_t x;
    int16_t y;
    int16_t w;
    int16_t h;
};

enum DisplaySpiPhase {
    DISPLAY_SPI_COMMAND = 0,
    DISPLAY_SPI_DATA = 1,
};

typedef uint8_t WidgetHandle;
static const WidgetHandle DISPLAY_WIDGET_INVALID = 0;

class DisplayModule;

typedef void (*DisplayRenderCallback)(DisplayModule &display,
                                      const DisplayRect &clipped_rect,
                                      void *context);
typedef bool (*DisplaySpiTransfer)(void *context, DisplaySpiPhase phase,
                                   const uint8_t *data, uint16_t len,
                                   bool nonblocking);
typedef bool (*DisplaySchedulerGateFn)(void *context, bool urgent);

struct DisplayWidgetConfig {
    DisplayRect bounds;
    DisplayRenderCallback render;
    void *context;
    int16_t z;
    uint8_t page;
};

struct DisplaySpiSink {
    DisplaySpiTransfer transfer;
    void *context;
};

struct DisplaySchedulerGate {
    DisplaySchedulerGateFn allow;
    void *context;
};

#define DISPLAY_MAX_WIDGETS       15U
#define DISPLAY_MAX_DIRTY_RECTS   8U
#define DISPLAY_MAX_PAGES         4U
#define DISPLAY_MAX_DRAW_JOBS     4U
#define DISPLAY_TILE_PIXELS       32U
#define DISPLAY_MAX_PIXEL_BYTES   80U
#define DISPLAY_DEFAULT_QUANTUM   2U
#define DISPLAY_REFRESH_PERIOD_MS 100U

#ifdef BOARD_CLUE

class DisplayModule {
public:
    DisplayModule();
    void init();
    void endBootSplash();
    void fill(uint16_t color);
    void fillRect(int x, int y, int w, int h, uint16_t color);
    void drawText(int x, int y, const char *str, uint16_t fg, uint16_t bg);
    void drawText(int x, int y, const char *str, uint16_t fg);
    void setBacklight(bool on);

    WidgetHandle registerWidget(const DisplayWidgetConfig &config);
    bool unregisterWidget(WidgetHandle handle);
    bool addWidgetToPage(uint8_t page, WidgetHandle handle);
    void clearPage(uint8_t page);
    bool showPage(uint8_t page);

    void markDirty(const DisplayRect &rect);
    bool flushDirty(uint32_t now_ms, uint8_t max_transfers, bool urgent);
    bool advanceTransfers(uint8_t max_transfers);
    bool handleTransferComplete(bool ok);

    void setSpiSink(const DisplaySpiSink &sink);
    void setSchedulerGate(const DisplaySchedulerGate &gate);

    bool isTransferBusy() const;
    uint8_t dirtyRegionCount() const;
    uint8_t pendingJobCount() const;
    size_t dirtyMetadataBytes() const;

private:
    enum DrawJobKind {
        DRAW_JOB_NONE = 0,
        DRAW_JOB_SOLID = 1,
        DRAW_JOB_PIXELS = 2,
    };

    enum TransferPhase {
        TRANSFER_IDLE = 0,
        TRANSFER_COL_CMD,
        TRANSFER_COL_DATA,
        TRANSFER_ROW_CMD,
        TRANSFER_ROW_DATA,
        TRANSFER_RAM_CMD,
        TRANSFER_PAYLOAD,
    };

    struct WidgetSlot {
        DisplayRect bounds;
        DisplayRenderCallback render;
        void *context;
        int16_t z;
        uint8_t page_mask;
        uint8_t generation;
        bool active;
    };

    struct DrawJob {
        DrawJobKind kind;
        DisplayRect rect;
        uint16_t color;
        uint16_t pixel_bytes;
        uint16_t pixels_sent;
        uint8_t pixels[DISPLAY_MAX_PIXEL_BYTES];
    };

    void writeCommand(uint8_t cmd);
    void writeCommand(uint8_t cmd, const uint8_t *data, int len);
    void setAddrWindow(int x, int y, int w, int h);
    void select();
    void deselect();
    void dcCommand();
    void dcData();

    bool beginSpiTransfer(DisplaySpiPhase phase, const uint8_t *data,
                          uint16_t len, bool nonblocking);
    void finishSpiTransfer();
    bool startNextTransfer();
    bool startCurrentPayloadTransfer();
    void completeCurrentTransfer(bool ok);

    bool enqueueSolidRect(const DisplayRect &rect, uint16_t color);
    bool enqueuePixelRect(const DisplayRect &rect, const uint8_t *pixels,
                          uint16_t pixel_bytes);
    bool dequeueJob(DrawJob *job);
    bool hasQueuedWork() const;

    bool resolveHandle(WidgetHandle handle, uint8_t *index) const;
    WidgetHandle makeHandle(uint8_t index) const;
    void invalidateWidget(const WidgetSlot &slot);

    bool clipToScreen(const DisplayRect &rect, DisplayRect *out) const;
    bool intersectRects(const DisplayRect &a, const DisplayRect &b,
                        DisplayRect *out) const;
    void mergeDirtyRect(const DisplayRect &rect);
    DisplayRect unionRects(const DisplayRect &a, const DisplayRect &b) const;
    bool rectsTouchOrOverlap(const DisplayRect &a, const DisplayRect &b) const;

    void writeSolidRectSync(const DisplayRect &rect, uint16_t color);
    void writePixelsSync(const DisplayRect &rect, const uint8_t *pixels,
                         uint16_t pixel_bytes);
    void drawGlyph(int x, int y, char c, uint16_t fg, uint16_t bg);

    WidgetSlot m_widgets[DISPLAY_MAX_WIDGETS];
    DisplayRect m_dirty[DISPLAY_MAX_DIRTY_RECTS];
    DisplayRect m_repaint[DISPLAY_MAX_DIRTY_RECTS];
    DrawJob m_jobs[DISPLAY_MAX_DRAW_JOBS];
    DrawJob m_current_job;
    uint8_t m_dirty_count;
    uint8_t m_job_head;
    uint8_t m_job_count;
    uint8_t m_active_page;
    uint8_t m_next_generation;
    uint32_t m_last_refresh_ms;
    bool m_has_refreshed;
    bool m_sync_allowed;
    bool m_rendering;
    bool m_clip_active;
    DisplayRect m_render_clip;
    bool m_has_current_job;
    TransferPhase m_transfer_phase;
    TransferPhase m_pending_phase;
    bool m_transfer_pending;
    uint16_t m_pending_payload_bytes;
    uint16_t m_pending_payload_pixels;
    uint8_t m_dma_buf[DISPLAY_MAX_PIXEL_BYTES];
    DisplaySpiSink m_spi_sink;
    DisplaySchedulerGate m_scheduler_gate;
};

#else

class DisplayModule {
public:
    DisplayModule() {}
    void init() {}
    void fill(uint16_t) {}
    void fillRect(int, int, int, int, uint16_t) {}
    void drawText(int, int, const char *, uint16_t, uint16_t) {}
    void drawText(int, int, const char *, uint16_t) {}
    void setBacklight(bool) {}
    WidgetHandle registerWidget(const DisplayWidgetConfig &) { return DISPLAY_WIDGET_INVALID; }
    bool unregisterWidget(WidgetHandle) { return false; }
    bool addWidgetToPage(uint8_t, WidgetHandle) { return false; }
    void clearPage(uint8_t) {}
    bool showPage(uint8_t) { return false; }
    void markDirty(const DisplayRect &) {}
    bool flushDirty(uint32_t, uint8_t, bool) { return false; }
    bool advanceTransfers(uint8_t) { return false; }
    bool handleTransferComplete(bool) { return false; }
    void setSpiSink(const DisplaySpiSink &) {}
    void setSchedulerGate(const DisplaySchedulerGate &) {}
    bool isTransferBusy() const { return false; }
    uint8_t dirtyRegionCount() const { return 0; }
    uint8_t pendingJobCount() const { return 0; }
    size_t dirtyMetadataBytes() const { return 0; }
};

#endif

#endif
