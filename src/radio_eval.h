/*
 * radio_eval.h - Pure-logic radio peripheral evaluation.
 *
 * No SDK deps - safe for host tests. Included from radio.cpp
 * (firmware, where it shares lookup tables with the SDK-dependent
 * register writes) and test_radio.cpp (host test).
 *
 * Provides:
 *   - TX power validation + dBm <-> TxPower enum + nRF52840 TXPOWER
 *     register field table (per nRF52840 PS §6.17.13)
 *   - PHY mode validation + nRF52840 RADIO.MODE register field table
 *     (per nRF52840 PS §6.17.10 MODE register)
 *   - Frequency register validation (0..100 = 2400..2500 MHz)
 *   - BLE channel number <-> frequency in MHz (2402 + 2*ch, 0..39)
 *   - BLE channel <-> radio FREQUENCY register value (2 + 2*ch)
 *   - CRC size validation + nRF52840 CRCCNF.LEN field mapping
 *   - CRC skip-address field mapping (CRCCNF.SKIPADDR)
 *   - PCNF1.WHITEEN / PCNF1.ENDIAN field mapping
 *   - Jamming pattern matching (fixed-position + "anywhere" / 0xFF)
 *   - RX buffer size derivation from header fields (matches ISR path)
 *   - RX start-timestamp back-calculation from END timestamp
 *
 * Mirrors radio_defs.h values where the firmware enum is already
 * SDK-free; copied (not #included) so the eval layer is self-contained
 * for host tests, per the project's eval/C++ split convention
 * (see buzzer_eval.h).
 */
#ifndef RADIO_EVAL_H
#define RADIO_EVAL_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Mirrored Phy enum (matches radio_defs.h exactly) ---------------- */
/* DOT15D4_WAZABEE uses the BLE 2Mbit radio mode on this hardware
 * (see radio.cpp:688-691); it is treated as a distinct logical PHY
 * that shares the same RADIO.MODE register value as BLE_2MBITS. */
typedef enum {
	RADIO_EVAL_PHY_ESB_1MBITS        = 0,
	RADIO_EVAL_PHY_ESB_2MBITS        = 1,
	RADIO_EVAL_PHY_BLE_1MBITS        = 2,
	RADIO_EVAL_PHY_BLE_2MBITS        = 3,
	RADIO_EVAL_PHY_DOT15D4_NATIVE    = 4,
	RADIO_EVAL_PHY_DOT15D4_WAZABEE   = 5
} radio_eval_phy_t;

#define RADIO_EVAL_PHY_COUNT          6u
#define RADIO_EVAL_PHY_INVALID        ((radio_eval_phy_t)-1)

/* ---- Mirrored TxPower enum (matches radio_defs.h) -------------------- */
/* The signed enum value IS the dBm setting; the nRF52840 register
 * field is a separate unsigned 8-bit code exposed by
 * radio_eval_txpower_to_register(). */
typedef enum {
	RADIO_EVAL_NEG40_DBM  = -40,
	RADIO_EVAL_NEG30_DBM  = -30, /* deprecated alias for -40 on nRF52840 */
	RADIO_EVAL_NEG20_DBM  = -20,
	RADIO_EVAL_NEG16_DBM  = -16,
	RADIO_EVAL_NEG12_DBM  = -12,
	RADIO_EVAL_NEG8_DBM   = -8,
	RADIO_EVAL_NEG4_DBM   = -4,
	RADIO_EVAL_POS0_DBM   = 0,
	RADIO_EVAL_POS4_DBM   = 4,
	RADIO_EVAL_POS8_DBM   = 8
} radio_eval_txpower_t;

#define RADIO_EVAL_TXPOWER_INVALID    ((radio_eval_txpower_t)127)

/* ---- nRF52840 register field value tables ---------------------------- *
 * Source: nRF52840 PS §6.17 (radio peripheral register descriptions).
 * These match the SDK's nrf52840_bitfields.h constants exactly; they
 * are reproduced here so host tests can verify register computations
 * without pulling in the nRF5 SDK headers. */

