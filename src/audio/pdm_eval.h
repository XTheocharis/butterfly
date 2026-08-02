/*
 * pdm_eval.h - Pure-logic PDM microphone metrics pipeline evaluation.
 *
 * No SDK deps. Included from boardModule.cpp (firmware) and
 * test_pdm.cpp (host test).
 *
 * Provides: 50ms metric windows (800 samples at 16kHz), overflow-safe
 * sum-of-squares, Q15 RMS, milli-dBFS, clipping count, threshold FSM
 * with hysteresis/debounce, and raw PCM diagnostic chunk sequencing.
 *
 * PDM hardware: 1.280MHz clock / ratio 80 = exact 16.000kHz mono PCM16,
 * left channel, falling edge. Two 256-sample DMA buffers double-buffered.
 * Gain register 0x50 = +20dB (formula: register = dB*2 + 40).
 */
#ifndef AUDIO_PDM_EVAL_H
#define AUDIO_PDM_EVAL_H

#include <stdint.h>
#include <stdbool.h>

/* runtime_mode_t for PCM runtime rejection. */
#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- PDM hardware constants (frozen for tests) ----------------------- */

#define PDM_PDM_CLK_HZ            1280000u   /* 1.280 MHz master clock */
#define PDM_RATIO                 80u         /* PDM_CLK / ratio = sample rate */
#define PDM_SAMPLE_RATE_HZ        16000u      /* exact 16.000 kHz */
#define PDM_DMA_BUF_SAMPLES       256u        /* per DMA buffer */
#define PDM_DMA_BUF_COUNT         2u          /* double-buffered */

/* ---- Metric window constants ----------------------------------------- */

#define PDM_WINDOW_SAMPLES        800u        /* 50ms at 16kHz */
#define PDM_WINDOW_US             50000u      /* 50ms */
#define PDM_WINDOW_MS             50u
#define PDM_PUBLISH_RATE_MHZ      20000u      /* 20 Hz publish rate */

/* ---- Gain (register-encoded, NOT decimal) ---------------------------- */
/*
 * register_value = (desired_dB * 2) + 40
 * +20dB → (20*2)+40 = 80 = 0x50
 * Adafruit DEFAULT_PDM_GAIN=20 (decimal) → register 0x14 = −10dB (WRONG)
 */
#define PDM_GAIN_DEFAULT_DB       20
#define PDM_GAIN_DEFAULT          PDM_GAIN_TO_REG(PDM_GAIN_DEFAULT_DB)
#define PDM_GAIN_MIN_DB           (-20)
#define PDM_GAIN_MAX_DB           42
#define PDM_GAIN_TO_REG(db)       ((uint8_t)(((int32_t)(db) * 2) + 40))
#define PDM_REG_TO_GAIN_DB(reg)   (((int32_t)(reg) - 40) / 2)

/* ---- dBFS constants -------------------------------------------------- */
/*
 * Full-scale int16 sine RMS = 32767/sqrt(2) ≈ 23170 → −3.01 dBFS.
 * Silence (RMS=0) → silence sentinel −96.000 dBFS.
 */
#define PDM_DBFS_SILENCE_MDBFS    (-96000)    /* −96.000 dBFS floor */
#define PDM_DBFS_FULLSCALE_MDBFS  0           /*   0.000 dBFS */

/* ---- Audio sensor ID (board_manifest.json) --------------------------- */

#define BOARD_AUDIO_SENSOR_ID     13u
#define BOARD_AUDIO_VALUE_COUNT   1u
#define BOARD_AUDIO_RATE_MAX_MHZ  PDM_PUBLISH_RATE_MHZ

/* ---- Local BoardResultCode copies (avoid board.pb.h in host tests) --- */

#define PDM_EVAL_SUCCESS          0u
#define PDM_EVAL_INVALID_ARG      2u
#define PDM_EVAL_PERMISSION       3u
#define PDM_EVAL_BUSY             6u
#define PDM_EVAL_NOT_IMPLEMENTED  14u

