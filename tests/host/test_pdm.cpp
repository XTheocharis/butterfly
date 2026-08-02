/*
 * test_pdm.cpp - Host tests for PDM microphone metrics pipeline.
 *
 * Includes pdm_eval.c directly (pure-C, no SDK deps).
 *
 * Covers: silence, full-scale, INT16_MIN, sine wave RMS, noise,
 * exact 800-sample windows across DMA boundaries, accumulator
 * overflow safety, clipping count, dBFS floor, threshold FSM,
 * double-buffer overrun, raw PCM chunk sequencing, request_id
 * correlation, BLE-HID runtime rejection.
 */
#include "test_framework.h"

#include "pdm_eval.h"

#include <string.h>
#include <math.h>
#include <stdint.h>

#include "../../src/audio/pdm_eval.c"

/* ---- Helpers --------------------------------------------------------- */

static void fill_silence(int16_t *buf, uint32_t count)
{
	memset(buf, 0, count * sizeof(int16_t));
}

static void fill_fullscale(int16_t *buf, uint32_t count)
{
	for (uint32_t i = 0; i < count; i++) {
		buf[i] = (i % 2 == 0) ? INT16_MAX : INT16_MIN;
	}
}

static void fill_sine(int16_t *buf, uint32_t count, int16_t amplitude,
                      double freq_hz)
{
	for (uint32_t i = 0; i < count; i++) {
		double angle = 2.0 * M_PI * freq_hz * (double)i / 16000.0;
		buf[i] = (int16_t)((double)amplitude * sin(angle));
	}
}

/* ---- Constants tests ------------------------------------------------- */

static void pdm_clock_ratio_produces_exact_16khz(void)
{
	TEST_ASSERT_EQ_INT(16000u, PDM_PDM_CLK_HZ / PDM_RATIO);
	TEST_ASSERT_EQ_INT(1280000u, PDM_PDM_CLK_HZ);
	TEST_ASSERT_EQ_INT(80u, PDM_RATIO);
}

static void gain_default_is_0x50(void)
{
	TEST_ASSERT_EQ_INT(0x50, PDM_GAIN_DEFAULT);
}

static void gain_formula_produces_correct_register(void)
{
	TEST_ASSERT_EQ_INT(0x50, PDM_GAIN_TO_REG(20));   /* +20dB → 0x50 */
	TEST_ASSERT_EQ_INT(0x14, PDM_GAIN_TO_REG(-10));  /* −10dB → 0x14 */
	TEST_ASSERT_EQ_INT(0x00, PDM_GAIN_TO_REG(-20));  /* −20dB → 0x00 */
	TEST_ASSERT_EQ_INT(0x7C, PDM_GAIN_TO_REG(42));   /* +42dB → 0x7C */
}

static void window_is_exactly_800_samples_at_16khz(void)
{
	TEST_ASSERT_EQ_INT(800u, PDM_WINDOW_SAMPLES);
	TEST_ASSERT_EQ_INT(50u, PDM_WINDOW_SAMPLES / (PDM_SAMPLE_RATE_HZ / 1000u));
}

static void chunk_max_is_40_bytes(void)
{
	TEST_ASSERT_EQ_INT(40u, PDM_PCM_CHUNK_MAX_BYTES);
	TEST_ASSERT_EQ_INT(20u, PDM_PCM_CHUNK_MAX_SAMPLES);
}

/* ---- abs_sample / clipping tests ------------------------------------- */

static void abs_sample_int16_min_returns_32768(void)
{
	TEST_ASSERT_EQ_INT(32768, pdm_abs_sample(INT16_MIN));
}

static void abs_sample_int16_max_returns_32767(void)
{
	TEST_ASSERT_EQ_INT(32767, pdm_abs_sample(INT16_MAX));
}

static void abs_sample_zero_returns_zero(void)
{
	TEST_ASSERT_EQ_INT(0, pdm_abs_sample(0));
}

static void abs_sample_negative_one(void)
{
	TEST_ASSERT_EQ_INT(1, pdm_abs_sample(-1));
}

