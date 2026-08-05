/*
 * blecontroller_eval.h - Pure-C BLE controller evaluation layer
 * (host-testable, no SDK deps).
 *
 * Extracts the pure-logic subset of BLEController (radio state machine,
 * channel-map math, CSA1/CSA2 channel selection, connection-update FSM,
 * PHY-update FSM, sequence-number tracking, access-address candidate
 * tracker, payload bounds clamp, and CCM counter packing) so it can be
 * unit-tested on host without the nRF52 radio/Core/Timer SDK deps that
 * blecontroller.{cpp,h} pulls in via controller.h / packet.h / timer.h.
 *
 * The firmware C++ class BLEController keeps its radio/Core/Timer wiring
 * and continues to own SDK-dependent state; these functions operate on
 * caller-allocated arrays and blec_* state structs that mirror the C++
 * member layout. Migration of BLEController to call these functions is
 * incremental (T37 adds host tests first; the C++ wrapper refactor that
 * wires them in is a follow-up).
 *
 * Compiled on BOTH host (for unit tests) and device (linked into firmware).
 *
 * Spec reference: Bluetooth Core Spec Vol 6, Part B (LL):
 *   - Section 4.5.8 Channel Selection (CSA1 + CSA2)
 *   - Section 4.5.7 Connection Event (instant-based update apply)
 *   - Section 5.1.2 Access Address (candidate tracking)
 *
 * allow: SIZE_OK — Header mirrors the public surface of a single
 *        extractee (blecontroller.cpp). All declarations are
 *        type/struct/prototype; no logic. Splitting would break
 *        the "one eval header per C++ source" invariant.
 */
#ifndef BLECONTROLLER_EVAL_H
#define BLECONTROLLER_EVAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Frozen BLE physical constants (mirror blecontroller.h) ----------- */

#define BLE_NUM_DATA_CHANNELS      37u
#define BLE_NUM_ADV_CHANNELS       3u   /* 37, 38, 39 */
#define BLE_CHANNEL_MAP_BYTES      5u   /* 37 bits → 5 bytes */
#define BLE_MAX_AA_CANDIDATES      25u  /* blecontroller.h: MAX_AA_CANDIDATES */
#define BLE_ADV_REPORT_SIZE        25u  /* blecontroller.h: ADV_REPORT_SIZE */
#define BLE_IFS_US                 150u /* Inter-Frame Space, microseconds */
#define BLE_1MBPS_PREAMBLE_BYTES   1u
#define BLE_ACCESS_ADDRESS_BYTES   4u
#define BLE_CRC_BYTES              3u

/* BLE payload cap shared by attack/slave/master slots in blecontroller.cpp. */
#define BLE_PAYLOAD_MAX_BYTES      64u

/* ---- Result codes ---------------------------------------------------- */

typedef enum {
    BLEC_OK               = 0,
    BLEC_ERR_NULL_ARG     = 1,
    BLEC_ERR_INVALID_CH   = 2,  /* channel out of [0, 39] */
    BLEC_ERR_NO_CHANNELS  = 3,  /* channel map has zero used channels */
    BLEC_ERR_FULL         = 4,  /* candidate tracker full (post-eviction) */
} blec_result_t;

/* ---- Channel selection algorithm ------------------------------------- */

typedef enum {
    BLEC_CSA1 = 0,  /* Legacy hop increment mod 37 + remapping */
    BLEC_CSA2 = 1,  /* PRNG-based (Vol 6, Part B, 4.5.8) */
} blec_csa_t;

/* ---- Connection / advertising state machine --------------------------- */
/*
 * Mirrors BLEControllerState in blecontroller.h. Kept in sync by value;
 * the firmware class converts to/from via static cast at the SDK seam.
 */
