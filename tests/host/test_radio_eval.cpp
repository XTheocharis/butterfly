/*
 * test_radio_eval.cpp - Host tests for radio_eval layer.
 *
 * Textually includes radio_eval.c (pure-C, no SDK deps) per the
 * project's eval/C++ split convention. Mirrors the test_buzzer.cpp and
 * test_pdm.cpp patterns.
 *
 * Covers: PHY validation + register lookup, 2-Mbit flag, TX power
 * validation + dBm/TxPower/register round-trips, frequency register
 * validation, BLE channel ↔ MHz ↔ register, CRC size + SKIPADDR + PCNF1
 * field mapping, jamming pattern matching (fixed + anywhere), buffer
 * equality, RX buffer size derivation (DOT15D4 vs length-encoded),
 * RX start-timestamp back-calculation.
 */
#include "test_framework.h"

#include "radio_eval.h"

#include <string.h>
#include <stdint.h>

#include "../../src/radio_eval.c"

/* ---- PHY validation + RADIO.MODE lookup ------------------------------ */

static void phy_supported_for_all_six_logical_phys(void)
{
	TEST_ASSERT(radio_eval_phy_supported(RADIO_EVAL_PHY_ESB_1MBITS),
		"ESB_1M supported");
	TEST_ASSERT(radio_eval_phy_supported(RADIO_EVAL_PHY_ESB_2MBITS),
		"ESB_2M supported");
	TEST_ASSERT(radio_eval_phy_supported(RADIO_EVAL_PHY_BLE_1MBITS),
		"BLE_1M supported");
	TEST_ASSERT(radio_eval_phy_supported(RADIO_EVAL_PHY_BLE_2MBITS),
		"BLE_2M supported");
	TEST_ASSERT(radio_eval_phy_supported(RADIO_EVAL_PHY_DOT15D4_NATIVE),
		"DOT15D4_NATIVE supported");
	TEST_ASSERT(radio_eval_phy_supported(RADIO_EVAL_PHY_DOT15D4_WAZABEE),
		"DOT15D4_WAZABEE supported");
}

static void phy_unsupported_for_garbage_value(void)
{
	radio_eval_phy_t bogus = (radio_eval_phy_t)42;
	TEST_ASSERT(!radio_eval_phy_supported(bogus),
		"garbage PHY rejected");
	TEST_ASSERT(!radio_eval_phy_has_mode_register(bogus),
		"garbage PHY has no MODE register");
}

static void phy_to_mode_register_matches_ps6_17_10(void)
{
	uint32_t reg = 0xFFFFFFFFu;
	TEST_ASSERT(radio_eval_phy_to_mode_register(RADIO_EVAL_PHY_ESB_1MBITS, &reg),
		"ESB_1M register lookup");
	TEST_ASSERT_EQ_INT((int)RADIO_EVAL_MODE_REG_NRF_1MBIT, (int)reg);
	TEST_ASSERT(radio_eval_phy_to_mode_register(RADIO_EVAL_PHY_BLE_1MBITS, &reg),
		"BLE_1M register lookup");
	TEST_ASSERT_EQ_INT((int)RADIO_EVAL_MODE_REG_BLE_1MBIT, (int)reg);
	TEST_ASSERT(radio_eval_phy_to_mode_register(RADIO_EVAL_PHY_DOT15D4_NATIVE, &reg),
		"DOT15D4_NATIVE register lookup");
	TEST_ASSERT_EQ_INT((int)RADIO_EVAL_MODE_REG_IEEE802154_250, (int)reg);
	TEST_ASSERT(radio_eval_phy_to_mode_register(RADIO_EVAL_PHY_DOT15D4_WAZABEE, &reg),
		"DOT15D4_WAZABEE register lookup");
	TEST_ASSERT_EQ_INT((int)RADIO_EVAL_MODE_REG_BLE_2MBIT, (int)reg);
}

