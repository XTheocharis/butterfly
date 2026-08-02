/*
 * pdm_eval.c - Pure-logic PDM microphone metrics pipeline.
 *
 * No SDK deps. Included from boardModule.cpp and test_pdm.cpp.
 *
 * All metric computation uses integer arithmetic only (no float).
 * dBFS is computed via integer log2 approximation with Q16 precision.
 */
#include "pdm_eval.h"

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Internal: integer square root (64-bit) -------------------------- */
/*
 * Returns floor(sqrt(v)) for v >= 0 using Newton's method.
 * Converges in at most ~32 iterations for 64-bit values.
 */
static uint32_t pdm_isqrt64(uint64_t v)
{
    if (v == 0) {
        return 0;
    }
    /* Initial guess: 2^ceil(log2(v)/2) */
    uint64_t x = v;
    uint64_t y = (x + 1u) / 2u;
    while (y < x) {
        x = y;
        y = (x + v / x) / 2u;
    }
    return (uint32_t)x;
}

/* ---- Internal: log2 fractional lookup table (Q16) -------------------- */
/*
 * 256-entry table mapping mantissa Q8 index [0..255] to the fractional
 * part of log2(1 + index/256) in Q16. Computed offline for ±1 mdBFS accuracy.
 */
static const uint16_t s_log2_frac_lut[256] = {
    0, 369, 736, 1102, 1466, 1829, 2190, 2551, 2909, 3267, 3623, 3978, 4331, 4683, 5034, 5384,
    5732, 6079, 6425, 6769, 7112, 7454, 7795, 8134, 8473, 8810, 9146, 9480, 9814, 10146, 10477, 10807,
    11136, 11464, 11791, 12116, 12440, 12764, 13086, 13407, 13727, 14046, 14363, 14680, 14996, 15310, 15624, 15937,
    16248, 16559, 16868, 17177, 17484, 17791, 18096, 18401, 18704, 19007, 19308, 19609, 19909, 20207, 20505, 20802,
    21098, 21393, 21687, 21980, 22272, 22564, 22854, 23144, 23433, 23720, 24007, 24293, 24579, 24863, 25146, 25429,
    25711, 25992, 26272, 26551, 26830, 27108, 27384, 27660, 27936, 28210, 28484, 28757, 29029, 29300, 29571, 29840,
    30109, 30378, 30645, 30912, 31178, 31443, 31707, 31971, 32234, 32496, 32758, 33019, 33279, 33538, 33797, 34055,
    34312, 34569, 34825, 35080, 35334, 35588, 35841, 36094, 36346, 36597, 36847, 37097, 37346, 37595, 37842, 38090,
    38336, 38582, 38827, 39072, 39316, 39559, 39802, 40044, 40286, 40527, 40767, 41006, 41246, 41484, 41722, 41959,
    42196, 42432, 42667, 42902, 43137, 43370, 43603, 43836, 44068, 44300, 44530, 44761, 44990, 45220, 45448, 45676,
    45904, 46131, 46357, 46583, 46809, 47034, 47258, 47482, 47705, 47928, 48150, 48372, 48593, 48813, 49034, 49253,
    49472, 49691, 49909, 50127, 50344, 50560, 50776, 50992, 51207, 51422, 51636, 51850, 52063, 52276, 52488, 52700,
    52911, 53122, 53332, 53542, 53751, 53960, 54169, 54377, 54584, 54791, 54998, 55204, 55410, 55615, 55820, 56025,
    56229, 56432, 56635, 56838, 57040, 57242, 57443, 57644, 57845, 58045, 58245, 58444, 58643, 58841, 59039, 59237,
    59434, 59631, 59827, 60023, 60219, 60414, 60609, 60803, 60997, 61190, 61384, 61576, 61769, 61961, 62152, 62343,
    62534, 62725, 62915, 63104, 63294, 63483, 63671, 63859, 64047, 64234, 64421, 64608, 64794, 64980, 65166, 65351,
};

/* ---- Internal: integer log2 in Q16 using lookup table ---------------- */

static int32_t pdm_log2_q16(uint32_t v)
{
    if (v == 0) {
        return -0x7FFFFFFF;
    }

    int32_t n = 0;
    uint32_t tmp = v;
    while (tmp > 1u) {
        n++;
        tmp >>= 1;
    }
    /* v is in [2^n, 2^(n+1)). Normalize mantissa to Q8 [256, 511]. */
    uint32_t mantissa_q8;
    if (n >= 8) {
        mantissa_q8 = v >> (n - 8);
    } else {
        mantissa_q8 = v << (8 - n);
    }
    uint32_t idx = mantissa_q8 - 256u;
    if (idx > 255u) {
        idx = 255u;
    }
    int32_t frac_q16 = (int32_t)s_log2_frac_lut[idx];

    return (n << 16) + frac_q16;
}