typedef enum {
    BLEC_STATE_IDLE                       = 0,
    BLEC_STATE_SNIFFING_ADVERTISEMENTS    = 1,
    BLEC_STATE_COLLECTING_ADVINTERVAL     = 2,
    BLEC_STATE_FOLLOWING_ADVERTISEMENTS   = 3,
    BLEC_STATE_SNIFFING_ACCESS_ADDRESS    = 4,
    BLEC_STATE_RECOVERING_CRC_INIT        = 5,
    BLEC_STATE_RECOVERING_CHANNEL_MAP     = 6,
    BLEC_STATE_RECOVERING_HOP_INTERVAL    = 7,
    BLEC_STATE_RECOVERING_HOP_INCREMENT   = 8,
    BLEC_STATE_ATTACH_TO_EXISTING_CONN    = 9,
    BLEC_STATE_SNIFFING_CONNECTION        = 10,
    BLEC_STATE_INJECTING_TO_SLAVE         = 11,
    BLEC_STATE_INJECTING_TO_MASTER        = 12,
    BLEC_STATE_SIMULATING_SLAVE           = 13,
    BLEC_STATE_SYNCHRONIZING_MASTER       = 14,
    BLEC_STATE_SIMULATING_MASTER          = 15,
    BLEC_STATE_SYNCHRONIZING_MITM         = 16,
    BLEC_STATE_PERFORMING_MITM            = 17,
    BLEC_STATE_JAMMING_CONNECT_REQ        = 18,
    BLEC_STATE_REACTIVE_JAMMING           = 19,
    BLEC_STATE_CONNECTION_INITIATION      = 20,
    BLEC_STATE_CONNECTION_INITIATION_SLAVE = 21,
    BLEC_STATE_SCANNING                   = 22,
    BLEC_STATE_ADVERTISING                = 23,
} blec_state_t;

/* ---- Channel map operations ------------------------------------------ */

/*
 * Decode a 37-bit BLE channel map (5 bytes, low byte first, MSB of byte 4
 * unused — only bits for channels 0..36 are significant) into a per-channel
 * boolean `used[37]` array. Returns the number of used channels via
 * *out_count. Matches BLEController::updateChannelsInUse bit-iteration.
 *
 * Pass the same `used[37]` array to blec_build_remapping_table() to derive
 * the remap table used by CSA1 and CSA2 channel remapping.
 */
blec_result_t blec_decode_channel_map(const uint8_t channel_map[5],
                                      bool used[37],
                                      uint32_t *out_count);

/* Build the remapping table: remap[i] = the i-th used channel number.
 * `used[37]` comes from blec_decode_channel_map. `count` is its used-count.
 * Caller provides remap[37] storage. */
blec_result_t blec_build_remapping_table(const bool used[37],
                                         int32_t remap[37]);

/* Decode monitored-channel bitmap (same bit layout as channel_map, but
 * surfaces a per-channel `monitored[37]` flag and a count). Mirrors
 * BLEController::setMonitoredChannels. */
blec_result_t blec_decode_monitored_channels(const uint8_t channels[5],
                                             bool monitored[37],
                                             uint32_t *out_count);

/* ---- Channel Selection Algorithm #2 primitives ------------------------ */
/*
 * Vol 6, Part B, Section 4.5.8 — the same primitives the firmware uses
 * (mam / permute / prne). Exposed for round-trip testing against known
 * vectors in the spec.
 */
uint16_t blec_csa2_mam(uint16_t a, uint16_t b);
uint16_t blec_csa2_permute(uint16_t v);
uint16_t blec_csa2_prne(uint16_t counter, uint16_t chan_id);

/* unmapped = prne(counter, chan_id) mod 37. */
uint16_t blec_csa2_unmapped_channel(uint16_t counter, uint16_t chan_id);

/* ---- Channel → RF frequency offset (MHz from 2402) -------------------- */
/*
 * Mirrors BLEController::channelToFrequency. Returns 0 for channel 0,
 * 2 for channel 37, 26 for channel 38, 80 for channel 39. Channels 0..10
 * map to even offsets 4..24; channels 11..36 map to even offsets 28..78.
 */
int32_t blec_channel_to_frequency(int32_t channel);

/* ---- Hopping sequence analysis (CSA1 discovery aid) ------------------- */

/*
 * Generate a single CSA1 hopping sequence for the given hop_increment
 * (range 5..16 per spec; firmware uses 5..16 over 12 sequences).
 * Mirrors BLEController::generateLegacyHoppingSequence. Caller owns
 * sequence[37] storage.
 */
