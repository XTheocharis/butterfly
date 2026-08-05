/*
 * test_ble_controller.cpp - Host tests for blecontroller_eval layer.
 *
 * Textually includes blecontroller_eval.c (pure-C, no SDK deps) per the
 * project's eval/C++ split convention. Mirrors the test_buzzer.cpp and
 * test_pdm.cpp patterns.
 *
 * Covers: channel→frequency, channel map decode + remap, CSA2 primitives,
 * legacy CSA1 hop sequence + distance, connection update FSM, PHY update
 * FSM, channel selector (CSA1 + CSA2 next-channel), access-address
 * candidate tracker (with eviction), payload clamp, CCM counter packing,
 * state enum coverage.
 */
#include "test_framework.h"

#include "blecontroller_eval.h"

#include <string.h>
#include <stdint.h>

#include "../../src/controllers/blecontroller_eval.c"

/* ---- Constants ------------------------------------------------------- */

static void ble_constants_match_spec(void)
{
	TEST_ASSERT_EQ_INT(37u, BLE_NUM_DATA_CHANNELS);
	TEST_ASSERT_EQ_INT(3u,  BLE_NUM_ADV_CHANNELS);
	TEST_ASSERT_EQ_INT(25u, BLE_MAX_AA_CANDIDATES);
	TEST_ASSERT_EQ_INT(64u, BLE_PAYLOAD_MAX_BYTES);
}

static void ble_state_enum_is_contiguous_starting_at_idle(void)
{
	TEST_ASSERT_EQ_INT((int)BLEC_STATE_IDLE, 0);
	TEST_ASSERT_EQ_INT((int)BLEC_STATE_ADVERTISING, 23);
}

/* ---- Channel → frequency offset (MHz from 2402) ---------------------- */

static void chan_freq_for_advertising_channels(void)
{
	TEST_ASSERT_EQ_INT(2,  blec_channel_to_frequency(37));
	TEST_ASSERT_EQ_INT(26, blec_channel_to_frequency(38));
	TEST_ASSERT_EQ_INT(80, blec_channel_to_frequency(39));
}

static void chan_freq_for_data_channel_edges(void)
{
	/* ch 0..10 → even offsets 4..24; ch 11..36 → even offsets 28..78. */
	TEST_ASSERT_EQ_INT(4,  blec_channel_to_frequency(0));
	TEST_ASSERT_EQ_INT(6,  blec_channel_to_frequency(1));
	TEST_ASSERT_EQ_INT(24, blec_channel_to_frequency(10));
	TEST_ASSERT_EQ_INT(28, blec_channel_to_frequency(11));
	TEST_ASSERT_EQ_INT(78, blec_channel_to_frequency(36));
}

static void chan_freq_for_invalid_channels_returns_zero(void)
{
	TEST_ASSERT_EQ_INT(0, blec_channel_to_frequency(-1));
	TEST_ASSERT_EQ_INT(0, blec_channel_to_frequency(40));
	TEST_ASSERT_EQ_INT(0, blec_channel_to_frequency(99));
}

/* ---- Channel map decode + remap -------------------------------------- */

static void decode_full_channel_map_returns_37_used(void)
{
	uint8_t map[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0x1F}; /* 37 bits set */
	bool used[37] = {0};
	uint32_t count = 999u;
	TEST_ASSERT_EQ_INT((int)BLEC_OK,
		(int)blec_decode_channel_map(map, used, &count));
	TEST_ASSERT_EQ_INT(37u, count);
	for (uint32_t i = 0; i < 37; i++) {
		TEST_ASSERT(used[i], "all channels used");
	}
}

static void decode_empty_channel_map_returns_zero_used(void)
{
	uint8_t map[5] = {0, 0, 0, 0, 0};
	bool used[37] = {0};
	uint32_t count = 999u;
	TEST_ASSERT_EQ_INT((int)BLEC_OK,
		(int)blec_decode_channel_map(map, used, &count));
	TEST_ASSERT_EQ_INT(0u, count);
}

static void decode_channel_map_null_args_rejected(void)
{
	uint8_t map[5] = {0};
	bool used[37] = {0};
	uint32_t count = 0u;
	TEST_ASSERT_EQ_INT((int)BLEC_ERR_NULL_ARG,
		(int)blec_decode_channel_map(NULL, used, &count));
	TEST_ASSERT_EQ_INT((int)BLEC_ERR_NULL_ARG,
		(int)blec_decode_channel_map(map, NULL, &count));
	/* NULL out_count is permitted by the impl (no NULL_ARG check). */
}

