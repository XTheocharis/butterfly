/*
 * pdm.h - PDM microphone firmware driver for CLUE.
 *
 * Wraps the pure-logic pdm_eval layer with nrfx_pdm SDK calls.
 * Hardware: nrfx_pdm at 1.280MHz, ratio 80 → exact 16kHz mono PCM16,
 * left channel, falling edge. Two 256-sample DMA buffers (double-buffered).
 * Gain register 0x50 = +20dB.
 *
 * The PDM IRQ handler (priority 6 = app level) only marks a DMA buffer
 * as ready — no metric computation in ISR context. Metrics are computed
 * in thread context via pdm_process_buffer().
 *
 * Firmware-only: all SDK calls are behind #ifdef BOARD_CLUE.
 */
#ifndef AUDIO_PDM_H
#define AUDIO_PDM_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus

#include "pdm_eval.h"

#ifdef BOARD_CLUE

/* ---- PdmMicrophone class (firmware) ---------------------------------- */

class PdmMicrophone {
public:
    PdmMicrophone();

    /* Initialize nrfx_pdm with default config (16kHz, gain 0x50).
     * Acquires PINREG_GROUP_PDM via pinRegistry. Returns true on success. */
    bool init(void);

    /* Start DMA sampling. */
    bool start(void);

    /* Stop DMA sampling. Releases pin group if held. */
    void stop(void);

    /* Called from main loop: check if a DMA buffer is ready and
     * feed it into the metric window. Publishes metrics at 20Hz. */
    void poll(void);

    /* Configure audio parameters (gain, enable/disable).
     * Returns BoardResultCode. */
    uint32_t configure(bool enabled, uint32_t sample_rate_hz,
                       int32_t gain_db_x2, uint32_t flags,
                       uint32_t *out_actual_rate,
                       int32_t *out_actual_gain_db_x2);

    /* Start raw PCM diagnostics capture.
     * radio_idle must be true; the eval layer rejects otherwise.
     * Returns BoardResultCode. On SUCCESS, also ensures the PDM hardware
     * is sampling so the capture ring is fed by poll(). */
    uint32_t startRawPcm(uint32_t request_id, uint32_t duration_ms,
                         uint32_t chunk_size, bool radio_idle);

    /* Check if a raw PCM diagnostics session is active. */
    bool isPcmActive(void) const { return m_pcmState.active; }

    /* Drain up to max_samples captured PCM into out (contiguous write).
     * Returns the number of samples actually written (0 if ring empty).
     * Safe to call from thread context; the ring is fed from poll(). */
    uint32_t drainPcmSamples(int16_t *out, uint32_t max_samples);

    /* Account for samples drained from the ring this pass. Advances the
     * session's sent_samples counter and deactivates the session when
     * total_samples is reached. Returns true if the session is now
     * complete (caller emits the terminal eof chunk). */
    bool accountPcmDrained(uint32_t sample_count);

    /* Cancel any active PCM session and flush the capture ring. */
    void cancelPcm(void);

    /* Total samples the active PCM session will deliver (0 if inactive). */
    uint32_t pcmTotalSamples(void) const { return m_pcmState.total_samples; }

    /* Samples already drained from the active PCM session. */
    uint32_t pcmSentSamples(void) const { return m_pcmState.sent_samples; }

    /* Get latest metrics (NULL if no window completed yet). */
    const pdm_metrics_t *getLatestMetrics(void) const;

    /* Get current configured gain register value. */
    uint8_t getGainRegister(void) const;

    /* Get count of DMA overruns detected since init (0 if none). */
    uint32_t getOverrunCount(void) const { return m_overrunCount; }

private:
    bool m_initialized;
    bool m_running;
    uint8_t m_gain_reg;
    pdm_window_t m_window;
    pdm_metrics_t m_latestMetrics;
    bool m_hasMetrics;
    pdm_pcm_state_t m_pcmState;
    pdm_threshold_state_t m_threshold;
    uint32_t m_overrunCount;

    /* Double-buffered DMA buffers in RAM (EasyDMA requirement). */
    static const uint32_t DMA_BUF_SAMPLES = PDM_DMA_BUF_SAMPLES;
};

#endif /* BOARD_CLUE */

#endif /* __cplusplus */

#endif /* AUDIO_PDM_H */