/* ---- Sample helpers -------------------------------------------------- */

int32_t pdm_abs_sample(int16_t s)
{
    /* INT16_MIN safe: sign-extend to int32_t, then negate.
     * -(int32_t)INT16_MIN = -(-32768) = 32768, which is valid as int32_t. */
    int32_t v = (int32_t)s;
    return (v < 0) ? -v : v;
}

bool pdm_is_clipping(int16_t s)
{
    return (s == INT16_MAX) || (s == INT16_MIN);
}

/* ---- Metric window --------------------------------------------------- */

void pdm_window_init(pdm_window_t *w)
{
    if (w == NULL) {
        return;
    }
    memset(w, 0, sizeof(*w));
}

bool pdm_window_feed(pdm_window_t *w, const int16_t *samples, uint32_t count,
                     uint64_t timestamp_us, uint32_t *out_consumed)
{
    if (w == NULL || samples == NULL || count == 0) {
        if (out_consumed != NULL) {
            *out_consumed = 0;
        }
        return false;
    }

    if (w->samples_collected == 0) {
        w->window_start_us = timestamp_us;
    }

    uint32_t remaining = PDM_WINDOW_SAMPLES - w->samples_collected;
    uint32_t to_consume = (count < remaining) ? count : remaining;

    for (uint32_t i = 0; i < to_consume; i++) {
        int32_t v = (int32_t)samples[i];
        int64_t sq = (int64_t)v * (int64_t)v;
        w->sum_sq += sq;

        int32_t absv = (v < 0) ? -v : v;
        if (absv > w->peak_abs) {
            w->peak_abs = absv;
        }

        if (samples[i] == INT16_MAX || samples[i] == INT16_MIN) {
            w->clip_count++;
        }
    }

    w->samples_collected += to_consume;

    if (out_consumed != NULL) {
        *out_consumed = to_consume;
    }

    return (w->samples_collected >= PDM_WINDOW_SAMPLES);
}

void pdm_window_finalize(pdm_window_t *w, pdm_metrics_t *out)
{
    if (w == NULL || out == NULL) {
        return;
    }

    out->rms_q15 = pdm_metrics_rms_q15(w->sum_sq, w->samples_collected);
    out->peak_abs = w->peak_abs;
    out->clip_count = w->clip_count;
    out->dbfs_x1000 = pdm_metrics_dbfs_x1000(out->rms_q15);
    out->timestamp_us = w->window_start_us;
    out->sample_count = w->samples_collected;

    /* Reset window for next period. */
    w->samples_collected = 0;
    w->sum_sq = 0;
    w->peak_abs = 0;
    w->clip_count = 0;
    w->window_start_us = 0;
}

/* ---- Pure metric functions ------------------------------------------- */

int32_t pdm_metrics_rms_q15(int64_t sum_sq, uint32_t count)
{
    if (count == 0 || sum_sq <= 0) {
        return 0;
    }

    /* mean_sq = sum_sq / count. Use unsigned division since sum_sq >= 0. */
    uint64_t usum = (uint64_t)sum_sq;
    uint64_t mean_sq = usum / count;

    /* rms = floor(sqrt(mean_sq)). */
    return (int32_t)pdm_isqrt64(mean_sq);
}

int32_t pdm_metrics_dbfs_x1000(int32_t rms_q15)
{
    if (rms_q15 <= 0) {
        return PDM_DBFS_SILENCE_MDBFS;
    }
    if (rms_q15 >= 32767) {
        return PDM_DBFS_FULLSCALE_MDBFS;
    }

    /* dBFS = 20 * log10(rms / 32767)
     *      = 20 * log2(rms / 32767) / log2(10)
     *      = 20 * log2(rms / 32767) / 3.32193
     *      = 6.0206 * log2(rms / 32767)
     *
     * milli-dBFS = 6021 * log2(rms / 32767) (integer scaled by 1001/1000)
     *
     * log2(rms / 32767) = log2(rms) - log2(32767)
     * log2(32767) ≈ 15 - epsilon ≈ 15.0 (error < 0.0001 dB)
     *
     * Using Q16 log2:
     *   mdBFS = (6021 * (log2_q16(rms) - log2_q16(32767))) >> 16
     */

    int32_t log2_rms = pdm_log2_q16((uint32_t)rms_q15);
    /* log2(32767) in Q16: ≈ 15 * 65536 = 983040 (error < 1 LSB) */
    int32_t log2_ref = 15 * 65536;

    int64_t diff_q16 = (int64_t)log2_rms - (int64_t)log2_ref;
    int64_t mdBFS = (6021LL * diff_q16) >> 16;

    /* mdBFS should be negative (rms < 32767). Clamp to 0 if rounding
     * pushed it slightly positive. */
    if (mdBFS > 0) {
        mdBFS = 0;
    }

    return (int32_t)mdBFS;
}