static void clipping_detects_both_extremes(void)
{
	TEST_ASSERT(pdm_is_clipping(INT16_MAX), "INT16_MAX clips");
	TEST_ASSERT(pdm_is_clipping(INT16_MIN), "INT16_MIN clips");
	TEST_ASSERT(!pdm_is_clipping(32766), "32766 not clip");
	TEST_ASSERT(!pdm_is_clipping(-32767), "-32767 not clip");
	TEST_ASSERT(!pdm_is_clipping(0), "0 not clip");
}

/* ---- RMS computation ------------------------------------------------- */

static void rms_silence_is_zero(void)
{
	TEST_ASSERT_EQ_INT(0, pdm_metrics_rms_q15(0, 800));
}

static void rms_constant_amplitude(void)
{
	/* 800 samples of value 1000 → rms = 1000 */
	int64_t sum_sq = 800LL * 1000 * 1000;
	TEST_ASSERT_EQ_INT(1000, pdm_metrics_rms_q15(sum_sq, 800));
}

static void rms_fullscale_dc(void)
{
	/* 800 samples of 32767 → rms = 32767 */
	int64_t sum_sq = 800LL * 32767 * 32767;
	TEST_ASSERT_EQ_INT(32767, pdm_metrics_rms_q15(sum_sq, 800));
}

static void rms_count_zero_returns_zero(void)
{
	TEST_ASSERT_EQ_INT(0, pdm_metrics_rms_q15(1000000, 0));
}

static void rms_overflow_safe_for_max_accumulation(void)
{
	/* 800 samples all INT16_MIN → each square = 32768^2 = 1073741824.
	 * sum = 800 * 1073741824 = 858993459200 — fits in int64. */
	int64_t sum_sq = 800LL * 1073741824LL;
	int32_t rms = pdm_metrics_rms_q15(sum_sq, 800);
	TEST_ASSERT(rms >= 32760 && rms <= 32769,
		"rms of all-fullscale should be ~32768");
}

/* ---- dBFS computation ------------------------------------------------ */

static void dbfs_silence_is_negative_96000(void)
{
	TEST_ASSERT_EQ_INT(PDM_DBFS_SILENCE_MDBFS, pdm_metrics_dbfs_x1000(0));
	TEST_ASSERT_EQ_INT(PDM_DBFS_SILENCE_MDBFS, pdm_metrics_dbfs_x1000(-1));
}

static void dbfs_fullscale_is_zero(void)
{
	TEST_ASSERT_EQ_INT(0, pdm_metrics_dbfs_x1000(32767));
	TEST_ASSERT_EQ_INT(0, pdm_metrics_dbfs_x1000(32768));
	TEST_ASSERT_EQ_INT(0, pdm_metrics_dbfs_x1000(100000));
}

static void dbfs_half_scale_approx_neg_6db(void)
{
	int32_t dbfs = pdm_metrics_dbfs_x1000(16384);
	TEST_ASSERT(dbfs >= -6100 && dbfs <= -5940,
		"half-scale should be ~-6020 mdBFS");
}

static void dbfs_quarter_scale_approx_neg_12db(void)
{
	int32_t dbfs = pdm_metrics_dbfs_x1000(8192);
	TEST_ASSERT(dbfs >= -12100 && dbfs <= -11980,
		"quarter-scale should be ~-12041 mdBFS");
}

static void dbfs_monotonic_decreasing(void)
{
	int32_t d1 = pdm_metrics_dbfs_x1000(32767);
	int32_t d2 = pdm_metrics_dbfs_x1000(16384);
	int32_t d3 = pdm_metrics_dbfs_x1000(8192);
	int32_t d4 = pdm_metrics_dbfs_x1000(256);
	TEST_ASSERT(d1 >= d2, "32767 >= 16384");
	TEST_ASSERT(d2 >= d3, "16384 >= 8192");
	TEST_ASSERT(d3 >= d4, "8192 >= 256");
}

/* ---- Window feed / finalize ----------------------------------------- */