/* RADIO.TXPOWER field (8-bit, offset 0) - PS §6.17.13 */
#define RADIO_EVAL_TXPOWER_REG_NEG40   0xD8u
#define RADIO_EVAL_TXPOWER_REG_NEG30   0xE2u  /* deprecated; rounds to -40 dBm */
#define RADIO_EVAL_TXPOWER_REG_NEG20   0xECu
#define RADIO_EVAL_TXPOWER_REG_NEG16   0xF0u
#define RADIO_EVAL_TXPOWER_REG_NEG12   0xF4u
#define RADIO_EVAL_TXPOWER_REG_NEG8    0xF8u
#define RADIO_EVAL_TXPOWER_REG_NEG4    0xFCu
#define RADIO_EVAL_TXPOWER_REG_POS0    0x00u
#define RADIO_EVAL_TXPOWER_REG_POS4    0x04u
#define RADIO_EVAL_TXPOWER_REG_POS8    0x08u

/* RADIO.MODE field (4-bit, offset 0) - PS §6.17.10 */
#define RADIO_EVAL_MODE_REG_NRF_1MBIT       0u
#define RADIO_EVAL_MODE_REG_NRF_2MBIT       1u
#define RADIO_EVAL_MODE_REG_BLE_1MBIT       3u
#define RADIO_EVAL_MODE_REG_BLE_2MBIT       4u
#define RADIO_EVAL_MODE_REG_BLE_LR125KBIT   5u
#define RADIO_EVAL_MODE_REG_BLE_LR500KBIT   6u
#define RADIO_EVAL_MODE_REG_IEEE802154_250  15u

/* RADIO.CRCCNF.LEN field (2-bit, offset 0) - PS §6.17.19 */
#define RADIO_EVAL_CRCCNF_LEN_DISABLED   0u
#define RADIO_EVAL_CRCCNF_LEN_ONE        1u
#define RADIO_EVAL_CRCCNF_LEN_TWO        2u
#define RADIO_EVAL_CRCCNF_LEN_THREE      3u

/* RADIO.CRCCNF.SKIPADDR field (2-bit, offset 8) - PS §6.17.19 */
#define RADIO_EVAL_CRCCNF_SKIPADDR_INCLUDE       0u
#define RADIO_EVAL_CRCCNF_SKIPADDR_SKIP          1u
#define RADIO_EVAL_CRCCNF_SKIPADDR_IEEE802154    2u

/* RADIO.PCNF1.WHITEEN (1-bit, offset 25) - PS §6.17.16 */
#define RADIO_EVAL_PCNF1_WHITEEN_DISABLED   0u
#define RADIO_EVAL_PCNF1_WHITEEN_ENABLED    1u

/* RADIO.PCNF1.ENDIAN (1-bit, offset 24) - PS §6.17.16 */
#define RADIO_EVAL_PCNF1_ENDIAN_LITTLE   0u
#define RADIO_EVAL_PCNF1_ENDIAN_BIG      1u

/* ---- Frequency / channel math constants ------------------------------ *
 * The nRF RADIO FREQUENCY register is an offset from 2400 MHz in 1 MHz
 * steps (PS §6.17.14). BLE channels 0..39 map to 2402..2480 MHz in
 * 2 MHz increments; the advertising channel indices (37,38,39) are
 * remapped per BLE Core §6.B.5.22 but the radio-frequency conversion
 * itself is monotonic. */
#define RADIO_EVAL_FREQ_REG_MIN        0     /* 2400 MHz */
#define RADIO_EVAL_FREQ_REG_MAX        100   /* 2500 MHz */
#define RADIO_EVAL_FREQ_REG_BASE_MHZ   2400

#define RADIO_EVAL_BLE_CHANNEL_MIN     0
#define RADIO_EVAL_BLE_CHANNEL_MAX     39
#define RADIO_EVAL_BLE_FREQ_MHZ_MIN    2402  /* channel 0 */
#define RADIO_EVAL_BLE_FREQ_MHZ_MAX    2480  /* channel 39 */

/* ---- Mirrored jamming pattern position sentinel (radio_defs.h) ------- */
#define RADIO_EVAL_JAM_POSITION_ANYWHERE   0xFFu