/* ---- Metric window state --------------------------------------------- */

typedef struct {
    uint32_t samples_collected;
    int64_t  sum_sq;           /* overflow-safe sum of x^2 */
    int32_t  peak_abs;         /* absolute peak (INT16_MIN-safe) */
    uint32_t clip_count;       /* samples at full-scale */
    uint64_t window_start_us;  /* timestamp of first sample */
} pdm_window_t;

typedef struct {
    int32_t  rms_q15;          /* Q15 RMS (0..32768) */
    int32_t  peak_abs;         /* absolute peak sample */
    uint32_t clip_count;       /* full-scale sample count */
    int32_t  dbfs_x1000;       /* milli-dBFS */
    uint64_t timestamp_us;     /* window start timestamp */
    uint32_t sample_count;     /* samples in this window */
} pdm_metrics_t;

void pdm_window_init(pdm_window_t *w);

/* Feed samples into the window. Returns true when the window is complete
 * (800 samples collected). Sets *out_consumed to the number of samples
 * consumed from the input (may be less than count if the window fills
 * mid-batch). Caller calls pdm_window_finalize() on true, then re-feeds
 * remaining samples. */
bool pdm_window_feed(pdm_window_t *w, const int16_t *samples, uint32_t count,
                     uint64_t timestamp_us, uint32_t *out_consumed);

/* Finalize the window: compute metrics and reset for the next period.
 * The window must have samples_collected == PDM_WINDOW_SAMPLES. */
void pdm_window_finalize(pdm_window_t *w, pdm_metrics_t *out);

/* ---- Pure metric functions ------------------------------------------- */

/* Compute Q15 RMS from sum-of-squares and count.
 * Returns floor(sqrt(sum_sq / count)) or 0 for count==0 / sum_sq<=0. */
int32_t pdm_metrics_rms_q15(int64_t sum_sq, uint32_t count);

/* Compute milli-dBFS from Q15 RMS value (0..32768).
 * Returns PDM_DBFS_SILENCE_MDBFS for rms_q15 <= 0.
 * Clamped to 0 for rms_q15 >= 32767. */
int32_t pdm_metrics_dbfs_x1000(int32_t rms_q15);

/* Safe absolute value for int16_t including INT16_MIN.
 * INT16_MIN → 32768 (not UB). */
int32_t pdm_abs_sample(int16_t s);

/* Check if sample is at digital full-scale (clipping). */
bool pdm_is_clipping(int16_t s);

/* ---- Threshold FSM --------------------------------------------------- */
/*
 * States: idle → armed (above arm level) → triggered (above trigger level
 * for debounce_windows consecutive windows) → released (below release level).
 * Hysteresis: release_mdbfs < arm_mdbfs prevents oscillation.
 * Missed windows (no data) increment missed_count; if it exceeds
 * missed_window_limit, the threshold is considered stale.
 */

typedef enum {
    PDM_THRESH_EVT_NONE    = 0,
    PDM_THRESH_EVT_ARM     = 1,
    PDM_THRESH_EVT_TRIGGER = 2,
    PDM_THRESH_EVT_RELEASE = 3,
} pdm_threshold_event_t;

typedef struct {
    /* Configuration (milli-dBFS, negative values) */
    int32_t  arm_mdbfs;
    int32_t  trigger_mdbfs;
    int32_t  release_mdbfs;
    uint32_t debounce_windows;
    uint32_t missed_window_limit;
    /* Runtime state */
    bool     armed;
    bool     triggered;
    uint32_t debounce_count;
    uint32_t missed_count;
} pdm_threshold_state_t;

void pdm_threshold_init(pdm_threshold_state_t *st);

/* Configure threshold parameters. */
void pdm_threshold_configure(pdm_threshold_state_t *st,
                             int32_t arm_mdbfs,
                             int32_t trigger_mdbfs,
                             int32_t release_mdbfs,
                             uint32_t debounce_windows,
                             uint32_t missed_window_limit);

