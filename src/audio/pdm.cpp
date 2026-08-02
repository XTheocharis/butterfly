/*
 * pdm.cpp - PDM microphone firmware driver implementation.
 *
 * Wraps nrfx_pdm SDK calls around the pure-logic pdm_eval layer.
 * DMA IRQ handler marks buffer ready only — no ISR computation.
 * Metric windows are finalized in thread context via poll().
 *
 * Firmware-only: #ifdef BOARD_CLUE guards all SDK code.
 */
#include "pdm.h"

#ifdef BOARD_CLUE

#include <string.h>
#include <stdio.h>

#include "nrfx_pdm.h"
#include "nrf_gpio.h"
#include "pinRegistry.h"
#include "timebase.h"
#include "custom_board.h"
#include "platformRuntime.h"

/* ---- DMA buffers (must be in RAM for EasyDMA) ------------------------ */

static int16_t s_dma_buf[2][PDM_DMA_BUF_SAMPLES] __attribute__((aligned(4)));

/* volatile: written from ISR, read from thread context. */
static volatile uint8_t s_ready_idx;   /* 0/1 = ready buffer, 0xFF = none */
static volatile bool s_overrun;         /* ISR fired before previous consumed */

/* ---- PDM event handler (ISR context — minimal work) ----------------- */

static void pdm_handler(nrfx_pdm_evt_t const *const p_evt)
{
    if (p_evt->error != 0u) {
        return;
    }

    if (p_evt->buffer_requested) {
        /* Provide next buffer for double-buffering. */
        uint8_t next = (s_ready_idx == 0u) ? 1u : 0u;
        if (s_ready_idx != 0xFF) {
            /* Previous buffer not yet consumed → overrun. */
            s_overrun = true;
        }
        nrfx_pdm_buffer_set(s_dma_buf[next], PDM_DMA_BUF_SAMPLES);
        s_ready_idx = next;
    }
}

/* ---- PdmMicrophone class --------------------------------------------- */

PdmMicrophone::PdmMicrophone()
    : m_initialized(false)
    , m_running(false)
    , m_gain_reg(PDM_GAIN_DEFAULT)
    , m_hasMetrics(false)
    , m_overrunCount(0)
{
    pdm_window_init(&m_window);
    memset(&m_latestMetrics, 0, sizeof(m_latestMetrics));
    pdm_pcm_init(&m_pcmState);
    pdm_threshold_init(&m_threshold);
    s_ready_idx = 0xFF;
    s_overrun = false;
}

bool PdmMicrophone::init(void)
{
    if (m_initialized) {
        return true;
    }

    /* Acquire PDM pin group (P0.00 DATA + P0.01 CLK). */
    pinreg_token_t token;
    pinreg_result_t rc = pinreg_acquire_group(
        PINREG_GROUP_PDM, PINREG_OWNER_AUDIO, NULL, &token);
    if (rc != PINREG_OK) {
        return false;
    }

    /* Configure nrfx_pdm. */
    nrfx_pdm_config_t config = NRFX_PDM_DEFAULT_CONFIG(
        CLUE_PDM_CLK, CLUE_PDM_DATA);
    config.mode = NRF_PDM_MODE_MONO;  /* CLUE has a single MEMS mic on Left channel; MONO yields successive Left samples at 16kHz */
    config.edge = NRF_PDM_EDGE_LEFTFALLING;
    config.clock_freq = (nrf_pdm_freq_t)PDM_PDMCLKCTRL_FREQ_1280K;  /* 1.280 MHz → 16kHz / ratio 80 */
    config.gain_l = m_gain_reg;
    config.gain_r = m_gain_reg;
    config.interrupt_priority = 6;  /* app level, below SD/BLE */

    nrfx_err_t err = nrfx_pdm_init(&config, pdm_handler);
    if (err != NRFX_SUCCESS) {
        pinreg_release(token);
        return false;
    }

    m_initialized = true;
    return true;
}