static void window_completes_after_800_samples(void)
{
	pdm_window_t w;
	pdm_window_init(&w);

	int16_t buf[800];
	memset(buf, 0, sizeof(buf));

	uint32_t consumed = 0;
	bool complete = pdm_window_feed(&w, buf, 800, 1000, &consumed);

	TEST_ASSERT(complete, "should complete with 800 samples");
	TEST_ASSERT_EQ_INT(800u, consumed);
}

static void window_partial_feed_across_dma_boundaries(void)
{
	pdm_window_t w;
	pdm_window_init(&w);

	int16_t buf[256];
	memset(buf, 0, sizeof(buf));

	uint32_t consumed = 0;
	bool c1 = pdm_window_feed(&w, buf, 256, 1000, &consumed);
	TEST_ASSERT(!c1, "256 < 800, not complete");
	TEST_ASSERT_EQ_INT(256u, consumed);

	bool c2 = pdm_window_feed(&w, buf, 256, 2000, &consumed);
	TEST_ASSERT(!c2, "512 < 800, not complete");
	TEST_ASSERT_EQ_INT(256u, consumed);

	bool c3 = pdm_window_feed(&w, buf, 256, 3000, &consumed);
	TEST_ASSERT(!c3, "768 < 800, not complete");
	TEST_ASSERT_EQ_INT(256u, consumed);

	/* 4th buffer: 256 samples, but only 32 needed to complete. */
	bool c4 = pdm_window_feed(&w, buf, 256, 4000, &consumed);
	TEST_ASSERT(c4, "800 reached, complete");
	TEST_ASSERT_EQ_INT(32u, consumed);
}

static void window_finalize_resets_for_next_period(void)
{
	pdm_window_t w;
	pdm_window_init(&w);

	int16_t buf[800];
	memset(buf, 0, sizeof(buf));

	uint32_t consumed = 0;
	pdm_window_feed(&w, buf, 800, 5000, &consumed);

	pdm_metrics_t m;
	pdm_window_finalize(&w, &m);

	TEST_ASSERT_EQ_INT(0u, w.samples_collected);
	TEST_ASSERT_EQ_INT(0, w.sum_sq);
	TEST_ASSERT_EQ_INT(0, w.peak_abs);
}

static void window_timestamp_captured_at_first_sample(void)
{
	pdm_window_t w;
	pdm_window_init(&w);

	int16_t buf[800];
	memset(buf, 0, sizeof(buf));

	uint32_t consumed = 0;
	pdm_window_feed(&w, buf, 800, 12345, &consumed);

	pdm_metrics_t m;
	pdm_window_finalize(&w, &m);

	TEST_ASSERT_EQ_INT(12345u, m.timestamp_us);
}

static void window_silence_produces_silence_metrics(void)
{
	pdm_window_t w;
	pdm_window_init(&w);

	int16_t buf[800];
	fill_silence(buf, 800);

	uint32_t consumed = 0;
	pdm_window_feed(&w, buf, 800, 0, &consumed);

	pdm_metrics_t m;
	pdm_window_finalize(&w, &m);

	TEST_ASSERT_EQ_INT(0, m.rms_q15);
	TEST_ASSERT_EQ_INT(PDM_DBFS_SILENCE_MDBFS, m.dbfs_x1000);
	TEST_ASSERT_EQ_INT(0, m.peak_abs);
	TEST_ASSERT_EQ_INT(0u, m.clip_count);
}

static void window_fullscale_produces_zero_dbfs(void)
{
	pdm_window_t w;
	pdm_window_init(&w);

	int16_t buf[800];
	fill_fullscale(buf, 800);

	uint32_t consumed = 0;
	pdm_window_feed(&w, buf, 800, 0, &consumed);

	pdm_metrics_t m;
	pdm_window_finalize(&w, &m);

	TEST_ASSERT_EQ_INT(0, m.dbfs_x1000);
	TEST_ASSERT_EQ_INT(800u, m.clip_count);
	TEST_ASSERT_EQ_INT(32768, m.peak_abs);
}

