/*
 * radio_eval.c - Pure-logic radio peripheral evaluation implementation.
 *
 * Compiles standalone with `cc -std=c11 -Wall` (no SDK headers).
 * See radio_eval.h for the API contract and the per-section source
 * line references into radio.cpp.
 *
 * allow: SIZE_OK — mirrors a single cohesive subsystem (radio.cpp
 * 1480L) and is comparable to existing eval files (pdm_eval 421 pure
 * LOC, qspi_journal_eval 657, motion_eval 383). Splitting would
 * scatter tightly-coupled nRF52840 register tables across files
 * without reducing per-reviewer working memory.
 */
#include "radio_eval.h"

/* ---- PHY validation + RADIO.MODE lookup ------------------------------ */

bool radio_eval_phy_supported(radio_eval_phy_t phy)
{
	switch (phy) {
	case RADIO_EVAL_PHY_ESB_1MBITS:
	case RADIO_EVAL_PHY_ESB_2MBITS:
	case RADIO_EVAL_PHY_BLE_1MBITS:
	case RADIO_EVAL_PHY_BLE_2MBITS:
	case RADIO_EVAL_PHY_DOT15D4_NATIVE:
	case RADIO_EVAL_PHY_DOT15D4_WAZABEE:
		return true;
	default:
		return false;
	}
}

bool radio_eval_phy_has_mode_register(radio_eval_phy_t phy)
{
	/* radio.cpp:676-700 - every supported PHY hits a case; the default
	 * branch (return false) only fires for out-of-range enum values. */
	return radio_eval_phy_supported(phy);
}

bool radio_eval_phy_to_mode_register(radio_eval_phy_t phy, uint32_t *reg_out)
{
	if (reg_out == NULL) {
		return false;
	}
	switch (phy) {
	case RADIO_EVAL_PHY_ESB_1MBITS:
		*reg_out = RADIO_EVAL_MODE_REG_NRF_1MBIT;
		return true;
	case RADIO_EVAL_PHY_ESB_2MBITS:
		*reg_out = RADIO_EVAL_MODE_REG_NRF_2MBIT;
		return true;
	case RADIO_EVAL_PHY_BLE_1MBITS:
		*reg_out = RADIO_EVAL_MODE_REG_BLE_1MBIT;
		return true;
	case RADIO_EVAL_PHY_BLE_2MBITS:
	case RADIO_EVAL_PHY_DOT15D4_WAZABEE:
		/* radio.cpp:688-691 - DOT15D4_WAZABEE shares BLE_2MBIT's
		 * radio mode (the 802.15.4 wazabee shim layers on top of the
		 * BLE 2 Mbit/s physical mode on this hardware). */
		*reg_out = RADIO_EVAL_MODE_REG_BLE_2MBIT;
		return true;
	case RADIO_EVAL_PHY_DOT15D4_NATIVE:
		*reg_out = RADIO_EVAL_MODE_REG_IEEE802154_250;
		return true;
	default:
		return false;
	}
}

bool radio_eval_phy_is_2mbit(radio_eval_phy_t phy)
{
	/* radio.cpp:1433 - 2-Mbit PHYs use 4 us/byte, others use 8 us/byte.
	 * DOT15D4_WAZABEE is treated as 1-Mbit here because radio.cpp:1433
	 * only special-cases BLE_2MBITS and ESB_2MBITS; the wazabee shim
	 * carries its own framing overhead so the radio air-time formula
	 * doesn't apply uniformly. The caller is responsible for the
	 * PHY-specific decision; this function mirrors radio.cpp:1433. */
	return phy == RADIO_EVAL_PHY_BLE_2MBITS ||
	       phy == RADIO_EVAL_PHY_ESB_2MBITS;
}

/* ---- TX power validation + lookup ------------------------------------ */

bool radio_eval_dbm_supported(int dbm)
{
	/* radio.cpp:286-323 - the case set in Radio::setTxPower(int). */
	switch (dbm) {
	case -40:
	case -30:
	case -20:
	case -16:
	case -12:
	case -8:
	case -4:
	case 0:
	case 4:
	case 8:
		return true;
	default:
		return false;
	}
}

