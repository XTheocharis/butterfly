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

/* ---- PCM capture ring (4 × 256-sample blocks = 1024 samples = 64ms) --
 * Written by PdmMicrophone::poll() in thread context when a raw PCM
 * session is active. Drained by PdmMicrophone::drainPcmSamples() from
 * BoardModule::tick(). All indices are byte-aligned (uint8_t/uint16_t)
 * so single-copy reads are atomic on Cortex-M4. Critical regions are
 * still used on multi-field updates to keep the invariants tight. */
#define PCM_RING_BLOCKS 4u
static int16_t s_pcmRing[PCM_RING_BLOCKS][PDM_DMA_BUF_SAMPLES] __attribute__((aligned(4)));
static volatile uint8_t  s_pcmWriteIdx;          /* next block to fill (0..3) */
static volatile uint8_t  s_pcmReadIdx;           /* block being drained (0..3) */
static volatile uint16_t s_pcmReadOffset;        /* samples consumed in read block */
static volatile uint16_t s_pcmSamplesAvailable;  /* total unread samples */

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
    , m_initResult(0)
    , m_startResult(0)
{
    pdm_window_init(&m_window);
    memset(&m_latestMetrics, 0, sizeof(m_latestMetrics));
    pdm_pcm_init(&m_pcmState);
    pdm_threshold_init(&m_threshold);
    s_ready_idx = 0xFF;
    s_overrun = false;
    s_pcmWriteIdx = 0;
    s_pcmReadIdx = 0;
    s_pcmReadOffset = 0;
    s_pcmSamplesAvailable = 0;
}

/* No-op restore callback for pin registry (PDM never releases its pins). */
static void pdm_restore_noop(pinreg_group_t, uint8_t) {}

bool PdmMicrophone::init(void)
{
    if (m_initialized) {
        return true;
    }

    /* Acquire PDM pin group (P0.00 DATA + P0.01 CLK). */
    pinreg_token_t token;
    pinreg_result_t rc = pinreg_acquire_group(
        PINREG_GROUP_PDM, PINREG_OWNER_AUDIO, pdm_restore_noop, &token);
    if (rc != PINREG_OK) {
        m_initResult = -1;
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
        m_initResult = -2;
        pinreg_release(token);
        return false;
    }

    m_initialized = true;
    m_initResult = 1;
    return true;
}

bool PdmMicrophone::start(void)
{
    if (!m_initialized || m_running) {
        m_startResult = m_running ? 1 : -3;
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

    /* Feed PCM capture ring when a raw PCM session is active.
     * One full DMA buffer (256 samples = 16ms @ 16kHz) per poll() pass.
     * If the ring is full we drop the oldest block by advancing the read
     * pointer — preferred over stalling the DMA pipeline. */
    if (m_pcmState.active) {
        uint8_t widx = s_pcmWriteIdx;
        memcpy(s_pcmRing[widx], buf,
               PDM_DMA_BUF_SAMPLES * sizeof(int16_t));

        uint8_t next_w = (uint8_t)((widx + 1u) % PCM_RING_BLOCKS);
        s_pcmWriteIdx = next_w;

        uint16_t avail = s_pcmSamplesAvailable;
        if (avail + PDM_DMA_BUF_SAMPLES >
            (PCM_RING_BLOCKS * PDM_DMA_BUF_SAMPLES)) {
            /* Ring overflow: drop the oldest block by advancing read idx. */
            s_pcmReadIdx = (uint8_t)((s_pcmReadIdx + 1u) % PCM_RING_BLOCKS);
            s_pcmReadOffset = 0;
            avail = (uint16_t)((PCM_RING_BLOCKS - 1u) * PDM_DMA_BUF_SAMPLES);
        }
        s_pcmSamplesAvailable = (uint16_t)(avail + PDM_DMA_BUF_SAMPLES);
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
    uint32_t code = pdm_pcm_start(&m_pcmState, mode, radio_idle, request_id,
                                  duration_ms, chunk_size);
    if (code != PDM_EVAL_SUCCESS) {
        return code;
    }

    /* Ensure PDM hardware is sampling so poll() can feed the ring.
     * Caller is responsible for having called init() once at startup;
     * if init failed earlier we surface BUSY (hardware unavailable). */
    if (!m_initialized) {
        pdm_pcm_cancel(&m_pcmState);
        return PDM_EVAL_BUSY;
    }
    if (!m_running) {
        if (!start()) {
            pdm_pcm_cancel(&m_pcmState);
            return PDM_EVAL_BUSY;
        }
    }

    /* Flush any stale capture ring state so the new session starts clean. */
    s_pcmWriteIdx = 0;
    s_pcmReadIdx = 0;
    s_pcmReadOffset = 0;
    s_pcmSamplesAvailable = 0;
    return PDM_EVAL_SUCCESS;
}

uint32_t PdmMicrophone::drainPcmSamples(int16_t *out, uint32_t max_samples)
{
    if (out == NULL || max_samples == 0) {
        return 0;
    }

    /* Critical region around ring index mutation. poll() writes
     * s_pcmWriteIdx/s_pcmSamplesAvailable from the same thread context
     * as tick(), but PRIMASK-based masking guarantees the read+update
     * sequence here is not torn by an intervening ISR-redirected poll(). */
    uint8_t nested = 0;
    (void)platform_runtime_critical_region_enter(&nested);

    uint32_t drained = 0;
    while (drained < max_samples && s_pcmSamplesAvailable > 0) {
        uint16_t in_block = (uint16_t)(PDM_DMA_BUF_SAMPLES - s_pcmReadOffset);
        uint32_t take = (in_block < (max_samples - drained))
                      ? in_block : (max_samples - drained);

        memcpy(out + drained,
               &s_pcmRing[s_pcmReadIdx][s_pcmReadOffset],
               take * sizeof(int16_t));

        drained += take;
        s_pcmReadOffset = (uint16_t)(s_pcmReadOffset + take);
        s_pcmSamplesAvailable = (uint16_t)(s_pcmSamplesAvailable - take);

        if (s_pcmReadOffset >= PDM_DMA_BUF_SAMPLES) {
            s_pcmReadOffset = 0;
            s_pcmReadIdx = (uint8_t)((s_pcmReadIdx + 1u) % PCM_RING_BLOCKS);
        }
    }

    (void)platform_runtime_critical_region_exit(nested);
    return drained;
}

void PdmMicrophone::cancelPcm(void)
{
    pdm_pcm_cancel(&m_pcmState);
    uint8_t nested = 0;
    (void)platform_runtime_critical_region_enter(&nested);
    s_pcmWriteIdx = 0;
    s_pcmReadIdx = 0;
    s_pcmReadOffset = 0;
    s_pcmSamplesAvailable = 0;
    (void)platform_runtime_critical_region_exit(nested);
}

bool PdmMicrophone::accountPcmDrained(uint32_t sample_count)
{
    if (!m_pcmState.active || sample_count == 0) {
        return !m_pcmState.active;
    }

    m_pcmState.sent_samples += sample_count;
    if (m_pcmState.sent_samples >= m_pcmState.total_samples) {
        m_pcmState.active = false;
        return true;
    }
    return false;
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