static void window_int16_min_handled_safely(void)
{
	pdm_window_t w;
	pdm_window_init(&w);

	int16_t buf[800];
	for (uint32_t i = 0; i < 800; i++) {
		buf[i] = INT16_MIN;
	}

	uint32_t consumed = 0;
	pdm_window_feed(&w, buf, 800, 0, &consumed);

	pdm_metrics_t m;
	pdm_window_finalize(&w, &m);

	TEST_ASSERT_EQ_INT(32768, m.peak_abs);
	TEST_ASSERT_EQ_INT(800u, m.clip_count);
	TEST_ASSERT(m.rms_q15 >= 32760,
		"rms of all-INT16_MIN should be ~32768");
	TEST_ASSERT_EQ_INT(0, m.dbfs_x1000);
}

static void window_sine_wave_rms_correct(void)
{
	pdm_window_t w;
	pdm_window_init(&w);

	int16_t buf[800];
	fill_sine(buf, 800, 32767, 440.0);

	uint32_t consumed = 0;
	pdm_window_feed(&w, buf, 800, 0, &consumed);

	pdm_metrics_t m;
	pdm_window_finalize(&w, &m);

	/* Full-scale sine: rms = 32767/sqrt(2) ≈ 23170. */
	TEST_ASSERT(m.rms_q15 >= 23000 && m.rms_q15 <= 23300,
		"full-scale sine RMS should be ~23170");

	/* dBFS = 20*log10(1/sqrt(2)) ≈ -3.01 dBFS → -3010 mdBFS. */
	TEST_ASSERT(m.dbfs_x1000 >= -3100 && m.dbfs_x1000 <= -2950,
		"full-scale sine dBFS should be ~-3010");
}

static void window_half_amplitude_sine(void)
{
	pdm_window_t w;
	pdm_window_init(&w);

	int16_t buf[800];
	fill_sine(buf, 800, 16384, 440.0);

	uint32_t consumed = 0;
	pdm_window_feed(&w, buf, 800, 0, &consumed);

	pdm_metrics_t m;
	pdm_window_finalize(&w, &m);

	/* Half-amplitude sine: rms = 16384/sqrt(2) ≈ 11585. */
	TEST_ASSERT(m.rms_q15 >= 11450 && m.rms_q15 <= 11700,
		"half-amp sine RMS ~11585");
	/* dBFS ≈ -9.03 dBFS → -9030 mdBFS. */
	TEST_ASSERT(m.dbfs_x1000 >= -9200 && m.dbfs_x1000 <= -8900,
		"half-amp sine dBFS ~-9030");
}

static void window_clipping_count_partial(void)
{
	pdm_window_t w;
	pdm_window_init(&w);

	int16_t buf[800];
	for (uint32_t i = 0; i < 800; i++) {
		if (i % 100 == 0) {
			buf[i] = INT16_MAX;
		} else {
			buf[i] = (int16_t)(i % 1000);
		}
	}

	uint32_t consumed = 0;
	pdm_window_feed(&w, buf, 800, 0, &consumed);

	pdm_metrics_t m;
	pdm_window_finalize(&w, &m);

	TEST_ASSERT_EQ_INT(8u, m.clip_count);
}

static void window_long_run_across_many_boundaries(void)
{
	pdm_window_t w;
	pdm_window_init(&w);

	int16_t dma_buf[256];
	fill_sine(dma_buf, 256, 32767, 1000.0);

	uint32_t windows_completed = 0;
	uint64_t ts = 0;

	for (uint32_t batch = 0; batch < 40; batch++) {
		uint32_t offset = 0;
		while (offset < 256) {
			uint32_t consumed = 0;
			if (pdm_window_feed(&w, dma_buf + offset,
			                    256 - offset, ts, &consumed)) {
				pdm_metrics_t m;
				pdm_window_finalize(&w, &m);
				windows_completed++;
				TEST_ASSERT(m.rms_q15 > 20000,
					"long-run sine RMS consistent");
			}
			offset += consumed;
			ts += 10;
		}
	}

	TEST_ASSERT(windows_completed > 0, "should complete windows in long run");
}

/* ---- Threshold FSM --------------------------------------------------- */