/* ---- Mirrored CRC size range (radio.cpp:1036-1053) ------------------- */
#define RADIO_EVAL_CRC_SIZE_MIN        0
#define RADIO_EVAL_CRC_SIZE_MAX        3

/* ---- Air-time constants (radio.cpp:1433) ----------------------------- *
 * Per-byte air time: 8 us at 1 Mbit/s, 4 us at 2 Mbit/s.
 * Fixed 100 us offset accounts for radio ramp-down / END-event latency
 * (matches radio.cpp:1433 ISR back-calculation). */
#define RADIO_EVAL_AIR_TIME_US_PER_BYTE_1MBIT   8u
#define RADIO_EVAL_AIR_TIME_US_PER_BYTE_2MBIT   4u
#define RADIO_EVAL_END_EVENT_OFFSET_US          100u

/* ---- PHY / TX power / frequency validation --------------------------- */

/* True iff phy is one of the 6 supported logical PHYs (radio.cpp:678-698). */
bool radio_eval_phy_supported(radio_eval_phy_t phy);

/* True iff the nRF52840 RADIO.MODE register value is defined for the
 * given PHY. All 6 supported PHYs have a register mapping; the default
 * branch in radio.cpp:695-697 corresponds to "unsupported". */
bool radio_eval_phy_has_mode_register(radio_eval_phy_t phy);

/* Lookup the nRF52840 RADIO.MODE register value for a PHY.
 * Returns true and writes *reg_out on success; false on unsupported PHY.
 * DOT15D4_WAZABE shares BLE_2MBIT's register value per radio.cpp:688-690. */
bool radio_eval_phy_to_mode_register(radio_eval_phy_t phy, uint32_t *reg_out);

/* True iff the PHY uses a 2 Mbit/s on-air rate (BLE_2MBITS, ESB_2MBITS,
 * or DOT15D4_WAZABEE which runs the BLE 2Mbit radio mode). */
bool radio_eval_phy_is_2mbit(radio_eval_phy_t phy);

/* True iff dBm is one of the 10 power levels supported by radio.cpp:286-323
 * (-40, -30, -20, -16, -12, -8, -4, 0, +4, +8). */
bool radio_eval_dbm_supported(int dbm);

/* Map dBm to the TxPower enum. Returns RADIO_EVAL_TXPOWER_INVALID for
 * unsupported values. (Mirrors Radio::setTxPower(int) at radio.cpp:286-323.) */
radio_eval_txpower_t radio_eval_dbm_to_txpower(int dbm);

/* Map TxPower back to integer dBm. Returns 127 for the invalid sentinel. */
int radio_eval_txpower_to_dbm(radio_eval_txpower_t tp);

/* Lookup the nRF52840 RADIO.TXPOWER register value for a TxPower enum.
 * Returns true and writes *reg_out on success; false on invalid enum.
 * The mapping matches nRF52840 PS §6.17.13 (reproduced from the SDK's
 * nrf52840_bitfields.h RADIO_TXPOWER_TXPOWER_* constants). */
bool radio_eval_txpower_to_register(radio_eval_txpower_t tp, uint32_t *reg_out);

/* Validate nRF52840 FREQUENCY register value (0..100). (radio.cpp:668-675.) */
bool radio_eval_frequency_register_valid(int freq_reg);

/* BLE channel number (0..39) <-> frequency in MHz (2402..2480). */
bool radio_eval_ble_channel_valid(int channel);
int  radio_eval_ble_channel_to_mhz(int channel);     /* -1 on invalid */
int  radio_eval_ble_mhz_to_channel(int freq_mhz);    /* -1 on invalid */

/* BLE channel <-> nRF52840 FREQUENCY register value (= MHz - 2400). */
int  radio_eval_ble_channel_to_freq_reg(int channel); /* -1 on invalid */

/* ---- CRC field mapping (radio.cpp:1036-1060) ------------------------- */

/* Validate crcSize (0..3). */
bool radio_eval_crc_size_valid(uint8_t crc_size);

