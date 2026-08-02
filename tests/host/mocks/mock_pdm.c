#include "mock_pdm.h"
#include <string.h>

static int16_t g_queue[MOCK_PDM_QUEUE_DEPTH];
static size_t  g_head, g_tail, g_count;

void mock_pdm_reset(void) {
    memset(g_queue, 0, sizeof g_queue);
    g_head = g_tail = g_count = 0;
}

size_t mock_pdm_push(const int16_t *samples, size_t n) {
    size_t pushed = 0;
    while (pushed < n && g_count < MOCK_PDM_QUEUE_DEPTH) {
        g_queue[g_tail] = samples[pushed++];
        g_tail = (g_tail + 1) % MOCK_PDM_QUEUE_DEPTH;
        ++g_count;
    }
    return pushed;
}

size_t mock_pdm_read(int16_t *out, size_t max_n) {
    size_t got = 0;
    while (got < max_n && g_count > 0) {
        out[got++] = g_queue[g_head];
        g_head = (g_head + 1) % MOCK_PDM_QUEUE_DEPTH;
        --g_count;
    }
    return got;
}

size_t mock_pdm_pending(void) {
    return g_count;
}