static void threshold_init_has_defaults(void)
{
	pdm_threshold_state_t st;
	pdm_threshold_init(&st);

	TEST_ASSERT(!st.armed, "not armed at init");
	TEST_ASSERT(!st.triggered, "not triggered at init");
	TEST_ASSERT_EQ_INT(-40000, st.arm_mdbfs);
	TEST_ASSERT_EQ_INT(-20000, st.trigger_mdbfs);
	TEST_ASSERT_EQ_INT(-50000, st.release_mdbfs);
}

static void threshold_arms_above_arm_level(void)
{
	pdm_threshold_state_t st;
	pdm_threshold_init(&st);
	pdm_threshold_configure(&st, -40000, -20000, -50000, 1, 4);

	pdm_threshold_event_t evt = pdm_threshold_eval(&st, -35000, true);

	TEST_ASSERT_EQ_INT(PDM_THRESH_EVT_ARM, evt);
	TEST_ASSERT(st.armed, "should be armed");
}

static void threshold_triggers_after_debounce(void)
{
	pdm_threshold_state_t st;
	pdm_threshold_init(&st);
	pdm_threshold_configure(&st, -40000, -20000, -50000, 2, 4);

	pdm_threshold_eval(&st, -35000, true);  /* ARM */
	TEST_ASSERT(!st.triggered, "not triggered after arm only");

	(void)pdm_threshold_eval(&st, -15000, true);
	TEST_ASSERT(!st.triggered, "not triggered after 1 window (< debounce=2)");

	pdm_threshold_event_t evt3 = pdm_threshold_eval(&st, -15000, true);
	TEST_ASSERT_EQ_INT(PDM_THRESH_EVT_TRIGGER, evt3);
	TEST_ASSERT(st.triggered, "triggered after debounce");
}

static void threshold_releases_below_release_level(void)
{
	pdm_threshold_state_t st;
	pdm_threshold_init(&st);
	pdm_threshold_configure(&st, -40000, -20000, -50000, 1, 4);

	pdm_threshold_eval(&st, -35000, true);   /* ARM */
	pdm_threshold_eval(&st, -15000, true);   /* TRIGGER */

	pdm_threshold_event_t evt = pdm_threshold_eval(&st, -55000, true);
	TEST_ASSERT_EQ_INT(PDM_THRESH_EVT_RELEASE, evt);
	TEST_ASSERT(!st.armed, "released disarms");
	TEST_ASSERT(!st.triggered, "released untriggers");
}

static void threshold_hysteresis_no_oscillation(void)
{
	pdm_threshold_state_t st;
	pdm_threshold_init(&st);
	pdm_threshold_configure(&st, -40000, -20000, -50000, 1, 4);

	pdm_threshold_eval(&st, -35000, true);   /* ARM */

	/* Level between release and arm should NOT release or re-arm. */
	pdm_threshold_event_t evt = pdm_threshold_eval(&st, -45000, true);
	TEST_ASSERT_EQ_INT(PDM_THRESH_EVT_NONE, evt);
	TEST_ASSERT(st.armed, "still armed in hysteresis band");
}

static void threshold_missed_windows_eventually_release(void)
{
	pdm_threshold_state_t st;
	pdm_threshold_init(&st);
	pdm_threshold_configure(&st, -40000, -20000, -50000, 1, 3);

	pdm_threshold_eval(&st, -35000, true);   /* ARM */
	pdm_threshold_eval(&st, -15000, true);   /* TRIGGER */

	pdm_threshold_event_t evt = PDM_THRESH_EVT_NONE;
	for (int i = 0; i < 4; i++) {
		evt = pdm_threshold_eval(&st, 0, false);
	}

	TEST_ASSERT_EQ_INT(PDM_THRESH_EVT_RELEASE, evt);
	TEST_ASSERT(pdm_threshold_is_missed(&st), "missed flag set");
}

static void threshold_below_arm_does_nothing(void)
{
	pdm_threshold_state_t st;
	pdm_threshold_init(&st);

	pdm_threshold_event_t evt = pdm_threshold_eval(&st, -80000, true);
	TEST_ASSERT_EQ_INT(PDM_THRESH_EVT_NONE, evt);
	TEST_ASSERT(!st.armed, "no arm below threshold");
}