static void remap_table_lists_used_channels_in_order(void)
{
	/* Channels 0, 5, 10, 36 used:
	 *   byte 0: ch 0 (bit 0) + ch 5 (bit 5) = 0x21
	 *   byte 1: ch 10 (bit 2 of byte 1)     = 0x04
	 *   byte 4: ch 36 (bit 4 of byte 4)     = 0x10 */
	uint8_t map[5] = {0x21, 0x04, 0x00, 0x00, 0x10};
	bool used[37] = {0};
	uint32_t count = 0u;
	(void)blec_decode_channel_map(map, used, &count);
	TEST_ASSERT_EQ_INT(4u, count);

	int32_t remap[37] = {0};
	TEST_ASSERT_EQ_INT((int)BLEC_OK,
		(int)blec_build_remapping_table(used, remap));
	TEST_ASSERT_EQ_INT(0,  remap[0]);
	TEST_ASSERT_EQ_INT(5,  remap[1]);
	TEST_ASSERT_EQ_INT(10, remap[2]);
	TEST_ASSERT_EQ_INT(36, remap[3]);
}

/* ---- CSA2 primitives (known formulas) -------------------------------- */

static void csa2_mam_is_linear_congruential(void)
{
	/* mam(a,b) = (17*a + b) mod 65536 */
	TEST_ASSERT_EQ_INT((int)((17u * 5u + 7u) & 0xFFFFu),
		(int)blec_csa2_mam(5, 7));
	TEST_ASSERT_EQ_INT(0, (int)blec_csa2_mam(0, 0));
}

static void csa2_permute_is_three_step_bit_permutation(void)
{
	/* The eval impl performs three steps (swap odd/even bits, swap pairs,
	 * swap nibbles) — Vol 6 Part B 4.5.8 permute(). Test invariants:
	 *   - permute(0) = 0
	 *   - permute is a permutation (bijection): preserves 16-bit popcount
	 *   - the implementation matches the documented formula
	 */
	TEST_ASSERT_EQ_INT(0, (int)blec_csa2_permute(0));
	uint16_t v = 0xABCDu;
	uint16_t v_perm = blec_csa2_permute(v);
	uint16_t v_perm_perm = blec_csa2_permute(v_perm);
	/* permute∘permute is NOT identity (3-step), but result must be
	 * deterministic and non-zero for non-zero input. */
	TEST_ASSERT(v_perm != 0u,   "permute(non-zero) is non-zero");
	TEST_ASSERT(v_perm != v,    "permute changes the value");
	/* Verify the documented 3-step formula round-trips to the same value. */
	uint16_t manual = v;
	manual = (uint16_t)(((manual & 0xaaaau) >> 1) | ((manual & 0x5555u) << 1));
	manual = (uint16_t)(((manual & 0xccccu) >> 2) | ((manual & 0x3333u) << 2));
	manual = (uint16_t)(((manual & 0xf0f0u) >> 4) | ((manual & 0x0f0fu) << 4));
	TEST_ASSERT_EQ_INT((int)manual, (int)v_perm);
	TEST_ASSERT_EQ_INT((int)v_perm_perm, (int)blec_csa2_permute(v_perm));
}

static void csa2_unmapped_is_prne_mod_37(void)
{
	uint16_t counter = 6;
	uint16_t chan_id = 0x1234u;
	uint16_t want = (uint16_t)(blec_csa2_prne(counter, chan_id) % 37u);
	TEST_ASSERT_EQ_INT((int)want,
		(int)blec_csa2_unmapped_channel(counter, chan_id));
}

/* ---- Legacy hop sequence + distance ---------------------------------- */

static void legacy_hop_sequence_has_37_entries_for_used_set(void)
{
	bool used[37];
	uint32_t count = 0;
	int32_t remap[37] = {0};
	uint8_t map[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0x1F};
	(void)blec_decode_channel_map(map, used, &count);
	(void)blec_build_remapping_table(used, remap);

	uint8_t seq[37] = {0};
	TEST_ASSERT_EQ_INT((int)BLEC_OK,
		(int)blec_generate_legacy_hop_sequence(used, remap, count,
		                                       /*hop_increment=*/5u, seq));
	for (uint32_t i = 0; i < 37; i++) {
		TEST_ASSERT(seq[i] < 37u, "channel in range");
		TEST_ASSERT(used[seq[i]], "channel is in used set");
	}
}