radio_eval_txpower_t radio_eval_dbm_to_txpower(int dbm)
{
	/* radio.cpp:286-323. */
	switch (dbm) {
	case -40:
		return RADIO_EVAL_NEG40_DBM;
	case -30:
		return RADIO_EVAL_NEG30_DBM;
	case -20:
		return RADIO_EVAL_NEG20_DBM;
	case -16:
		return RADIO_EVAL_NEG16_DBM;
	case -12:
		return RADIO_EVAL_NEG12_DBM;
	case -8:
		return RADIO_EVAL_NEG8_DBM;
	case -4:
		return RADIO_EVAL_NEG4_DBM;
	case 0:
		return RADIO_EVAL_POS0_DBM;
	case 4:
		return RADIO_EVAL_POS4_DBM;
	case 8:
		return RADIO_EVAL_POS8_DBM;
	default:
		return RADIO_EVAL_TXPOWER_INVALID;
	}
}

int radio_eval_txpower_to_dbm(radio_eval_txpower_t tp)
{
	/* The TxPower enum value IS the dBm setting (radio_defs.h:44-55). */
	switch (tp) {
	case RADIO_EVAL_NEG40_DBM:
	case RADIO_EVAL_NEG30_DBM:
	case RADIO_EVAL_NEG20_DBM:
	case RADIO_EVAL_NEG16_DBM:
	case RADIO_EVAL_NEG12_DBM:
	case RADIO_EVAL_NEG8_DBM:
	case RADIO_EVAL_NEG4_DBM:
	case RADIO_EVAL_POS0_DBM:
	case RADIO_EVAL_POS4_DBM:
	case RADIO_EVAL_POS8_DBM:
		return (int)tp;
	default:
		return 127;
	}
}

bool radio_eval_txpower_to_register(radio_eval_txpower_t tp, uint32_t *reg_out)
{
	if (reg_out == NULL) {
		return false;
	}
	/* nRF52840 PS §6.17.13 TXPOWER field - matches SDK
	 * RADIO_TXPOWER_TXPOWER_* constants exactly. Source order mirrors
	 * radio.cpp:621-656. */
	switch (tp) {
	case RADIO_EVAL_NEG40_DBM:
		*reg_out = RADIO_EVAL_TXPOWER_REG_NEG40;
		return true;
	case RADIO_EVAL_NEG30_DBM:
		/* SDK marks this as deprecated and clamps to -40 dBm on
		 * silicon; we still expose it because radio.cpp:286-323
		 * accepts -30 as a valid input level. */
		*reg_out = RADIO_EVAL_TXPOWER_REG_NEG30;
		return true;
	case RADIO_EVAL_NEG20_DBM:
		*reg_out = RADIO_EVAL_TXPOWER_REG_NEG20;
		return true;
	case RADIO_EVAL_NEG16_DBM:
		*reg_out = RADIO_EVAL_TXPOWER_REG_NEG16;
		return true;
	case RADIO_EVAL_NEG12_DBM:
		*reg_out = RADIO_EVAL_TXPOWER_REG_NEG12;
		return true;
	case RADIO_EVAL_NEG8_DBM:
		*reg_out = RADIO_EVAL_TXPOWER_REG_NEG8;
		return true;
	case RADIO_EVAL_NEG4_DBM:
		*reg_out = RADIO_EVAL_TXPOWER_REG_NEG4;
		return true;
	case RADIO_EVAL_POS0_DBM:
		*reg_out = RADIO_EVAL_TXPOWER_REG_POS0;
		return true;
	case RADIO_EVAL_POS4_DBM:
		*reg_out = RADIO_EVAL_TXPOWER_REG_POS4;
		return true;
	case RADIO_EVAL_POS8_DBM:
		*reg_out = RADIO_EVAL_TXPOWER_REG_POS8;
		return true;
	default:
		return false;
	}
}

/* ---- Frequency register validation (radio.cpp:668-675) --------------- */

bool radio_eval_frequency_register_valid(int freq_reg)
{
	return freq_reg >= (int)RADIO_EVAL_FREQ_REG_MIN &&
	       freq_reg <= (int)RADIO_EVAL_FREQ_REG_MAX;
}

/* ---- BLE channel <-> frequency math ---------------------------------- */

bool radio_eval_ble_channel_valid(int channel)
{
	return channel >= (int)RADIO_EVAL_BLE_CHANNEL_MIN &&
	       channel <= (int)RADIO_EVAL_BLE_CHANNEL_MAX;
}