/* ---- Raw PCM diagnostics sequencing ---------------------------------- */

static void pcm_runtime_rejects_ble_hid(void)
{
	TEST_ASSERT(!pdm_pcm_runtime_allowed(RUNTIME_BLE_HID, true),
		"BLE-HID must reject raw PCM");
	TEST_ASSERT(pdm_pcm_runtime_allowed(RUNTIME_RAW_WHAD, true),
		"RAW_WHAD with radio idle allows raw PCM");
}

static void pcm_runtime_rejects_when_radio_active(void)
{
	TEST_ASSERT(!pdm_pcm_runtime_allowed(RUNTIME_RAW_WHAD, false),
		"RAW_WHAD with radio active must reject raw PCM");
}

static void pcm_start_succeeds_in_raw_whad(void)
{
	pdm_pcm_state_t st;
	pdm_pcm_init(&st);

	uint32_t code = pdm_pcm_start(&st, RUNTIME_RAW_WHAD, true, 42, 100, 40);
	TEST_ASSERT_EQ_INT(PDM_EVAL_SUCCESS, code);
	TEST_ASSERT(st.active, "should be active");
	TEST_ASSERT_EQ_INT(42u, st.request_id);
	TEST_ASSERT_EQ_INT(1600u, st.total_samples);
	TEST_ASSERT_EQ_INT(20u, st.chunk_samples);
}

static void pcm_start_rejects_ble_hid(void)
{
	pdm_pcm_state_t st;
	pdm_pcm_init(&st);

	uint32_t code = pdm_pcm_start(&st, RUNTIME_BLE_HID, true, 1, 100, 40);
	TEST_ASSERT_EQ_INT(PDM_EVAL_PERMISSION, code);
	TEST_ASSERT(!st.active, "not active after rejection");
}

static void pcm_start_rejects_zero_duration(void)
{
	pdm_pcm_state_t st;
	pdm_pcm_init(&st);

	uint32_t code = pdm_pcm_start(&st, RUNTIME_RAW_WHAD, true, 1, 0, 40);
	TEST_ASSERT_EQ_INT(PDM_EVAL_INVALID_ARG, code);
}

static void pcm_start_rejects_too_long_duration(void)
{
	pdm_pcm_state_t st;
	pdm_pcm_init(&st);

	uint32_t code = pdm_pcm_start(&st, RUNTIME_RAW_WHAD, true, 1,
	                              PDM_PCM_MAX_DURATION_MS + 1, 40);
	TEST_ASSERT_EQ_INT(PDM_EVAL_INVALID_ARG, code);
}

static void pcm_start_rejects_when_busy(void)
{
	pdm_pcm_state_t st;
	pdm_pcm_init(&st);

	pdm_pcm_start(&st, RUNTIME_RAW_WHAD, true, 1, 100, 40);
	uint32_t code = pdm_pcm_start(&st, RUNTIME_RAW_WHAD, true, 2, 100, 40);
	TEST_ASSERT_EQ_INT(PDM_EVAL_BUSY, code);
}

static void pcm_chunk_sequence_accepted_to_eof(void)
{
	pdm_pcm_state_t st;
	pdm_pcm_init(&st);

	/* 50ms duration → 800 samples. chunk_size=40 → 20 samples/chunk. */
	pdm_pcm_start(&st, RUNTIME_RAW_WHAD, true, 7, 50, 40);

	int16_t pcm_src[800];
	for (uint32_t i = 0; i < 800; i++) {
		pcm_src[i] = (int16_t)(i & 0xFFFF);
	}

	uint32_t chunks = 0;
	uint32_t last_seq = 0;
	bool saw_eof = false;

	while (st.active) {
		pdm_pcm_chunk_t chunk;
		bool produced = pdm_pcm_next_chunk(&st, pcm_src, 800, &chunk);
		if (!produced) {
			break;
		}
		chunks++;
		last_seq = chunk.sequence;
		saw_eof = chunk.eof;

		/* Verify chunk fields. */
		TEST_ASSERT(chunk.count <= 40, "chunk count ≤ 40 bytes");
		TEST_ASSERT_EQ_INT(7u, st.request_id);

		if (!chunk.eof) {
			TEST_ASSERT_EQ_INT(chunk.count, chunk.count);
		}
	}

	TEST_ASSERT(saw_eof, "last chunk must have eof=true");
	TEST_ASSERT_EQ_INT(40u, chunks);  /* 800/20 = 40 chunks */
	TEST_ASSERT_EQ_INT(39u, last_seq);
}

