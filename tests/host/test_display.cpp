#define BOARD_CLUE 1
#define DISPLAY_HOST_TEST 1

#include "../../src/display.cpp"

#include "test_framework.h"

#include <stdint.h>
#include <string.h>

struct SpiEvent {
    DisplaySpiPhase phase;
    uint16_t len;
    bool nonblocking;
    uint8_t bytes[8];
};

struct SpiRecorder {
    SpiEvent events[4096];
    size_t count;
    bool reject_next;
};

struct PaintProbe {
    uint16_t color;
    int calls;
    DisplayRect last_clip;
    uint8_t order[8];
    uint8_t order_len;
    uint8_t id;
};

static bool record_spi(void *context, DisplaySpiPhase phase,
                       const uint8_t *data, uint16_t len,
                       bool nonblocking) {
    SpiRecorder *recorder = static_cast<SpiRecorder *>(context);
    if (recorder->reject_next) {
        recorder->reject_next = false;
        return false;
    }
    if (recorder->count >= (sizeof recorder->events / sizeof recorder->events[0])) {
        TEST_FAIL("SPI recorder overflow");
        return false;
    }

    SpiEvent *event = &recorder->events[recorder->count++];
    event->phase = phase;
    event->len = len;
    event->nonblocking = nonblocking;
    size_t stored = len < sizeof event->bytes ? len : sizeof event->bytes;
    if (stored > 0) {
        memcpy(event->bytes, data, stored);
    }
    return true;
}

static bool closed_gate(void *context, bool urgent) {
    (void)context;
    return urgent;
}

static void install_recorder(DisplayModule *display, SpiRecorder *recorder) {
    memset(recorder, 0, sizeof *recorder);
    DisplaySpiSink sink = { record_spi, recorder };
    display->setSpiSink(sink);
}

static int find_command(const SpiRecorder *recorder, uint8_t command,
                        size_t start) {
    for (size_t i = start; i < recorder->count; ++i) {
        const SpiEvent *event = &recorder->events[i];
        if (event->phase == DISPLAY_SPI_COMMAND && event->len == 1 &&
            event->bytes[0] == command) {
            return (int)i;
        }
    }
    return -1;
}

static void drain_display(DisplayModule *display) {
    for (int guard = 0; guard < 2000; ++guard) {
        if (display->isTransferBusy()) {
            TEST_ASSERT(display->handleTransferComplete(true),
                        "pending transfer completes exactly once");
            continue;
        }
        if (display->pendingJobCount() == 0) {
            return;
        }
        TEST_ASSERT(display->advanceTransfers(1), "queued job advances");
    }
    TEST_FAIL("display drain did not become idle");
}

static void paint_probe(DisplayModule &display, const DisplayRect &clip,
                        void *context) {
    PaintProbe *probe = static_cast<PaintProbe *>(context);
    probe->calls++;
    probe->last_clip = clip;
    if (probe->order_len < sizeof probe->order) {
        probe->order[probe->order_len++] = probe->id;
    }
    display.fillRect(clip.x, clip.y, clip.w, clip.h, probe->color);
}

static WidgetHandle add_probe(DisplayModule *display, PaintProbe *probe,
                              const DisplayRect &bounds, int16_t z,
                              uint8_t page) {
    DisplayWidgetConfig config = { bounds, paint_probe, probe, z, page };
    WidgetHandle handle = display->registerWidget(config);
    TEST_ASSERT(handle != DISPLAY_WIDGET_INVALID, "widget registered");
    return handle;
}

static void test_init_stream_includes_frctrl2(void) {
    DisplayModule display;
    SpiRecorder recorder;
    install_recorder(&display, &recorder);

    display.init();

    const uint8_t expected[] = { 0x01, 0x11, 0x36, 0x3A, 0xC6, 0x21, 0x13, 0x29 };
    size_t cursor = 0;
    for (size_t i = 0; i < sizeof expected; ++i) {
        int idx = find_command(&recorder, expected[i], cursor);
        TEST_ASSERT(idx >= 0, "ST7789 init command appears in order");
        cursor = (size_t)idx + 1;
    }

    int frctrl2 = find_command(&recorder, 0xC6, 0);
    TEST_ASSERT(frctrl2 >= 0, "FRCTRL2 command present");
    TEST_ASSERT((size_t)(frctrl2 + 1) < recorder.count, "FRCTRL2 has data event");
    const SpiEvent *data = &recorder.events[frctrl2 + 1];
    TEST_ASSERT_EQ_INT(DISPLAY_SPI_DATA, data->phase);
    TEST_ASSERT_EQ_INT(1, data->len);
    TEST_ASSERT_EQ_INT(0x01, data->bytes[0]);
}