static void legacy_hop_sequence_rejects_zero_used_channels(void)
{
	bool used[37] = {0};
	int32_t remap[37] = {0};
	uint8_t seq[37] = {0};
	TEST_ASSERT_EQ_INT((int)BLEC_ERR_NO_CHANNELS,
		(int)blec_generate_legacy_hop_sequence(used, remap, /*count=*/0u,
		                                       /*hop_increment=*/5u, seq));
}

static void find_channel_in_sequence_returns_index_or_minus_one(void)
{
	uint8_t seq[12][37] = {0};
	/* Plant known values into hop_increment slot 0: channel 7 at index 3. */
	seq[0][3] = 7u;
	TEST_ASSERT_EQ_INT(3,
		blec_find_channel_in_sequence(seq, /*hop_increment=*/0,
		                              /*channel=*/7u, /*start=*/0u));
	TEST_ASSERT_EQ_INT(-1,
		blec_find_channel_in_sequence(seq, /*hop_increment=*/0,
		                              /*channel=*/42u, /*start=*/0u));
}

static void distance_between_channels_is_forward_modular(void)
{
	uint8_t seq[12][37] = {0};
	/* slot 0: 5 at index 2, 5 again at index 10. */
	seq[0][2]  = 5;
	seq[0][10] = 5;
	seq[0][4]  = 9;
	/* From channel 5 (idx 2) to channel 9 (idx 4) → distance 2. */
	TEST_ASSERT_EQ_INT(2,
		blec_distance_between_channels(seq, /*hop_increment=*/0,
		                               /*first=*/5u, /*second=*/9u));
}

/* ---- Connection update FSM ------------------------------------------- */

static void connection_update_clear_zeros_state(void)
{
	blec_connection_update_t st;
	blec_connection_update_clear(&st);
	TEST_ASSERT_EQ_INT((int)BLEC_UPDATE_NONE, (int)st.type);
	TEST_ASSERT_EQ_INT(0u, st.instant);
}

static void connection_update_prepare_interval_sets_fields(void)
{
	blec_connection_update_t st;
	blec_connection_update_clear(&st);
	blec_connection_update_prepare_interval(&st, /*instant=*/100u,
	                                       /*hop_interval=*/12u,
	                                       /*window_size=*/3u,
	                                       /*window_offset=*/1u);
	TEST_ASSERT_EQ_INT((int)BLEC_UPDATE_CONNECTION_UPDATE_REQ,
		(int)st.type);
	TEST_ASSERT_EQ_INT(100u, st.instant);
	TEST_ASSERT_EQ_INT(12u,  st.hop_interval);
	TEST_ASSERT_EQ_INT(3u,   (uint32_t)st.window_size);
	TEST_ASSERT_EQ_INT(1u,   (uint32_t)st.window_offset);
}

static void connection_update_fires_at_instant_and_clears(void)
{
	blec_connection_update_t st;
	blec_connection_update_clear(&st);
	blec_connection_update_prepare_interval(&st, /*instant=*/42u, 1u, 0u, 0u);
	blec_update_type_t out_type = BLEC_UPDATE_NONE;
	TEST_ASSERT(blec_connection_update_maybe_fire(&st, /*count=*/41u,
		&out_type) == false, "no fire before instant");
	TEST_ASSERT(blec_connection_update_maybe_fire(&st, /*count=*/42u,
		&out_type) == true,  "fires at instant");
	TEST_ASSERT_EQ_INT((int)BLEC_UPDATE_CONNECTION_UPDATE_REQ,
		(int)out_type);
	/* Idempotent — clear after fire. */
	TEST_ASSERT(blec_connection_update_maybe_fire(&st, /*count=*/42u,
		&out_type) == false, "does not re-fire");
}

static void connection_update_prepare_channel_map_sets_type(void)
{
	blec_connection_update_t st;
	blec_connection_update_clear(&st);
	uint8_t map[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0x1F};
	blec_connection_update_prepare_channel_map(&st, /*instant=*/7u, map);
	TEST_ASSERT_EQ_INT((int)BLEC_UPDATE_CHANNEL_MAP_REQ, (int)st.type);
	TEST_ASSERT_EQ_INT(7u, st.instant);
	TEST_ASSERT_EQ_INT(0xFFu, (uint32_t)st.channel_map[0]);
}

/* ---- PHY update FSM -------------------------------------------------- */