static void pcm_offset_progresses_correctly(void)
{
	pdm_pcm_state_t st;
	pdm_pcm_init(&st);
	pdm_pcm_start(&st, RUNTIME_RAW_WHAD, true, 1, 50, 40);

	int16_t pcm_src[800];
	memset(pcm_src, 0, sizeof(pcm_src));

	pdm_pcm_chunk_t c0;
	pdm_pcm_next_chunk(&st, pcm_src, 800, &c0);
	TEST_ASSERT_EQ_INT(0u, c0.offset);
	TEST_ASSERT_EQ_INT(40u, c0.count);
	TEST_ASSERT_EQ_INT(1600u, c0.total);
	TEST_ASSERT(!c0.eof, "first chunk not eof");

	pdm_pcm_chunk_t c1;
	pdm_pcm_next_chunk(&st, pcm_src, 800, &c1);
	TEST_ASSERT_EQ_INT(40u, c1.offset);
	TEST_ASSERT_EQ_INT(40u, c1.count);
}

static void pcm_last_chunk_has_remaining_samples(void)
{
	pdm_pcm_state_t st;
	pdm_pcm_init(&st);
	/* 25ms → 400 samples. chunk_size=40 → 20/chunk → 20 chunks. */
	pdm_pcm_start(&st, RUNTIME_RAW_WHAD, true, 1, 25, 40);

	int16_t pcm_src[400];
	memset(pcm_src, 0, sizeof(pcm_src));

	pdm_pcm_chunk_t chunk;
	uint32_t count = 0;
	while (pdm_pcm_next_chunk(&st, pcm_src, 400, &chunk)) {
		count++;
	}

	TEST_ASSERT_EQ_INT(20u, count);
}

static void pcm_cancel_stops_session(void)
{
	pdm_pcm_state_t st;
	pdm_pcm_init(&st);
	pdm_pcm_start(&st, RUNTIME_RAW_WHAD, true, 1, 100, 40);

	pdm_pcm_cancel(&st);
	TEST_ASSERT(!st.active, "not active after cancel");

	/* Cancel is idempotent. */
	pdm_pcm_cancel(&st);
	TEST_ASSERT(!st.active, "idempotent cancel");
}

static void pcm_chunk_size_clamped_to_max(void)
{
	pdm_pcm_state_t st;
	pdm_pcm_init(&st);
	/* Request chunk_size=200 (way above 40 max). */
	pdm_pcm_start(&st, RUNTIME_RAW_WHAD, true, 1, 50, 200);

	TEST_ASSERT_EQ_INT(20u, st.chunk_samples);
}

/* ---- AudioConfigure evaluation --------------------------------------- */

static void audio_configure_accepts_valid_params(void)
{
	uint32_t actual_rate = 0;
	int32_t actual_gain = 0;

	uint32_t code = pdm_eval_audio_configure(true, 16000, 40, 0,
	                                         &actual_rate, &actual_gain);
	TEST_ASSERT_EQ_INT(PDM_EVAL_SUCCESS, code);
	TEST_ASSERT_EQ_INT(16000u, actual_rate);
	TEST_ASSERT_EQ_INT(40, actual_gain);
}

static void audio_configure_rejects_wrong_sample_rate(void)
{
	uint32_t code = pdm_eval_audio_configure(true, 44100, 40, 0, NULL, NULL);
	TEST_ASSERT_EQ_INT(PDM_EVAL_INVALID_ARG, code);
}