int radio_eval_ble_channel_to_mhz(int channel)
{
	if (!radio_eval_ble_channel_valid(channel)) {
		return -1;
	}
	/* BLE Core Vol 6, Part A, §2: channel N is at 2402 + 2*N MHz. */
	return RADIO_EVAL_BLE_FREQ_MHZ_MIN + 2 * channel;
}

int radio_eval_ble_mhz_to_channel(int freq_mhz)
{
	if ((freq_mhz & 1) != 0) {
		/* Odd MHz values are not BLE channel centers. */
		return -1;
	}
	int channel = (freq_mhz - RADIO_EVAL_BLE_FREQ_MHZ_MIN) / 2;
	if (!radio_eval_ble_channel_valid(channel)) {
		return -1;
	}
	return channel;
}

int radio_eval_ble_channel_to_freq_reg(int channel)
{
	int mhz = radio_eval_ble_channel_to_mhz(channel);
	if (mhz < 0) {
		return -1;
	}
	int reg = mhz - RADIO_EVAL_FREQ_REG_BASE_MHZ;
	/* Round-trip guard: radio.cpp:668-675 accepts 0..100. */
	if (!radio_eval_frequency_register_valid(reg)) {
		return -1;
	}
	return reg;
}

/* ---- CRC field mapping (radio.cpp:1036-1060) ------------------------- */

bool radio_eval_crc_size_valid(uint8_t crc_size)
{
	/* RADIO_EVAL_CRC_SIZE_MIN is 0 and crc_size is uint8_t, so the
	 * lower-bound check is statically true; skip it to avoid the
	 * -Wtype-limits warning the firmware's esb.c also carries. */
	return crc_size <= RADIO_EVAL_CRC_SIZE_MAX;
}

bool radio_eval_crc_size_to_len(uint8_t crc_size, uint32_t *len_out)
{
	if (len_out == NULL) {
		return false;
	}
	switch (crc_size) {
	case 0:
		*len_out = RADIO_EVAL_CRCCNF_LEN_DISABLED;
		return true;
	case 1:
		*len_out = RADIO_EVAL_CRCCNF_LEN_ONE;
		return true;
	case 2:
		*len_out = RADIO_EVAL_CRCCNF_LEN_TWO;
		return true;
	case 3:
		*len_out = RADIO_EVAL_CRCCNF_LEN_THREE;
		return true;
	default:
		return false;
	}
}

uint32_t radio_eval_crc_skipaddr_field(bool skip_address, radio_eval_phy_t phy)
{
	/* radio.cpp:1055-1060 - DOT15D4_NATIVE forces IEEE802154 mode. */
	if (phy == RADIO_EVAL_PHY_DOT15D4_NATIVE) {
		return RADIO_EVAL_CRCCNF_SKIPADDR_IEEE802154;
	}
	return skip_address ? RADIO_EVAL_CRCCNF_SKIPADDR_SKIP
	                    : RADIO_EVAL_CRCCNF_SKIPADDR_INCLUDE;
}

/* ---- PCNF1 field mapping (radio.cpp:1019-1023) ----------------------- */

uint32_t radio_eval_pcnf1_whiteen_field(bool hardware_whitening)
{
	return hardware_whitening ? RADIO_EVAL_PCNF1_WHITEEN_ENABLED
	                          : RADIO_EVAL_PCNF1_WHITEEN_DISABLED;
}

uint32_t radio_eval_pcnf1_endian_field(bool little_endian)
{
	return little_endian ? RADIO_EVAL_PCNF1_ENDIAN_LITTLE
	                     : RADIO_EVAL_PCNF1_ENDIAN_BIG;
}

/* ---- Jamming pattern matching (radio.cpp:182-202) -------------------- */

bool radio_eval_buffers_equal(const uint8_t *a, const uint8_t *b, size_t size)
{
	if (a == NULL || b == NULL) {
		/* Two NULL pointers are "equal" only when size is 0;
		 * a NULL vs non-NULL mismatch is never equal. This mirrors
		 * the firmware's implicit contract: callers always pass
		 * non-NULL pointers backed by the static jamming buffer
		 * pool (radio.cpp:9-11). */
		return size == 0 && a == b;
	}
	for (size_t i = 0; i < size; i++) {
		if (a[i] != b[i]) {
			return false;
		}
	}
	return true;
}