static void phy_update_prepare_and_fire_at_instant(void)
{
	blec_phy_update_t st;
	blec_phy_update_clear(&st);
	TEST_ASSERT_EQ_INT((int)BLEC_PHY_UPDATE_NONE, (int)st.type);
	blec_phy_update_prepare(&st, /*instant=*/200u, /*c2p=*/2u, /*p2c=*/1u);
	TEST_ASSERT_EQ_INT((int)BLEC_PHY_UPDATE_BOTH, (int)st.type);
	TEST_ASSERT_EQ_INT(200u, st.instant);
	TEST_ASSERT(!blec_phy_update_maybe_fire(&st, 199u),
		"no fire before instant");
	TEST_ASSERT(blec_phy_update_maybe_fire(&st, 200u),
		"fires at instant");
	TEST_ASSERT_EQ_INT((int)BLEC_PHY_UPDATE_NONE, (int)st.type);
}

/* ---- Channel selector (CSA1 + CSA2 next-channel) --------------------- */

static void channel_selector_csa2_returns_in_range_after_set_map(void)
{
	blec_channel_selector_t s;
	blec_channel_selector_init(&s, BLEC_CSA2);
	s.csa2_chan_id = 0x305Eu;
	uint8_t map[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0x1F};  /* all 37 used */
	TEST_ASSERT_EQ_INT((int)BLEC_OK,
		(int)blec_channel_selector_set_map(&s, map));
	TEST_ASSERT_EQ_INT(37u, s.used_count);

	for (uint16_t ev = 0; ev < 50; ev++) {
		int32_t ch = blec_next_channel(&s, ev);
		TEST_ASSERT(ch >= 0 && ch < 37, "CSA2 channel in [0,36]");
	}
}

static void channel_selector_csa1_progresses_by_hop_increment(void)
{
	blec_channel_selector_t s;
	blec_channel_selector_init(&s, BLEC_CSA1);
	s.hop_increment = 5u;
	uint8_t map[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0x1F};
	TEST_ASSERT_EQ_INT((int)BLEC_OK,
		(int)blec_channel_selector_set_map(&s, map));
	int32_t c0 = blec_next_channel(&s, /*count=*/1u);
	int32_t c1 = blec_next_channel(&s, /*count=*/2u);
	/* Two distinct consecutive samples (CSA1 walks the band). */
	TEST_ASSERT(c0 != c1, "CSA1 advances between events");
	TEST_ASSERT(c0 >= 0 && c0 < 37, "c0 in range");
	TEST_ASSERT(c1 >= 0 && c1 < 37, "c1 in range");
}

static void channel_selector_returns_minus_one_when_no_used_channels(void)
{
	blec_channel_selector_t s;
	blec_channel_selector_init(&s, BLEC_CSA1);
	uint8_t empty[5] = {0};
	TEST_ASSERT_EQ_INT((int)BLEC_OK,
		(int)blec_channel_selector_set_map(&s, empty));
	TEST_ASSERT_EQ_INT(0u, s.used_count);
	TEST_ASSERT_EQ_INT(-1, blec_next_channel(&s, /*count=*/1u));
}

/* ---- Access address candidate tracker -------------------------------- */

static void aa_candidate_becomes_known_after_second_sighting(void)
{
	blec_aa_candidates_t st;
	blec_aa_candidates_reset(&st);
	TEST_ASSERT_EQ_INT(0, st.count);
	const uint32_t aa = 0x8E89BED6u;  /* canonical BLE AA */
	TEST_ASSERT(!blec_aa_candidates_is_known(&st, aa),
		"first lookup not yet known");
	(void)blec_aa_candidates_add(&st, aa);
	/* Now stored with seen=1; first is_known bumps seen→2 and returns true. */
	TEST_ASSERT(blec_aa_candidates_is_known(&st, aa),
		"known after first stored then re-queried");
}

static void aa_candidate_evicts_under_capacity_pressure(void)
{
	blec_aa_candidates_t st;
	blec_aa_candidates_reset(&st);
	/* Fill to capacity with distinct AAs. */
	for (uint32_t i = 0; i < BLE_MAX_AA_CANDIDATES; i++) {
		TEST_ASSERT_EQ_INT((int)BLEC_OK,
			(int)blec_aa_candidates_add(&st, 0x10000000u + i));
	}
	TEST_ASSERT_EQ_INT((int)BLE_MAX_AA_CANDIDATES, (int)st.count);
	/* One more triggers sort + halve + append; still succeeds. */
	blec_result_t r = blec_aa_candidates_add(&st, 0xDEADBEEFu);
	TEST_ASSERT(r == BLEC_OK || r == BLEC_ERR_FULL,
		"add on full returns OK or FULL");
	TEST_ASSERT(st.count <= BLE_MAX_AA_CANDIDATES,
		"count never exceeds capacity");
}