/* Map crcSize (0/1/2/3) to RADIO.CRCCNF.LEN register value.
 * Returns false for crcSize > 3. */
bool radio_eval_crc_size_to_len(uint8_t crc_size, uint32_t *len_out);

/* Map the crcSkipAddress boolean and PHY to RADIO.CRCCNF.SKIPADDR.
 * DOT15D4_NATIVE always uses IEEE802154 mode; other PHYs honor the
 * boolean (true=Skip, false=Include). (radio.cpp:1054-1060.) */
uint32_t radio_eval_crc_skipaddr_field(bool skip_address, radio_eval_phy_t phy);

/* ---- PCNF1 field mapping (radio.cpp:1019-1023) ----------------------- */

uint32_t radio_eval_pcnf1_whiteen_field(bool hardware_whitening);
uint32_t radio_eval_pcnf1_endian_field(bool little_endian);

/* ---- Jamming pattern matching (radio.cpp:182-213) -------------------- *
 * The firmware uses JammingPattern (radio_defs.h) with pointer fields
 * for pattern/mask. The eval layer uses a simpler view that drops the
 * linked-list `next` pointer (queue iteration stays in radio.cpp). */

typedef struct {
	const uint8_t *pattern;   /* pattern bytes to match against */
	const uint8_t *mask;      /* AND mask applied to both pattern and buffer */
	size_t         size;      /* number of bytes in pattern/mask */
	uint8_t        position;  /* byte offset in buffer, or RADIO_EVAL_JAM_POSITION_ANYWHERE */
} radio_eval_jam_view_t;

/* Check a buffer against a single jamming pattern.
 * - position == RADIO_EVAL_JAM_POSITION_ANYWHERE (0xFF): scans every
 *   byte offset where (offset + size) <= buffer_size; returns true on
 *   the first offset where every (buffer[offset+i] & mask[i]) == pattern[i].
 * - otherwise: tests bytes at the fixed offset; returns true iff every
 *   byte matches (no scan).
 * (Mirrors Radio::checkJammingPattern at radio.cpp:182-202.) */
bool radio_eval_jam_match(const radio_eval_jam_view_t *view,
                          const uint8_t *buffer, size_t buffer_size);

/* Byte-wise buffer equality. Returns true iff the first `size` bytes
 * of a and b are identical. (Mirrors compareBuffers from helpers.cpp,
 * copied here so the eval layer has no firmware-side dependencies.) */
bool radio_eval_buffers_equal(const uint8_t *a, const uint8_t *b, size_t size);

/* ---- RX buffer size derivation (radio.cpp:1396-1413) ----------------- *
 * Given the configured header (s0/length/s1 fields), the payload length,
 * and the first 2 bytes of the received buffer, compute the on-air
 * buffer size. DOT15D4_NATIVE is a fixed 128 bytes; otherwise:
 *   s0 present -> +1
 *   s1 present -> +1
 *   length field present (length != 0):
 *     +1 (length byte) + (s0==0 ? rx[0] : rx[1])   // length-encoded payload
 *   length field absent:
 *     + payload_length                               // fixed payload
 */

/* Header view matching Header in radio_defs.h. */
typedef struct {
	uint8_t s0;
	uint8_t length;
	uint8_t s1;
} radio_eval_header_t;

uint8_t radio_eval_compute_rx_buffer_size(radio_eval_phy_t phy,
                                          const radio_eval_header_t *hdr,
                                          uint8_t payload_length,
                                          const uint8_t *rx_first2);

/* ---- RX start-timestamp back-calculation (radio.cpp:1433) ------------ *
 * Given the radio END timestamp (us), preamble size, buffer size, and
 * the 2-Mbit flag, compute the start-of-packet timestamp by subtracting
 * the air time plus a fixed 100 us offset. (Mirrors radio.cpp:1433.)
 * `phy` is used to derive the 2-Mbit flag. */
uint32_t radio_eval_compute_rx_start_us(uint32_t end_us,
                                        uint8_t preamble_size,
                                        uint8_t buffer_size,
                                        radio_eval_phy_t phy);

#ifdef __cplusplus
}
#endif

#endif /* RADIO_EVAL_H */
