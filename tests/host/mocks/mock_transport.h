/*
 * mock_transport.h - WHAD transport sink/source for code-under-test.
 *
 * Captures every byte sequence the code-under-test "sends" and lets the
 * test driver enqueue packets for it to "receive". Used by protocol
 * round-trip tests, framed-payload regression tests, and stream rate tests.
 */
#ifndef MOCK_TRANSPORT_H
#define MOCK_TRANSPORT_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOCK_TRANSPORT_MAX_PKT     256
#define MOCK_TRANSPORT_QUEUE_DEPTH 32
#define MOCK_TRANSPORT_MAX_SENT    64

typedef struct {
    uint8_t  bytes[MOCK_TRANSPORT_MAX_PKT];
    uint16_t len;
} mock_transport_pkt_t;

void mock_transport_reset(void);

/* Code-under-test side. */
int mock_transport_send(const uint8_t *data, size_t len);  /* 0 ok, -1 oversize/full */
int mock_transport_recv(uint8_t *buf, size_t maxlen, size_t *out_len); /* 0 ok, -1 empty/truncated */

/* Test-driver side. */
void mock_transport_enqueue_rx(const uint8_t *data, size_t len);
size_t mock_transport_sent_count(void);
const mock_transport_pkt_t *mock_transport_sent(size_t index);

#ifdef __cplusplus
}
#endif
#endif /* MOCK_TRANSPORT_H */