blec_result_t blec_generate_legacy_hop_sequence(const bool used[37],
                                                const int32_t remap[37],
                                                uint32_t used_count,
                                                uint8_t hop_increment,
                                                uint8_t sequence[37]);

/* Find the first index `i >= start` (mod 37) where
 * hopping_sequences[hop_increment][i] == channel. Returns -1 if absent.
 * hop_increment range is [0, 11]. */
int32_t blec_find_channel_in_sequence(const uint8_t hopping_sequences[12][37],
                                      uint8_t hop_increment,
                                      uint8_t channel,
                                      uint8_t start);

/*
 * Forward distance between two channels inside a single hopping sequence.
 * Mirrors BLEController::computeDistanceBetweenChannels. Returns the
 * positive modular distance (1..37) from firstChannel to secondChannel
 * starting at firstChannel's index, or -1 if either is absent.
 */
int32_t blec_distance_between_channels(const uint8_t hopping_sequences[12][37],
                                       uint8_t hop_increment,
                                       uint8_t first_channel,
                                       uint8_t second_channel);

/* ---- Connection update FSM ------------------------------------------- */

typedef enum {
    BLEC_UPDATE_NONE                  = 0,
    BLEC_UPDATE_CONNECTION_UPDATE_REQ = 1,  /* hop interval / window / latency */
    BLEC_UPDATE_CHANNEL_MAP_REQ       = 2,  /* only channelMap changes */
} blec_update_type_t;

typedef struct {
    blec_update_type_t type;
    uint16_t instant;          /* connectionEventCount that triggers apply */
    uint16_t hop_interval;     /* only for CONNECTION_UPDATE_REQ */
    uint8_t  window_size;
    uint8_t  window_offset;
    uint8_t  channel_map[5];   /* only for CHANNEL_MAP_REQ */
} blec_connection_update_t;

void blec_connection_update_clear(blec_connection_update_t *st);

/* Stage a connection-update request (interval/window/offset/latency).
 * The channel-map side is forced to all-zero (per firmware behavior:
 * a connection-update carries no channel-map change). */
void blec_connection_update_prepare_interval(blec_connection_update_t *st,
                                             uint16_t instant,
                                             uint16_t hop_interval,
                                             uint8_t window_size,
                                             uint8_t window_offset);

/* Stage a channel-map-only update. hop_interval/window fields zeroed. */
void blec_connection_update_prepare_channel_map(blec_connection_update_t *st,
                                                uint16_t instant,
                                                const uint8_t channel_map[5]);

/*
 * Evaluate whether the staged update should fire at this connection event
 * count. Returns true and populates *out_type if fired (caller then applies
 * hop_interval / channel_map to its own state). Idempotent: clears the
 * staged update on fire so subsequent calls return false.
 *
 * This captures ONLY the timing/fire decision (Vol 6, Part B, 4.5.7).
 * Side effects on the radio (re-anchor, CRC refresh) stay in the C++ layer.
 */
bool blec_connection_update_maybe_fire(blec_connection_update_t *st,
                                       uint16_t connection_event_count,
                                       blec_update_type_t *out_type);

/* ---- PHY update FSM --------------------------------------------------- */

typedef enum {
    BLEC_PHY_UPDATE_NONE = 0,
    BLEC_PHY_UPDATE_BOTH = 1,  /* symmetric c2p/p2c update at instant */
} blec_phy_update_type_t;

typedef struct {
    blec_phy_update_type_t type;
    uint16_t instant;
    uint8_t  c2p;   /* central-to-peripheral PHY (1=1M, 2=2M, 3=coded) */
    uint8_t  p2c;   /* peripheral-to-central PHY */
} blec_phy_update_t;

void blec_phy_update_clear(blec_phy_update_t *st);
void blec_phy_update_prepare(blec_phy_update_t *st,
                             uint16_t instant,
                             uint8_t c2p,
                             uint8_t p2c);

/* Timing-only fire check. Returns true when type != NONE and
 * connection_event_count == instant; caller applies the new PHY via radio. */
bool blec_phy_update_maybe_fire(blec_phy_update_t *st,
                                uint16_t connection_event_count);