static void phy_to_mode_register_rejects_garbage(void)
{
	uint32_t reg = 0;
	radio_eval_phy_t bogus = (radio_eval_phy_t)99;
	TEST_ASSERT(!radio_eval_phy_to_mode_register(bogus, &reg),
		"garbage PHY returns false");
	TEST_ASSERT(!radio_eval_phy_to_mode_register(RADIO_EVAL_PHY_BLE_1MBITS, NULL),
		"NULL reg_out returns false");
}

static void phy_is_2mbit_for_esb_and_ble_2mbit_only(void)
{
	TEST_ASSERT(radio_eval_phy_is_2mbit(RADIO_EVAL_PHY_ESB_2MBITS),
		"ESB_2M is 2Mbit");
	TEST_ASSERT(radio_eval_phy_is_2mbit(RADIO_EVAL_PHY_BLE_2MBITS),
		"BLE_2M is 2Mbit");
	TEST_ASSERT(!radio_eval_phy_is_2mbit(RADIO_EVAL_PHY_DOT15D4_WAZABEE),
		"DOT15D4_WAZABEE excluded from 2Mbit predicate (carries own framing)");
	TEST_ASSERT(!radio_eval_phy_is_2mbit(RADIO_EVAL_PHY_ESB_1MBITS),
		"ESB_1M is not 2Mbit");
	TEST_ASSERT(!radio_eval_phy_is_2mbit(RADIO_EVAL_PHY_BLE_1MBITS),
		"BLE_1M is not 2Mbit");
	TEST_ASSERT(!radio_eval_phy_is_2mbit(RADIO_EVAL_PHY_DOT15D4_NATIVE),
		"DOT15D4_NATIVE is not 2Mbit");
}

/* ---- TX power validation + dBm/TxPower/register --------------------- */

static void dbm_supported_for_all_ten_levels(void)
{
	int levels[] = {-40, -30, -20, -16, -12, -8, -4, 0, 4, 8};
	for (size_t i = 0; i < sizeof(levels)/sizeof(levels[0]); i++) {
		TEST_ASSERT(radio_eval_dbm_supported(levels[i]),
			"dBm level supported");
	}
}

static void dbm_unsupported_for_garbage(void)
{
	TEST_ASSERT(!radio_eval_dbm_supported(-100), "-100 rejected");
	TEST_ASSERT(!radio_eval_dbm_supported(-39),  "-39 rejected");
	TEST_ASSERT(!radio_eval_dbm_supported(+9),   "+9 rejected");
	TEST_ASSERT(!radio_eval_dbm_supported(+100), "+100 rejected");
}

static void dbm_txpower_round_trip_is_identity(void)
{
	int levels[] = {-40, -20, -16, -12, -8, -4, 0, 4, 8};
	for (size_t i = 0; i < sizeof(levels)/sizeof(levels[0]); i++) {
		radio_eval_txpower_t tp = radio_eval_dbm_to_txpower(levels[i]);
		TEST_ASSERT(tp != RADIO_EVAL_TXPOWER_INVALID,
			"dBm → TxPower valid");
		TEST_ASSERT_EQ_INT(levels[i], radio_eval_txpower_to_dbm(tp));
	}
}

static void txpower_to_register_matches_ps6_17_13(void)
{
	uint32_t reg = 0;
	TEST_ASSERT(radio_eval_txpower_to_register(RADIO_EVAL_NEG40_DBM, &reg),
		"-40dBm register lookup");
	TEST_ASSERT_EQ_INT((int)RADIO_EVAL_TXPOWER_REG_NEG40, (int)reg);
	TEST_ASSERT(radio_eval_txpower_to_register(RADIO_EVAL_POS0_DBM, &reg),
		"0dBm register lookup");
	TEST_ASSERT_EQ_INT((int)RADIO_EVAL_TXPOWER_REG_POS0, (int)reg);
	TEST_ASSERT(radio_eval_txpower_to_register(RADIO_EVAL_POS8_DBM, &reg),
		"+8dBm register lookup");
	TEST_ASSERT_EQ_INT((int)RADIO_EVAL_TXPOWER_REG_POS8, (int)reg);
}

