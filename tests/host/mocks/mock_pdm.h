/*
 * mock_pdm.h - PCM sample queue standing in for the nRF52 PDM microphone.
 *
 * Tests enqueue known PCM samples (Q15) and the code-under-test pulls them
 * via mock_pdm_read. Backed by a fixed-size ring so memory cost is bounded.
 * Used by Todo 26 PDM microphone tests.
 */
#ifndef MOCK_PDM_H
#define MOCK_PDM_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOCK_PDM_QUEUE_DEPTH 512

void   mock_pdm_reset(void);
size_t mock_pdm_push(const int16_t *samples, size_t n);
size_t mock_pdm_read(int16_t *out, size_t max_n);
size_t mock_pdm_pending(void);

#ifdef __cplusplus
}
#endif
#endif /* MOCK_PDM_H */