bool radio_eval_jam_match(const radio_eval_jam_view_t *view,
                          const uint8_t *buffer, size_t buffer_size)
{
	if (view == NULL || view->pattern == NULL || view->mask == NULL ||
	    buffer == NULL) {
		return false;
	}
	if (view->size == 0) {
		/* An empty pattern is considered a match - matches the
		 * firmware's behavior (the loop body never runs but the
		 * initial match=true is returned). */
		return true;
	}

	if (view->position == RADIO_EVAL_JAM_POSITION_ANYWHERE) {
		/* radio.cpp:183-192 - sliding scan across every offset
		 * where (offset + size) <= buffer_size. The firmware uses
		 * `pos < size-pattern->size` which underflows when
		 * buffer_size < pattern->size; we guard explicitly. */
		if (buffer_size < view->size) {
			return false;
		}
		size_t last = buffer_size - view->size;
		for (size_t pos = 0; pos <= last; pos++) {
			bool match = true;
			for (size_t i = 0; i < view->size; i++) {
				if ((buffer[pos + i] & view->mask[i]) !=
				    view->pattern[i]) {
					match = false;
					break;
				}
			}
			if (match) {
				return true;
			}
		}
		return false;
	}

	/* radio.cpp:194-200 - fixed-position match. */
	if ((size_t)view->position + view->size > buffer_size) {
		return false;
	}
	for (size_t i = 0; i < view->size; i++) {
		if ((buffer[view->position + i] & view->mask[i]) !=
		    view->pattern[i]) {
			return false;
		}
	}
	return true;
}

/* ---- RX buffer size derivation (radio.cpp:1396-1413) ----------------- */

uint8_t radio_eval_compute_rx_buffer_size(radio_eval_phy_t phy,
                                          const radio_eval_header_t *hdr,
                                          uint8_t payload_length,
                                          const uint8_t *rx_first2)
{
	if (hdr == NULL) {
		return 0;
	}

	/* radio.cpp:1397-1399 - 802.15.4 native is a fixed 128-byte max. */
	if (phy == RADIO_EVAL_PHY_DOT15D4_NATIVE) {
		return 128;
	}

	uint8_t size = 0;
	if (hdr->s0 != 0) {
		size += 1;
	}
	if (hdr->s1 != 0) {
		size += 1;
	}
	if (hdr->length != 0) {
		/* radio.cpp:1407-1408 - length-encoded payload: pick the
		 * length byte from rx[0] when s0 is absent, rx[1] when
		 * s0 is present (the length field shifts by one byte). */
		uint8_t len_byte = (hdr->s0 == 0) ? rx_first2[0] : rx_first2[1];
		size = (uint8_t)(size + 1u + len_byte);
	} else {
		/* radio.cpp:1410-1412 - fixed payload length from config. */
		size = (uint8_t)(size + payload_length);
	}
	return size;
}

/* ---- RX start-timestamp back-calculation (radio.cpp:1433) ------------ */

uint32_t radio_eval_compute_rx_start_us(uint32_t end_us,
                                        uint8_t preamble_size,
                                        uint8_t buffer_size,
                                        radio_eval_phy_t phy)
{
	/* radio.cpp:1433:
	 *   now - (preamble.size + bufferSize) * 4 * (2Mbit ? 1 : 2) - 100
	 * The arithmetic is performed in unsigned 32-bit; callers must
	 * ensure end_us is later than the air time + offset (always true
	 * for a real RX event because the radio fires END after the bytes
	 * leave the antenna). */
	uint32_t us_per_byte = radio_eval_phy_is_2mbit(phy)
	                           ? RADIO_EVAL_AIR_TIME_US_PER_BYTE_2MBIT
	                           : RADIO_EVAL_AIR_TIME_US_PER_BYTE_1MBIT;
	uint32_t air_time_us =
	    ((uint32_t)preamble_size + (uint32_t)buffer_size) * us_per_byte;
	uint32_t total_offset = air_time_us + RADIO_EVAL_END_EVENT_OFFSET_US;
	if (total_offset > end_us) {
		return 0;
	}
	return end_us - total_offset;
}