static void txpower_invalid_sentinel_returns_127_dbm(void)
{
	TEST_ASSERT_EQ_INT(127,
		radio_eval_txpower_to_dbm(RADIO_EVAL_TXPOWER_INVALID));
}

/* ---- Frequency register + BLE channel math --------------------------- */

static void frequency_register_valid_at_boundaries(void)
{
	TEST_ASSERT(radio_eval_frequency_register_valid(0),   "0 valid");
	TEST_ASSERT(radio_eval_frequency_register_valid(100), "100 valid");
	TEST_ASSERT(radio_eval_frequency_register_valid(50),  "50 valid");
	TEST_ASSERT(!radio_eval_frequency_register_valid(-1), "-1 invalid");
	TEST_ASSERT(!radio_eval_frequency_register_valid(101), "101 invalid");
}

static void ble_channel_to_mhz_edges(void)
{
	TEST_ASSERT_EQ_INT(2402, radio_eval_ble_channel_to_mhz(0));
	TEST_ASSERT_EQ_INT(2480, radio_eval_ble_channel_to_mhz(39));
	TEST_ASSERT_EQ_INT(2476, radio_eval_ble_channel_to_mhz(37));
	TEST_ASSERT_EQ_INT(2478, radio_eval_ble_channel_to_mhz(38));
	TEST_ASSERT_EQ_INT(-1,   radio_eval_ble_channel_to_mhz(40));
	TEST_ASSERT_EQ_INT(-1,   radio_eval_ble_channel_to_mhz(-1));
}

static void ble_mhz_to_channel_round_trips(void)
{
	TEST_ASSERT_EQ_INT(0,  radio_eval_ble_mhz_to_channel(2402));
	TEST_ASSERT_EQ_INT(39, radio_eval_ble_mhz_to_channel(2480));
	TEST_ASSERT_EQ_INT(-1, radio_eval_ble_mhz_to_channel(2403));  /* odd */
	TEST_ASSERT_EQ_INT(-1, radio_eval_ble_mhz_to_channel(2399));  /* below band */
	TEST_ASSERT_EQ_INT(-1, radio_eval_ble_mhz_to_channel(2481));  /* above band */
}

static void ble_channel_to_freq_reg_is_mhz_minus_2400(void)
{
	TEST_ASSERT_EQ_INT(2,  radio_eval_ble_channel_to_freq_reg(0));
	TEST_ASSERT_EQ_INT(80, radio_eval_ble_channel_to_freq_reg(39));
	TEST_ASSERT_EQ_INT(-1, radio_eval_ble_channel_to_freq_reg(40));
}

/* ---- CRC field mapping ----------------------------------------------- */

static void crc_size_valid_only_zero_through_three(void)
{
	TEST_ASSERT(radio_eval_crc_size_valid(0),   "crc=0 valid");
	TEST_ASSERT(radio_eval_crc_size_valid(1),   "crc=1 valid");
	TEST_ASSERT(radio_eval_crc_size_valid(2),   "crc=2 valid");
	TEST_ASSERT(radio_eval_crc_size_valid(3),   "crc=3 valid");
	TEST_ASSERT(!radio_eval_crc_size_valid(4),  "crc=4 invalid");
	TEST_ASSERT(!radio_eval_crc_size_valid(255), "crc=255 invalid");
}