static void test_dirty_rect_repaints_partial_window(void) {
    DisplayModule display;
    SpiRecorder recorder;
    install_recorder(&display, &recorder);

    PaintProbe probe = { COLOR_RED, 0, { 0, 0, 0, 0 }, { 0 }, 0, 1 };
    add_probe(&display, &probe, { 0, 0, 100, 100 }, 1, 0);
    TEST_ASSERT(display.flushDirty(0, 0, true), "initial widget paint accepted");
    drain_display(&display);
    memset(&recorder, 0, sizeof recorder);
    probe.calls = 0;

    display.markDirty({ 10, 12, 20, 8 });
    TEST_ASSERT(display.flushDirty(100, 1, false), "dirty repaint starts");
    drain_display(&display);

    TEST_ASSERT_EQ_INT(1, probe.calls);
    TEST_ASSERT_EQ_INT(10, probe.last_clip.x);
    TEST_ASSERT_EQ_INT(12, probe.last_clip.y);
    TEST_ASSERT_EQ_INT(20, probe.last_clip.w);
    TEST_ASSERT_EQ_INT(8, probe.last_clip.h);

    int col = find_command(&recorder, 0x2A, 0);
    int row = find_command(&recorder, 0x2B, 0);
    TEST_ASSERT(col >= 0, "column window written");
    TEST_ASSERT(row >= 0, "row window written");
    const SpiEvent *col_data = &recorder.events[col + 1];
    const SpiEvent *row_data = &recorder.events[row + 1];
    TEST_ASSERT_EQ_INT(0, col_data->bytes[0]);
    TEST_ASSERT_EQ_INT(90, col_data->bytes[1]);
    TEST_ASSERT_EQ_INT(0, col_data->bytes[2]);
    TEST_ASSERT_EQ_INT(109, col_data->bytes[3]);
    TEST_ASSERT_EQ_INT(0, row_data->bytes[0]);
    TEST_ASSERT_EQ_INT(12, row_data->bytes[1]);
    TEST_ASSERT_EQ_INT(0, row_data->bytes[2]);
    TEST_ASSERT_EQ_INT(19, row_data->bytes[3]);
}

static void test_z_order_page_and_unregister(void) {
    DisplayModule display;
    SpiRecorder recorder;
    install_recorder(&display, &recorder);

    PaintProbe low = { COLOR_GREEN, 0, { 0, 0, 0, 0 }, { 0 }, 0, 1 };
    PaintProbe high = { COLOR_BLUE, 0, { 0, 0, 0, 0 }, { 0 }, 0, 2 };
    WidgetHandle low_handle = add_probe(&display, &low, { 0, 0, 20, 20 }, 1, 0);
    WidgetHandle high_handle = add_probe(&display, &high, { 0, 0, 20, 20 }, 3, 0);

    (void)low_handle;
    display.markDirty({ 0, 0, 8, 8 });
    TEST_ASSERT(display.flushDirty(0, 0, false), "z-order repaint accepted");
    TEST_ASSERT_EQ_INT(1, low.order[0]);
    TEST_ASSERT_EQ_INT(2, high.order[0]);
    drain_display(&display);

    TEST_ASSERT(display.unregisterWidget(high_handle), "unregister succeeds");
    TEST_ASSERT(display.flushDirty(100, 0, false), "unregister dirties old bounds");
    TEST_ASSERT_EQ_INT(2, low.calls);
    TEST_ASSERT_EQ_INT(1, high.calls);
    drain_display(&display);

    PaintProbe page1 = { COLOR_YELLOW, 0, { 0, 0, 0, 0 }, { 0 }, 0, 3 };
    add_probe(&display, &page1, { 40, 40, 10, 10 }, 1, 1);
    TEST_ASSERT(display.showPage(1), "page transition succeeds");
    TEST_ASSERT(display.flushDirty(200, 0, false), "page transition repaints");
    TEST_ASSERT_EQ_INT(1, page1.calls);
}

static void test_dma_completion_error_and_deferral(void) {
    DisplayModule display;
    SpiRecorder recorder;
    install_recorder(&display, &recorder);

    PaintProbe probe = { COLOR_WHITE, 0, { 0, 0, 0, 0 }, { 0 }, 0, 1 };
    add_probe(&display, &probe, { 0, 0, 12, 12 }, 1, 0);
    display.setSchedulerGate({ closed_gate, NULL });

    display.markDirty({ 1, 1, 4, 4 });
    TEST_ASSERT(!display.flushDirty(0, 1, false), "nonurgent repaint is deferred");
    TEST_ASSERT_EQ_INT(1, display.dirtyRegionCount());
    TEST_ASSERT(display.flushDirty(0, 1, true), "urgent repaint bypasses gate");
    TEST_ASSERT(display.isTransferBusy(), "async transfer is pending");
    TEST_ASSERT(display.handleTransferComplete(false), "DMA error is handled once");
    TEST_ASSERT(!display.handleTransferComplete(false), "second DMA completion is ignored");
    TEST_ASSERT(!display.isTransferBusy(), "error clears pending transfer");
    TEST_ASSERT(display.dirtyRegionCount() > 0, "failed transfer re-invalidates pixels");
}

static void test_10hz_coalescing_and_metadata_budget(void) {
    DisplayModule display;
    SpiRecorder recorder;
    install_recorder(&display, &recorder);

    PaintProbe probe = { COLOR_CYAN, 0, { 0, 0, 0, 0 }, { 0 }, 0, 1 };
    add_probe(&display, &probe, { 0, 0, 20, 20 }, 1, 0);

    display.markDirty({ 0, 0, 5, 5 });
    TEST_ASSERT(display.flushDirty(0, 0, false), "first refresh allowed");
    TEST_ASSERT_EQ_INT(1, probe.calls);
    drain_display(&display);

    display.markDirty({ 1, 1, 5, 5 });
    TEST_ASSERT(!display.flushDirty(50, 0, false), "refresh before 100ms coalesces");
    TEST_ASSERT_EQ_INT(1, probe.calls);
    TEST_ASSERT(display.flushDirty(100, 0, false), "refresh at 10Hz boundary runs");
    TEST_ASSERT_EQ_INT(2, probe.calls);
    TEST_ASSERT(display.dirtyMetadataBytes() < 1024, "dirty metadata stays under 1KiB");
}

int main(void) {
    test_framework_init();
    RUN_TEST(test_init_stream_includes_frctrl2);
    RUN_TEST(test_dirty_rect_repaints_partial_window);
    RUN_TEST(test_z_order_page_and_unregister);
    RUN_TEST(test_dma_completion_error_and_deferral);
    RUN_TEST(test_10hz_coalescing_and_metadata_budget);
    return test_framework_finish();
}