/* ---- Threshold FSM --------------------------------------------------- */

void pdm_threshold_init(pdm_threshold_state_t *st)
{
    if (st == NULL) {
        return;
    }
    memset(st, 0, sizeof(*st));
    /* Default config: arm at −40dB, trigger at −20dB,
     * release at −50dB, 1 window debounce, 4 missed limit. */
    st->arm_mdbfs = -40000;
    st->trigger_mdbfs = -20000;
    st->release_mdbfs = -50000;
    st->debounce_windows = 1;
    st->missed_window_limit = 4;
}

void pdm_threshold_configure(pdm_threshold_state_t *st,
                             int32_t arm_mdbfs,
                             int32_t trigger_mdbfs,
                             int32_t release_mdbfs,
                             uint32_t debounce_windows,
                             uint32_t missed_window_limit)
{
    if (st == NULL) {
        return;
    }
    st->arm_mdbfs = arm_mdbfs;
    st->trigger_mdbfs = trigger_mdbfs;
    st->release_mdbfs = release_mdbfs;
    st->debounce_windows = (debounce_windows == 0) ? 1 : debounce_windows;
    st->missed_window_limit = missed_window_limit;
}

pdm_threshold_event_t pdm_threshold_eval(pdm_threshold_state_t *st,
                                         int32_t dbfs_x1000,
                                         bool window_received)
{
    if (st == NULL) {
        return PDM_THRESH_EVT_NONE;
    }

    if (!window_received) {
        st->missed_count++;
        /* If exceeded missed limit while triggered/armed, release. */
        if (st->missed_count > st->missed_window_limit) {
            if (st->triggered || st->armed) {
                st->armed = false;
                st->triggered = false;
                st->debounce_count = 0;
                return PDM_THRESH_EVT_RELEASE;
            }
        }
        return PDM_THRESH_EVT_NONE;
    }

    /* Window received — reset missed counter. */
    st->missed_count = 0;

    /* Check release first (hysteresis: release below release_mdbfs). */
    if (dbfs_x1000 < st->release_mdbfs) {
        if (st->triggered || st->armed) {
            st->armed = false;
            st->triggered = false;
            st->debounce_count = 0;
            return PDM_THRESH_EVT_RELEASE;
        }
        return PDM_THRESH_EVT_NONE;
    }

    /* Check arm (above arm_mdbfs). */
    if (!st->armed && dbfs_x1000 >= st->arm_mdbfs) {
        st->armed = true;
        st->debounce_count = 0;
        return PDM_THRESH_EVT_ARM;
    }

    /* Check trigger (above trigger_mdbfs when armed). */
    if (st->armed && !st->triggered && dbfs_x1000 >= st->trigger_mdbfs) {
        st->debounce_count++;
        if (st->debounce_count >= st->debounce_windows) {
            st->triggered = true;
            return PDM_THRESH_EVT_TRIGGER;
        }
        return PDM_THRESH_EVT_NONE;
    }

    /* If armed but below trigger, reset debounce counter. */
    if (st->armed && dbfs_x1000 < st->trigger_mdbfs) {
        st->debounce_count = 0;
    }

    return PDM_THRESH_EVT_NONE;
}

bool pdm_threshold_is_missed(const pdm_threshold_state_t *st)
{
    if (st == NULL) {
        return false;
    }
    return st->missed_count > st->missed_window_limit;
}

/* ---- Raw PCM diagnostics sequencing ---------------------------------- */

void pdm_pcm_init(pdm_pcm_state_t *st)
{
    if (st == NULL) {
        return;
    }
    memset(st, 0, sizeof(*st));
}

bool pdm_pcm_runtime_allowed(runtime_mode_t mode, bool radio_idle)
{
    /* Raw PCM diagnostics: RAW_WHAD only, while radio is idle. */
    return (mode == RUNTIME_RAW_WHAD && radio_idle);
}