static void crc_size_to_len_direct_mapping(void)
{
	uint32_t len = 0;
	TEST_ASSERT(radio_eval_crc_size_to_len(0, &len), "crc=0 → len");
	TEST_ASSERT_EQ_INT((int)RADIO_EVAL_CRCCNF_LEN_DISABLED, (int)len);
	TEST_ASSERT(radio_eval_crc_size_to_len(3, &len), "crc=3 → len");
	TEST_ASSERT_EQ_INT((int)RADIO_EVAL_CRCCNF_LEN_THREE, (int)len);
	TEST_ASSERT(!radio_eval_crc_size_to_len(4, &len), "crc=4 rejected");
	TEST_ASSERT(!radio_eval_crc_size_to_len(3, NULL), "NULL len_out rejected");
}

static void crc_skipaddr_field_special_dot15d4_and_boolean(void)
{
	TEST_ASSERT_EQ_INT((int)RADIO_EVAL_CRCCNF_SKIPADDR_IEEE802154,
		(int)radio_eval_crc_skipaddr_field(false,
			RADIO_EVAL_PHY_DOT15D4_NATIVE));
	TEST_ASSERT_EQ_INT((int)RADIO_EVAL_CRCCNF_SKIPADDR_IEEE802154,
		(int)radio_eval_crc_skipaddr_field(true,
			RADIO_EVAL_PHY_DOT15D4_NATIVE));
	TEST_ASSERT_EQ_INT((int)RADIO_EVAL_CRCCNF_SKIPADDR_SKIP,
		(int)radio_eval_crc_skipaddr_field(true,
			RADIO_EVAL_PHY_BLE_1MBITS));
	TEST_ASSERT_EQ_INT((int)RADIO_EVAL_CRCCNF_SKIPADDR_INCLUDE,
		(int)radio_eval_crc_skipaddr_field(false,
			RADIO_EVAL_PHY_BLE_1MBITS));
}

/* ---- PCNF1 field mapping --------------------------------------------- */

static void pcnf1_whiteen_and_endian_field_mapping(void)
{
	TEST_ASSERT_EQ_INT((int)RADIO_EVAL_PCNF1_WHITEEN_ENABLED,
		(int)radio_eval_pcnf1_whiteen_field(true));
	TEST_ASSERT_EQ_INT((int)RADIO_EVAL_PCNF1_WHITEEN_DISABLED,
		(int)radio_eval_pcnf1_whiteen_field(false));
	TEST_ASSERT_EQ_INT((int)RADIO_EVAL_PCNF1_ENDIAN_LITTLE,
		(int)radio_eval_pcnf1_endian_field(true));
	TEST_ASSERT_EQ_INT((int)RADIO_EVAL_PCNF1_ENDIAN_BIG,
		(int)radio_eval_pcnf1_endian_field(false));
}

/* ---- Jamming pattern matching ---------------------------------------- */

static void jam_match_fixed_position_match_and_mismatch(void)
{
	const uint8_t buf[8]   = {0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0xFF, 0x11, 0x22};
	const uint8_t pat[2]   = {0xCC, 0xDD};
	const uint8_t mask[2]  = {0xFF, 0xFF};
	radio_eval_jam_view_t v;
	v.pattern = pat; v.mask = mask; v.size = 2; v.position = 2;
	TEST_ASSERT(radio_eval_jam_match(&v, buf, sizeof(buf)),
		"matches at fixed offset 2");
	v.position = 0;
	TEST_ASSERT(!radio_eval_jam_match(&v, buf, sizeof(buf)),
		"no match at fixed offset 0");
}

static void jam_match_anywhere_scans_buffer(void)
{
	const uint8_t buf[8]   = {0x10, 0x20, 0xAA, 0xBB, 0x30, 0x40, 0x50, 0x60};
	const uint8_t pat[2]   = {0xAA, 0xBB};
	const uint8_t mask[2]  = {0xFF, 0xFF};
	radio_eval_jam_view_t v;
	v.pattern = pat; v.mask = mask; v.size = 2;
	v.position = RADIO_EVAL_JAM_POSITION_ANYWHERE;
	TEST_ASSERT(radio_eval_jam_match(&v, buf, sizeof(buf)),
		"finds pattern at offset 2");
}