bool PdmMicrophone::start(void)
{
    if (!m_initialized || m_running) {
        return m_running;
    }

    s_ready_idx = 0xFF;
    s_overrun = false;

    /* Provide initial buffer, then start sampling. */
    nrfx_pdm_buffer_set(s_dma_buf[0], PDM_DMA_BUF_SAMPLES);
    nrfx_err_t err = nrfx_pdm_start();
    if (err != NRFX_SUCCESS) {
        return false;
    }

    m_running = true;
    return true;
}

void PdmMicrophone::stop(void)
{
    if (!m_running) {
        return;
    }

    nrfx_pdm_stop();
    m_running = false;
    s_ready_idx = 0xFF;
}

void PdmMicrophone::poll(void)
{
    if (!m_running) {
        return;
    }

    /* Check if a DMA buffer is ready. */
    uint8_t idx = s_ready_idx;
    if (idx == 0xFF) {
        return;
    }

    /* Clear ready flag (claim the buffer). */
    s_ready_idx = 0xFF;

    /* Feed the buffer into the metric window. */
    const int16_t *buf = s_dma_buf[idx];
    uint64_t ts = timebase_now_us();

    uint32_t offset = 0;
    while (offset < PDM_DMA_BUF_SAMPLES) {
        uint32_t consumed = 0;
        if (pdm_window_feed(&m_window, buf + offset,
                            PDM_DMA_BUF_SAMPLES - offset, ts, &consumed)) {
            pdm_window_finalize(&m_window, &m_latestMetrics);
            m_hasMetrics = true;

            /* Evaluate threshold if subscribed. */
            (void)pdm_threshold_eval(&m_threshold,
                                     m_latestMetrics.dbfs_x1000, true);
        }
        offset += consumed;
    }

    /* Detect DMA overrun (ISR fired before previous buffer was consumed). */
    if (s_overrun) {
        s_overrun = false;
        m_overrunCount++;
    }
}

uint32_t PdmMicrophone::configure(bool enabled, uint32_t sample_rate_hz,
                                  int32_t gain_db_x2, uint32_t flags,
                                  uint32_t *out_actual_rate,
                                  int32_t *out_actual_gain_db_x2)
{
    uint32_t code = pdm_eval_audio_configure(
        enabled, sample_rate_hz, gain_db_x2, flags,
        out_actual_rate, out_actual_gain_db_x2);

    if (code != PDM_EVAL_SUCCESS) {
        return code;
    }

    /* Apply gain if PDM is running. */
    if (out_actual_gain_db_x2 != NULL) {
        int32_t gain_db = *out_actual_gain_db_x2 / 2;
        m_gain_reg = PDM_GAIN_TO_REG(gain_db);
        if (m_running) {
            nrf_pdm_gain_set(m_gain_reg, m_gain_reg);
        }
    }

    if (!enabled && m_running) {
        stop();
    } else if (enabled && !m_running) {
        start();
    }

    return PDM_EVAL_SUCCESS;
}

uint32_t PdmMicrophone::startRawPcm(uint32_t request_id, uint32_t duration_ms,
                                    uint32_t chunk_size, bool radio_idle)
{
    runtime_mode_t mode = runtime_get_selected();
    return pdm_pcm_start(&m_pcmState, mode, radio_idle, request_id,
                         duration_ms, chunk_size);
}

const pdm_metrics_t *PdmMicrophone::getLatestMetrics(void) const
{
    return m_hasMetrics ? &m_latestMetrics : NULL;
}

uint8_t PdmMicrophone::getGainRegister(void) const
{
    return m_gain_reg;
}

/* ---- Compile-time verification of constants -------------------------- */
/*
 * 1.280MHz / 80 = 16,000Hz exactly.
 * This is verified at compile time so a wrong ratio is a build error.
 */
static_assert(PDM_PDM_CLK_HZ / PDM_RATIO == PDM_SAMPLE_RATE_HZ,
    "PDM clock / ratio must equal sample rate");

/* Gain 0x50 = +20dB (register = dB*2 + 40). */
static_assert(PDM_GAIN_DEFAULT == 0x50,
    "Default gain must be 0x50 (+20dB)");

#endif /* BOARD_CLUE */