uint32_t pdm_pcm_start(pdm_pcm_state_t *st, runtime_mode_t mode,
                       bool radio_idle,
                       uint32_t request_id, uint32_t duration_ms,
                       uint32_t chunk_size)
{
    if (st == NULL) {
        return PDM_EVAL_INVALID_ARG;
    }

    /* Runtime check: raw PCM only in RAW_WHAD mode with radio idle. */
    if (!pdm_pcm_runtime_allowed(mode, radio_idle)) {
        return PDM_EVAL_PERMISSION;
    }

    /* Reject if a session is already active. */
    if (st->active) {
        return PDM_EVAL_BUSY;
    }

    /* Validate duration. */
    if (duration_ms == 0 || duration_ms > PDM_PCM_MAX_DURATION_MS) {
        return PDM_EVAL_INVALID_ARG;
    }

    /* Clamp chunk_size to max (40 bytes = 20 samples). */
    if (chunk_size == 0) {
        chunk_size = PDM_PCM_CHUNK_MAX_BYTES;
    }
    uint32_t chunk_samples = chunk_size / 2;  /* bytes → int16 samples */
    if (chunk_samples == 0) {
        chunk_samples = 1;
    }
    if (chunk_samples > PDM_PCM_CHUNK_MAX_SAMPLES) {
        chunk_samples = PDM_PCM_CHUNK_MAX_SAMPLES;
    }

    st->request_id = request_id;
    st->total_samples = duration_ms * (PDM_SAMPLE_RATE_HZ / 1000u);
    st->sent_samples = 0;
    st->sequence = 0;
    st->chunk_samples = chunk_samples;
    st->active = true;

    return PDM_EVAL_SUCCESS;
}

bool pdm_pcm_next_chunk(pdm_pcm_state_t *st,
                        const int16_t *src, uint32_t src_sample_count,
                        pdm_pcm_chunk_t *out)
{
    if (st == NULL || out == NULL || !st->active || src == NULL) {
        return false;
    }

    uint32_t remaining = st->total_samples - st->sent_samples;
    if (remaining == 0) {
        /* Nothing left — session should have ended. */
        return false;
    }

    /* Limit by available source data, chunk size, and remaining. */
    uint32_t avail = src_sample_count - st->sent_samples;
    if (avail == 0) {
        return false;
    }

    uint32_t to_send = st->chunk_samples;
    if (to_send > remaining) {
        to_send = remaining;
    }
    if (to_send > avail) {
        to_send = avail;
    }

    memset(out, 0, sizeof(*out));
    out->sequence = st->sequence;
    out->offset = st->sent_samples * 2;  /* byte offset */
    out->count = to_send * 2;             /* byte count */
    for (uint32_t i = 0; i < to_send; i++) {
        out->samples[i] = src[st->sent_samples + i];
    }
    out->total = st->total_samples * 2;   /* total bytes */
    out->result_code = PDM_EVAL_SUCCESS;

    st->sent_samples += to_send;
    st->sequence++;

    /* eof when all samples sent or all source consumed. */
    if (st->sent_samples >= st->total_samples ||
        st->sent_samples >= src_sample_count) {
        out->eof = true;
        st->active = false;
    } else {
        out->eof = false;
    }

    return true;
}

void pdm_pcm_cancel(pdm_pcm_state_t *st)
{
    if (st == NULL) {
        return;
    }
    st->active = false;
}

/* ---- AudioConfigure evaluation --------------------------------------- */

uint32_t pdm_eval_audio_configure(bool enabled, uint32_t sample_rate_hz,
                                  int32_t gain_db_x2, uint32_t flags,
                                  uint32_t *out_actual_rate,
                                  int32_t *out_actual_gain_db_x2)
{
    (void)flags;

    /* Sample rate must be 16000 (hardware-fixed). */
    if (sample_rate_hz != 0 && sample_rate_hz != PDM_SAMPLE_RATE_HZ) {
        return PDM_EVAL_INVALID_ARG;
    }

    /* Gain range check: gain_db_x2 is in dB×2, valid range ±40..84.
     * Min −20dB → gain_db_x2 = −40, Max 42dB → gain_db_x2 = 84. */
    if (gain_db_x2 != 0) {
        if (gain_db_x2 < (PDM_GAIN_MIN_DB * 2) ||
            gain_db_x2 > (PDM_GAIN_MAX_DB * 2)) {
            return PDM_EVAL_INVALID_ARG;
        }
    }

    if (out_actual_rate != NULL) {
        *out_actual_rate = PDM_SAMPLE_RATE_HZ;
    }

    if (out_actual_gain_db_x2 != NULL) {
        if (gain_db_x2 == 0) {
            /* Default: +20dB = 40 in dB×2. */
            *out_actual_gain_db_x2 = PDM_GAIN_DEFAULT_DB * 2;
        } else {
            *out_actual_gain_db_x2 = gain_db_x2;
        }
    }

    (void)enabled;  /* enabled flag handled by firmware layer */
    return PDM_EVAL_SUCCESS;
}

#ifdef __cplusplus
}
#endif