static void jam_match_with_mask_only_matches_masked_bits(void)
{
	/* buf[1] has junk low nibble (0x3) — without mask, equality fails;
	 * with mask 0xF0, only high nibble matters and matches pattern 0xB0. */
	const uint8_t buf[4]   = {0xAA, 0xB3, 0xCD, 0xEF};
	const uint8_t pat[2]   = {0xB0, 0xCD};
	const uint8_t mask[2]  = {0xF0, 0xFF};
	radio_eval_jam_view_t v;
	v.pattern = pat; v.mask = mask; v.size = 2; v.position = 1;
	TEST_ASSERT(radio_eval_jam_match(&v, buf, sizeof(buf)),
		"masked equality at offset 1");
}

static void jam_match_rejects_null_view_and_overflowing_position(void)
{
	const uint8_t buf[4] = {0};
	const uint8_t pat[2] = {0};
	const uint8_t mask[2] = {0};
	radio_eval_jam_view_t v;
	v.pattern = pat; v.mask = mask; v.size = 2; v.position = 3;  /* 3 + 2 > 4 */
	TEST_ASSERT(!radio_eval_jam_match(&v, buf, sizeof(buf)),
		"position overflow rejected");
	TEST_ASSERT(!radio_eval_jam_match(NULL, buf, sizeof(buf)),
		"NULL view rejected");
}

/* ---- Buffer equality ------------------------------------------------- */

static void buffers_equal_basic_equality(void)
{
	const uint8_t a[4] = {1, 2, 3, 4};
	const uint8_t b[4] = {1, 2, 3, 4};
	const uint8_t c[4] = {1, 2, 3, 5};
	TEST_ASSERT(radio_eval_buffers_equal(a, b, 4), "identical");
	TEST_ASSERT(!radio_eval_buffers_equal(a, c, 4), "differ in last byte");
	TEST_ASSERT(radio_eval_buffers_equal(a, a, 0),  "size=0 always equal");
}

/* ---- RX buffer size derivation --------------------------------------- */

static void compute_rx_buffer_size_dot15d4_native_is_fixed_128(void)
{
	radio_eval_header_t hdr;
	hdr.s0 = 1; hdr.length = 1; hdr.s1 = 0;
	uint8_t rx[2] = {0};
	TEST_ASSERT_EQ_INT(128u,
		(uint32_t)radio_eval_compute_rx_buffer_size(
			RADIO_EVAL_PHY_DOT15D4_NATIVE, &hdr, /*payload=*/10u, rx));
}

static void compute_rx_buffer_size_length_encoded(void)
{
	/* s0=0, length=1: size = 0(s0) + 0(s1) + 1(length byte) + rx[0]
	 * With rx[0]=20 → size = 1 + 20 = 21. */
	radio_eval_header_t hdr;
	hdr.s0 = 0; hdr.length = 1; hdr.s1 = 0;
	uint8_t rx[2] = {20, 0};
	TEST_ASSERT_EQ_INT(21u,
		(uint32_t)radio_eval_compute_rx_buffer_size(
			RADIO_EVAL_PHY_BLE_1MBITS, &hdr, /*payload=*/0u, rx));
}

static void compute_rx_buffer_size_fixed_payload_when_no_length_field(void)
{
	/* s0=1, length=0: size = 1(s0) + 0(s1) + payload(=10) = 11. */
	radio_eval_header_t hdr;
	hdr.s0 = 1; hdr.length = 0; hdr.s1 = 0;
	uint8_t rx[2] = {0};
	TEST_ASSERT_EQ_INT(11u,
		(uint32_t)radio_eval_compute_rx_buffer_size(
			RADIO_EVAL_PHY_BLE_1MBITS, &hdr, /*payload=*/10u, rx));
}

/* ---- RX start-timestamp back-calculation ----------------------------- */