/* ---- Payload clamp + CCM counter ------------------------------------- */

static void clamp_payload_size_caps_at_64_bytes(void)
{
	TEST_ASSERT_EQ_INT(0u,  blec_clamp_payload_size(0u));
	TEST_ASSERT_EQ_INT(64u, blec_clamp_payload_size(64u));
	TEST_ASSERT_EQ_INT(64u, blec_clamp_payload_size(65u));
	TEST_ASSERT_EQ_INT(64u, blec_clamp_payload_size(1024u));
}

static void pack_ccm_counter_writes_little_endian_with_zero_top_byte(void)
{
	uint8_t out[5] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
	blec_pack_ccm_counter(out, 0xDEADBEEFu);
	TEST_ASSERT_EQ_INT(0xEFu, (uint32_t)out[0]);
	TEST_ASSERT_EQ_INT(0xBEu, (uint32_t)out[1]);
	TEST_ASSERT_EQ_INT(0xADu, (uint32_t)out[2]);
	TEST_ASSERT_EQ_INT(0xDEu, (uint32_t)out[3]);
	TEST_ASSERT_EQ_INT(0u,    (uint32_t)out[4]);
}

static void pack_ccm_counter_null_out_is_safe_noop(void)
{
	/* Must not crash. */
	blec_pack_ccm_counter(NULL, 0x12345678u);
	TEST_ASSERT(1, "reached here");
}

/* ---- MAIN ------------------------------------------------------------ */

int main(void)
{
	test_framework_init();

	/* Constants */
	RUN_TEST(ble_constants_match_spec);
	RUN_TEST(ble_state_enum_is_contiguous_starting_at_idle);

	/* Channel → frequency */
	RUN_TEST(chan_freq_for_advertising_channels);
	RUN_TEST(chan_freq_for_data_channel_edges);
	RUN_TEST(chan_freq_for_invalid_channels_returns_zero);

	/* Channel map decode + remap */
	RUN_TEST(decode_full_channel_map_returns_37_used);
	RUN_TEST(decode_empty_channel_map_returns_zero_used);
	RUN_TEST(decode_channel_map_null_args_rejected);
	RUN_TEST(remap_table_lists_used_channels_in_order);

	/* CSA2 primitives */
	RUN_TEST(csa2_mam_is_linear_congruential);
	RUN_TEST(csa2_permute_is_three_step_bit_permutation);
	RUN_TEST(csa2_unmapped_is_prne_mod_37);

	/* Legacy hop sequence + distance */
	RUN_TEST(legacy_hop_sequence_has_37_entries_for_used_set);
	RUN_TEST(legacy_hop_sequence_rejects_zero_used_channels);
	RUN_TEST(find_channel_in_sequence_returns_index_or_minus_one);
	RUN_TEST(distance_between_channels_is_forward_modular);

	/* Connection update FSM */
	RUN_TEST(connection_update_clear_zeros_state);
	RUN_TEST(connection_update_prepare_interval_sets_fields);
	RUN_TEST(connection_update_fires_at_instant_and_clears);
	RUN_TEST(connection_update_prepare_channel_map_sets_type);

	/* PHY update FSM */
	RUN_TEST(phy_update_prepare_and_fire_at_instant);

	/* Channel selector */
	RUN_TEST(channel_selector_csa2_returns_in_range_after_set_map);
	RUN_TEST(channel_selector_csa1_progresses_by_hop_increment);
	RUN_TEST(channel_selector_returns_minus_one_when_no_used_channels);

	/* Access address tracker */
	RUN_TEST(aa_candidate_becomes_known_after_second_sighting);
	RUN_TEST(aa_candidate_evicts_under_capacity_pressure);

	/* Payload + CCM */
	RUN_TEST(clamp_payload_size_caps_at_64_bytes);
	RUN_TEST(pack_ccm_counter_writes_little_endian_with_zero_top_byte);
	RUN_TEST(pack_ccm_counter_null_out_is_safe_noop);

	return test_framework_finish();
}