/* ---- Channel selection: CSA1 and CSA2 next-channel ------------------- */

typedef struct {
    blec_csa_t algorithm;
    uint16_t   csa2_chan_id;      /* composed from Access Address */
    uint8_t    hop_increment;     /* CSA1 only, range 5..16 */
    int32_t    last_unmapped;     /* CSA1: previous unmappedChannel */
    /* Channel map snapshot (kept by value; recalc via decode when map changes) */
    bool       used[37];
    int32_t    remap[37];
    uint32_t   used_count;
} blec_channel_selector_t;

void blec_channel_selector_init(blec_channel_selector_t *s, blec_csa_t alg);

/*
 * Install a fresh channel map. Re-derives used[] and remap[] in-place.
 * Equivalent to BLEController::updateChannelsInUse followed by keeping
 * the remap pointer warm.
 */
blec_result_t blec_channel_selector_set_map(blec_channel_selector_t *s,
                                            const uint8_t channel_map[5]);

/*
 * Compute the next data-channel for the given connection event count.
 * Mutates last_unmapped (CSA1). Returns the physical channel number
 * (0..36) after remapping, or -1 if used_count == 0.
 *
 * CSA1: unmapped = (last_unmapped + hop_increment) mod 37; if used, return
 *       unmapped; else remap[unmapped mod used_count].
 * CSA2: unmapped = prne(counter, chan_id) mod 37; if used, return unmapped;
 *       else remap[(used_count * prne(counter, chan_id)) / 65536].
 */
int32_t blec_next_channel(blec_channel_selector_t *s,
                          uint16_t connection_event_count);

/* ---- Sequence numbers ------------------------------------------------- */

typedef struct {
    uint8_t sn;    /* Sequence Number */
    uint8_t nesn;  /* Next Expected Sequence Number */
} blec_seq_nums_t;

static inline void blec_seq_set(blec_seq_nums_t *st, uint8_t sn, uint8_t nesn) {
    if (st) { st->sn = sn; st->nesn = nesn; }
}

/* ---- Access address candidate tracker -------------------------------- */

typedef struct {
    uint32_t aa[BLE_MAX_AA_CANDIDATES];
    uint32_t seen[BLE_MAX_AA_CANDIDATES];
    int32_t  count;
} blec_aa_candidates_t;

void blec_aa_candidates_reset(blec_aa_candidates_t *st);

/* Lookup an Access Address and bump its seen-count if present.
 * Returns true iff the AA has been seen at least twice (i.e., is "known").
 * Mirrors BLEController::isAccessAddressKnown. */
bool blec_aa_candidates_is_known(blec_aa_candidates_t *st, uint32_t access_address);

/*
 * Add a candidate Access Address. When the table is full, sort descending
 * by seen-count, halve the count, and append. Mirrors
 * BLEController::addCandidateAccessAddress. Returns BLEC_OK on success or
 * BLEC_ERR_FULL if the post-eviction append still produced count > MAX
 * (cannot happen given the halving, but callers may defensively check).
 */
blec_result_t blec_aa_candidates_add(blec_aa_candidates_t *st,
                                     uint32_t access_address);

/* ---- Payload bounds clamp -------------------------------------------- */
/*
 * Mirrors the size-clamp pattern at blecontroller.cpp:623, 633, 646, 2058.
 * Caps the input size to BLE_PAYLOAD_MAX_BYTES (64) and returns the
 * effective size to copy. Caller performs the copy.
 */
uint32_t blec_clamp_payload_size(uint32_t requested_size);

/* ---- CCM encryption counter packing ---------------------------------- */
/*
 * Pack a 32-bit counter into the 5-byte little-endian counter slot used
 * by EncryptionData (blecontroller.h:204). The 39-bit packet counter is
 * stored little-endian in counter[0..3]; counter[4] holds the upper bits
 * and is set to 0 here (firmware never exceeds 32-bit event counts).
 * Mirrors blecontroller.cpp:set_ccm_counter (file-scope inline).
 */
void blec_pack_ccm_counter(uint8_t counter_out[5], uint32_t counter);

#ifdef __cplusplus
}
#endif

#endif /* BLECONTROLLER_EVAL_H */