/* Evaluate a metric window against the threshold FSM.
 * If window_received=false, increments missed counter.
 * Returns event type. */
pdm_threshold_event_t pdm_threshold_eval(pdm_threshold_state_t *st,
                                         int32_t dbfs_x1000,
                                         bool window_received);

/* Check if threshold FSM has exceeded missed-window limit. */
bool pdm_threshold_is_missed(const pdm_threshold_state_t *st);

/* ---- Raw PCM diagnostics sequencing ---------------------------------- */
/*
 * Raw PCM is USB-only in RAW_WHAD runtime while radio is idle.
 * Chunks are little-endian PCM16, ≤40 bytes (20 samples) per chunk.
 * Nonzero request_id correlation. Accepted result is nonterminal until
 * the final chunk (eof=true) or error.
 */

#define PDM_PCM_CHUNK_MAX_BYTES   40u
#define PDM_PCM_CHUNK_MAX_SAMPLES (PDM_PCM_CHUNK_MAX_BYTES / 2)  /* 20 */
#define PDM_PCM_MAX_DURATION_MS   10000u   /* 10s safety cap */

typedef struct {
    uint32_t request_id;
    uint32_t total_samples;    /* total samples to capture/send */
    uint32_t sent_samples;     /* samples already chunked out */
    uint32_t sequence;         /* chunk sequence number */
    uint32_t chunk_samples;    /* samples per chunk (≤20) */
    bool     active;
} pdm_pcm_state_t;

typedef struct {
    uint32_t sequence;
    uint32_t offset;           /* byte offset in stream */
    uint32_t count;            /* bytes in this chunk */
    int16_t  samples[PDM_PCM_CHUNK_MAX_SAMPLES];
    bool     eof;
    uint32_t total;            /* total bytes in stream */
    uint32_t result_code;      /* BoardResultCode (0=SUCCESS) */
} pdm_pcm_chunk_t;

void pdm_pcm_init(pdm_pcm_state_t *st);

/* Start a raw PCM diagnostics session.
 * Validates: runtime must be RAW_WHAD with radio idle, chunk_size clamped to max.
 * Returns PDM_EVAL_SUCCESS, PDM_EVAL_BUSY, or PDM_EVAL_PERMISSION. */
uint32_t pdm_pcm_start(pdm_pcm_state_t *st, runtime_mode_t mode,
                       bool radio_idle,
                       uint32_t request_id, uint32_t duration_ms,
                       uint32_t chunk_size);

/* Extract the next chunk from a PCM source buffer.
 * Returns true if a chunk was produced. Sets eof on the final chunk. */
bool pdm_pcm_next_chunk(pdm_pcm_state_t *st,
                        const int16_t *src, uint32_t src_sample_count,
                        pdm_pcm_chunk_t *out);

/* Cancel an active PCM session (idempotent). */
void pdm_pcm_cancel(pdm_pcm_state_t *st);

/* Check if raw PCM diagnostics is allowed in the given runtime mode.
 * Returns true only when mode is RAW_WHAD and the radio is idle. */
bool pdm_pcm_runtime_allowed(runtime_mode_t mode, bool radio_idle);

/* ---- AudioConfigure evaluation --------------------------------------- */
/*
 * Validates sample_rate_hz (must be 16000), gain_db_x2 (range check),
 * returns actual clamped values.
 * gain_db_x2 is requested gain in dB×2 (so +20dB → gain_db_x2=40).
 */
uint32_t pdm_eval_audio_configure(bool enabled, uint32_t sample_rate_hz,
                                  int32_t gain_db_x2, uint32_t flags,
                                  uint32_t *out_actual_rate,
                                  int32_t *out_actual_gain_db_x2);

#ifdef __cplusplus
}
#endif

#endif /* AUDIO_PDM_EVAL_H */