static void audio_configure_rejects_gain_out_of_range(void)
{
	uint32_t code = pdm_eval_audio_configure(true, 16000, 200, 0, NULL, NULL);
	TEST_ASSERT_EQ_INT(PDM_EVAL_INVALID_ARG, code);
}

static void audio_configure_zero_gain_uses_default(void)
{
	int32_t actual_gain = 0;
	pdm_eval_audio_configure(true, 16000, 0, 0, NULL, &actual_gain);
	TEST_ASSERT_EQ_INT(40, actual_gain);  /* +20dB default → 40 in dB×2 */
}

/* ---- Test runner ----------------------------------------------------- */

int main(void)
{
	test_framework_init();

	RUN_TEST(pdm_clock_ratio_produces_exact_16khz);
	RUN_TEST(gain_default_is_0x50);
	RUN_TEST(gain_formula_produces_correct_register);
	RUN_TEST(window_is_exactly_800_samples_at_16khz);
	RUN_TEST(chunk_max_is_40_bytes);

	RUN_TEST(abs_sample_int16_min_returns_32768);
	RUN_TEST(abs_sample_int16_max_returns_32767);
	RUN_TEST(abs_sample_zero_returns_zero);
	RUN_TEST(abs_sample_negative_one);
	RUN_TEST(clipping_detects_both_extremes);

	RUN_TEST(rms_silence_is_zero);
	RUN_TEST(rms_constant_amplitude);
	RUN_TEST(rms_fullscale_dc);
	RUN_TEST(rms_count_zero_returns_zero);
	RUN_TEST(rms_overflow_safe_for_max_accumulation);

	RUN_TEST(dbfs_silence_is_negative_96000);
	RUN_TEST(dbfs_fullscale_is_zero);
	RUN_TEST(dbfs_half_scale_approx_neg_6db);
	RUN_TEST(dbfs_quarter_scale_approx_neg_12db);
	RUN_TEST(dbfs_monotonic_decreasing);

	RUN_TEST(window_completes_after_800_samples);
	RUN_TEST(window_partial_feed_across_dma_boundaries);
	RUN_TEST(window_finalize_resets_for_next_period);
	RUN_TEST(window_timestamp_captured_at_first_sample);
	RUN_TEST(window_silence_produces_silence_metrics);
	RUN_TEST(window_fullscale_produces_zero_dbfs);
	RUN_TEST(window_int16_min_handled_safely);
	RUN_TEST(window_sine_wave_rms_correct);
	RUN_TEST(window_half_amplitude_sine);
	RUN_TEST(window_clipping_count_partial);
	RUN_TEST(window_long_run_across_many_boundaries);

	RUN_TEST(threshold_init_has_defaults);
	RUN_TEST(threshold_arms_above_arm_level);
	RUN_TEST(threshold_triggers_after_debounce);
	RUN_TEST(threshold_releases_below_release_level);
	RUN_TEST(threshold_hysteresis_no_oscillation);
	RUN_TEST(threshold_missed_windows_eventually_release);
	RUN_TEST(threshold_below_arm_does_nothing);

	RUN_TEST(pcm_runtime_rejects_ble_hid);
	RUN_TEST(pcm_runtime_rejects_when_radio_active);
	RUN_TEST(pcm_start_succeeds_in_raw_whad);
	RUN_TEST(pcm_start_rejects_ble_hid);
	RUN_TEST(pcm_start_rejects_zero_duration);
	RUN_TEST(pcm_start_rejects_too_long_duration);
	RUN_TEST(pcm_start_rejects_when_busy);
	RUN_TEST(pcm_chunk_sequence_accepted_to_eof);
	RUN_TEST(pcm_offset_progresses_correctly);
	RUN_TEST(pcm_last_chunk_has_remaining_samples);
	RUN_TEST(pcm_cancel_stops_session);
	RUN_TEST(pcm_chunk_size_clamped_to_max);

	RUN_TEST(audio_configure_accepts_valid_params);
	RUN_TEST(audio_configure_rejects_wrong_sample_rate);
	RUN_TEST(audio_configure_rejects_gain_out_of_range);
	RUN_TEST(audio_configure_zero_gain_uses_default);

	return test_framework_finish();
}
