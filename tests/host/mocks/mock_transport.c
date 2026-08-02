#include "mock_transport.h"
#include <string.h>

static mock_transport_pkt_t g_rx_queue[MOCK_TRANSPORT_QUEUE_DEPTH];
static size_t g_rx_head, g_rx_tail;

static mock_transport_pkt_t g_sent[MOCK_TRANSPORT_MAX_SENT];
static size_t g_sent_count;

void mock_transport_reset(void) {
    g_rx_head = g_rx_tail = 0;
    g_sent_count = 0;
    memset(g_rx_queue, 0, sizeof g_rx_queue);
    memset(g_sent, 0, sizeof g_sent);
}

int mock_transport_send(const uint8_t *data, size_t len) {
    if (len > MOCK_TRANSPORT_MAX_PKT) return -1;
    if (g_sent_count >= MOCK_TRANSPORT_MAX_SENT) return -1;
    mock_transport_pkt_t *p = &g_sent[g_sent_count++];
    memcpy(p->bytes, data, len);
    p->len = (uint16_t)len;
    return 0;
}

int mock_transport_recv(uint8_t *buf, size_t maxlen, size_t *out_len) {
    if (g_rx_head == g_rx_tail) return -1; /* empty */
    const mock_transport_pkt_t *p = &g_rx_queue[g_rx_head];
    size_t n = (p->len < maxlen) ? (size_t)p->len : maxlen;
    if (n) memcpy(buf, p->bytes, n);
    if (out_len) *out_len = p->len;
    g_rx_head = (g_rx_head + 1) % MOCK_TRANSPORT_QUEUE_DEPTH;
    return (n == (size_t)p->len) ? 0 : -1; /* -1 signals truncation */
}

void mock_transport_enqueue_rx(const uint8_t *data, size_t len) {
    if (len > MOCK_TRANSPORT_MAX_PKT) return;
    size_t next = (g_rx_tail + 1) % MOCK_TRANSPORT_QUEUE_DEPTH;
    if (next == g_rx_head) return; /* full, drop */
    mock_transport_pkt_t *p = &g_rx_queue[g_rx_tail];
    memcpy(p->bytes, data, len);
    p->len = (uint16_t)len;
    g_rx_tail = next;
}

size_t mock_transport_sent_count(void) {
    return g_sent_count;
}

const mock_transport_pkt_t *mock_transport_sent(size_t index) {
    return (index < g_sent_count) ? &g_sent[index] : NULL;
}