static void compute_rx_start_us_subtracts_air_time_plus_offset_1mbit(void)
{
	/* 1-Mbit: air = (preamble + buffer) * 8us + 100us offset. */
	uint32_t end_us = 10000u;
	uint8_t preamble = 1;
	uint8_t buffer = 10;
	uint32_t expected = end_us - ((1u + 10u) * 8u + 100u);
	TEST_ASSERT_EQ_INT(expected,
		radio_eval_compute_rx_start_us(end_us, preamble, buffer,
			RADIO_EVAL_PHY_BLE_1MBITS));
}

static void compute_rx_start_us_2mbit_uses_4us_per_byte(void)
{
	uint32_t end_us = 5000u;
	uint8_t preamble = 1;
	uint8_t buffer = 5;
	uint32_t expected = end_us - ((1u + 5u) * 4u + 100u);
	TEST_ASSERT_EQ_INT(expected,
		radio_eval_compute_rx_start_us(end_us, preamble, buffer,
			RADIO_EVAL_PHY_BLE_2MBITS));
}

static void compute_rx_start_us_underflow_clamps_to_zero(void)
{
	/* end_us smaller than air+offset → returns 0 instead of underflowing. */
	uint32_t end_us = 50u;  /* < 100us offset alone */
	TEST_ASSERT_EQ_INT(0u,
		radio_eval_compute_rx_start_us(end_us, 0, 0,
			RADIO_EVAL_PHY_BLE_1MBITS));
}

/* ---- MAIN ------------------------------------------------------------ */

int main(void)
{
	test_framework_init();

	/* PHY */
	RUN_TEST(phy_supported_for_all_six_logical_phys);
	RUN_TEST(phy_unsupported_for_garbage_value);
	RUN_TEST(phy_to_mode_register_matches_ps6_17_10);
	RUN_TEST(phy_to_mode_register_rejects_garbage);
	RUN_TEST(phy_is_2mbit_for_esb_and_ble_2mbit_only);

	/* TX power */
	RUN_TEST(dbm_supported_for_all_ten_levels);
	RUN_TEST(dbm_unsupported_for_garbage);
	RUN_TEST(dbm_txpower_round_trip_is_identity);
	RUN_TEST(txpower_to_register_matches_ps6_17_13);
	RUN_TEST(txpower_invalid_sentinel_returns_127_dbm);

	/* Frequency / channel */
	RUN_TEST(frequency_register_valid_at_boundaries);
	RUN_TEST(ble_channel_to_mhz_edges);
	RUN_TEST(ble_mhz_to_channel_round_trips);
	RUN_TEST(ble_channel_to_freq_reg_is_mhz_minus_2400);

	/* CRC */
	RUN_TEST(crc_size_valid_only_zero_through_three);
	RUN_TEST(crc_size_to_len_direct_mapping);
	RUN_TEST(crc_skipaddr_field_special_dot15d4_and_boolean);

	/* PCNF1 */
	RUN_TEST(pcnf1_whiteen_and_endian_field_mapping);

	/* Jamming */
	RUN_TEST(jam_match_fixed_position_match_and_mismatch);
	RUN_TEST(jam_match_anywhere_scans_buffer);
	RUN_TEST(jam_match_with_mask_only_matches_masked_bits);
	RUN_TEST(jam_match_rejects_null_view_and_overflowing_position);

	/* Buffer equality */
	RUN_TEST(buffers_equal_basic_equality);

	/* RX buffer size */
	RUN_TEST(compute_rx_buffer_size_dot15d4_native_is_fixed_128);
	RUN_TEST(compute_rx_buffer_size_length_encoded);
	RUN_TEST(compute_rx_buffer_size_fixed_payload_when_no_length_field);

	/* RX start timestamp */
	RUN_TEST(compute_rx_start_us_subtracts_air_time_plus_offset_1mbit);
	RUN_TEST(compute_rx_start_us_2mbit_uses_4us_per_byte);
	RUN_TEST(compute_rx_start_us_underflow_clamps_to_zero);

	return test_framework_finish();
}
