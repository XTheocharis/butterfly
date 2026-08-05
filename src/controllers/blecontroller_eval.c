/*
 * blecontroller_eval.c - Pure-logic BLE controller evaluation.
 *
 * No SDK deps. Compiled on BOTH host (for unit tests) and device
 * (linked into firmware). See blecontroller_eval.h for design notes.
 *
 * References to BLEController::<method> indicate the firmware C++ method
 * that this function extracts the pure-logic subset from.
 *
 * allow: SIZE_OK — Cohesive extraction of a single source file
 *        (blecontroller.cpp, 3152L). Splitting would scatter one
 *        extractee's logic across multiple .c files, breaking the
 *        "one eval per C++ source" invariant. Same pattern as
 *        pdm_eval.c (501L) and motion_eval.c.
 */
#include "blecontroller_eval.h"

#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Channel map operations ------------------------------------------ */

blec_result_t blec_decode_channel_map(const uint8_t channel_map[5],
                                      bool used[37],
                                      uint32_t *out_count)
{
    if (channel_map == NULL || used == NULL) {
        return BLEC_ERR_NULL_ARG;
    }

    uint32_t count = 0;
    for (uint32_t i = 0; i < 5; i++) {
        for (uint32_t j = 0; j < 8; j++) {
            uint32_t ch = 8u * i + j;
            if (ch < 37u) {
                bool is_used = (channel_map[i] & (uint8_t)(1u << j)) != 0u;
                used[ch] = is_used;
                if (is_used) {
                    count++;
                }
            }
        }
    }

    if (out_count != NULL) {
        *out_count = count;
    }
    return BLEC_OK;
}

blec_result_t blec_build_remapping_table(const bool used[37],
                                         int32_t remap[37])
{
    if (used == NULL || remap == NULL) {
        return BLEC_ERR_NULL_ARG;
    }
    /* remap[i] = the i-th used channel number. Unused slots are left
     * as their previous value; callers must not read past used_count. */
    uint32_t j = 0;
    for (uint32_t i = 0; i < 37u; i++) {
        if (used[i]) {
            remap[j] = (int32_t)i;
            j++;
        }
    }
    return BLEC_OK;
}

blec_result_t blec_decode_monitored_channels(const uint8_t channels[5],
                                             bool monitored[37],
                                             uint32_t *out_count)
{
    if (channels == NULL || monitored == NULL) {
        return BLEC_ERR_NULL_ARG;
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < 5; i++) {
        for (uint32_t j = 0; j < 8; j++) {
            uint32_t ch = 8u * i + j;
            if (ch < 37u) {
                bool is_monitored = (channels[i] & (uint8_t)(1u << j)) != 0u;
                monitored[ch] = is_monitored;
                if (is_monitored) {
                    count++;
                }
            }
        }
    }
    if (out_count != NULL) {
        *out_count = count;
    }
    return BLEC_OK;
}

/* ---- CSA2 primitives (Vol 6, Part B, 4.5.8) -------------------------- */

uint16_t blec_csa2_mam(uint16_t a, uint16_t b)
{
    /* mam(a, b) = (17 * a + b) mod 0x10000 — matches blecontroller.cpp:26. */
    return (uint16_t)((((uint32_t)17u * (uint32_t)a) + (uint32_t)b) % 0x10000u);
}

uint16_t blec_csa2_permute(uint16_t v)
{
    /* Bit-permute: swap odd/even bits, then nibble pairs, then byte halves.
     * Mirrors blecontroller.cpp:31. */
    v = (uint16_t)(((v & 0xaaaau) >> 1) | ((v & 0x5555u) << 1));
    v = (uint16_t)(((v & 0xccccu) >> 2) | ((v & 0x3333u) << 2));
    return (uint16_t)(((v & 0xf0f0u) >> 4) | ((v & 0x0f0fu) << 4));
}

uint16_t blec_csa2_prne(uint16_t counter, uint16_t chan_id)
{
    /* Three permute+mam rounds, XOR-folded with chan_id each round.
     * Mirrors blecontroller.cpp:38. */
    uint16_t prne = (uint16_t)(counter ^ chan_id);
    prne = blec_csa2_mam(blec_csa2_permute(prne), chan_id);
    prne = blec_csa2_mam(blec_csa2_permute(prne), chan_id);
    prne = blec_csa2_mam(blec_csa2_permute(prne), chan_id);
    return (uint16_t)(prne ^ chan_id);
}

uint16_t blec_csa2_unmapped_channel(uint16_t counter, uint16_t chan_id)
{
    return (uint16_t)(blec_csa2_prne(counter, chan_id) % 37u);
}

/* ---- Channel → frequency offset -------------------------------------- */

int32_t blec_channel_to_frequency(int32_t channel)
{
    /* Spec: advertising channels 37/38/39 → offsets 2/26/80;
     *       data channels 0..10 → even offsets 4..24;
     *       data channels 11..36 → even offsets 28..78.
     * Mirrors BLEController::channelToFrequency (blecontroller.cpp:57). */
    if (channel == 37) return 2;
    if (channel == 38) return 26;
    if (channel == 39) return 80;
    if (channel < 0 || channel > 36) {
        return 0;  /* invalid; caller should range-check first */
    }
    if (channel < 11) {
        return 2 * (channel + 2);
    }
    return 2 * (channel + 3);
}

/* ---- Legacy (CSA1) hopping sequence ---------------------------------- */

blec_result_t blec_generate_legacy_hop_sequence(const bool used[37],
                                                const int32_t remap[37],
                                                uint32_t used_count,
                                                uint8_t hop_increment,
                                                uint8_t sequence[37])
{
    if (used == NULL || remap == NULL || sequence == NULL) {
        return BLEC_ERR_NULL_ARG;
    }
    if (used_count == 0u) {
        return BLEC_ERR_NO_CHANNELS;
    }

    /* Mirrors BLEController::generateLegacyHoppingSequence (line 375).
     * Walks channel = (channel + hop_increment) mod 37; emits the channel
     * itself if it's in use, else remap[channel mod used_count]. */
    uint8_t channel = 0;
    for (uint32_t i = 0; i < 37u; i++) {
        if (used[channel]) {
            sequence[i] = channel;
        } else {
            uint32_t idx = (uint32_t)channel % used_count;
            sequence[i] = (uint8_t)remap[idx];
        }
        channel = (uint8_t)(((uint32_t)channel + (uint32_t)hop_increment) % 37u);
    }
    return BLEC_OK;
}

int32_t blec_find_channel_in_sequence(const uint8_t hopping_sequences[12][37],
                                      uint8_t hop_increment,
                                      uint8_t channel,
                                      uint8_t start)
{
    if (hopping_sequences == NULL || hop_increment >= 12u) {
        return -1;
    }
    /* Linear search from `start` (mod 37) for the first slot holding
     * `channel`. Mirrors BLEController::findChannelIndexInHoppingSequence. */
    for (uint32_t i = 0; i < 37u; i++) {
        uint32_t idx = ((uint32_t)start + i) % 37u;
        if (hopping_sequences[hop_increment][idx] == channel) {
            return (int32_t)idx;
        }
    }
    return -1;
}

int32_t blec_distance_between_channels(const uint8_t hopping_sequences[12][37],
                                       uint8_t hop_increment,
                                       uint8_t first_channel,
                                       uint8_t second_channel)
{
    /* Forward modular distance first→second inside hop_increment's sequence.
     * Mirrors BLEController::computeDistanceBetweenChannels (line 403). */
    int32_t first_idx  = blec_find_channel_in_sequence(hopping_sequences,
                                                       hop_increment,
                                                       first_channel, 0);
    if (first_idx < 0) {
        return -1;
    }
    int32_t second_idx = blec_find_channel_in_sequence(hopping_sequences,
                                                       hop_increment,
                                                       second_channel,
                                                       (uint8_t)first_idx);
    if (second_idx < 0) {
        return -1;
    }
    if (second_idx > first_idx) {
        return second_idx - first_idx;
    }
    return second_idx - first_idx + 37;
}

/* ---- Connection update FSM ------------------------------------------- */

void blec_connection_update_clear(blec_connection_update_t *st)
{
    if (st == NULL) {
        return;
    }
    /* memset is the simplest way to clear a flat POD; matches the explicit
     * field-zero pattern at blecontroller.cpp:501-512. */
    memset(st, 0, sizeof(*st));
    st->type = BLEC_UPDATE_NONE;
}

void blec_connection_update_prepare_interval(blec_connection_update_t *st,
                                             uint16_t instant,
                                             uint16_t hop_interval,
                                             uint8_t window_size,
                                             uint8_t window_offset)
{
    if (st == NULL) {
        return;
    }
    st->type         = BLEC_UPDATE_CONNECTION_UPDATE_REQ;
    st->instant      = instant;
    st->hop_interval = hop_interval;
    st->window_size  = window_size;
    st->window_offset = window_offset;
    /* Per firmware: interval-update carry no channel-map; zero it. */
    memset(st->channel_map, 0, sizeof(st->channel_map));
}

void blec_connection_update_prepare_channel_map(blec_connection_update_t *st,
                                                uint16_t instant,
                                                const uint8_t channel_map[5])
{
    if (st == NULL || channel_map == NULL) {
        return;
    }
    st->type     = BLEC_UPDATE_CHANNEL_MAP_REQ;
    st->instant  = instant;
    st->hop_interval  = 0;
    st->window_size   = 0;
    st->window_offset = 0;
    memcpy(st->channel_map, channel_map, 5);
}

bool blec_connection_update_maybe_fire(blec_connection_update_t *st,
                                       uint16_t connection_event_count,
                                       blec_update_type_t *out_type)
{
    if (st == NULL) {
        return false;
    }
    /* Fire iff a non-NONE update is staged AND the event count hit the
     * instant. Mirrors BLEController::applyConnectionUpdate (line 609). */
    if (st->type == BLEC_UPDATE_NONE) {
        return false;
    }
    if (connection_event_count != st->instant) {
        return false;
    }
    if (out_type != NULL) {
        *out_type = st->type;
    }
    /* Idempotent: clear after firing so subsequent events with the same
     * count do not re-trigger (matches firmware clear after apply). */
    blec_connection_update_clear(st);
    return true;
}

/* ---- PHY update FSM -------------------------------------------------- */

void blec_phy_update_clear(blec_phy_update_t *st)
{
    if (st == NULL) {
        return;
    }
    memset(st, 0, sizeof(*st));
    st->type = BLEC_PHY_UPDATE_NONE;
}

void blec_phy_update_prepare(blec_phy_update_t *st,
                             uint16_t instant,
                             uint8_t c2p,
                             uint8_t p2c)
{
    if (st == NULL) {
        return;
    }
    st->type    = BLEC_PHY_UPDATE_BOTH;
    st->instant = instant;
    st->c2p     = c2p;
    st->p2c     = p2c;
}

bool blec_phy_update_maybe_fire(blec_phy_update_t *st,
                                uint16_t connection_event_count)
{
    if (st == NULL) {
        return false;
    }
    /* Only the timing/fire decision; the radio PHY change stays in C++.
     * Mirrors the guard at BLEController::applyPhyUpdate (line 545). */
    if (st->type == BLEC_PHY_UPDATE_NONE) {
        return false;
    }
    if (connection_event_count != st->instant) {
        return false;
    }
    blec_phy_update_clear(st);
    return true;
}

/* ---- Channel selector (CSA1 + CSA2 next-channel) --------------------- */

void blec_channel_selector_init(blec_channel_selector_t *s, blec_csa_t alg)
{
    if (s == NULL) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->algorithm = alg;
}

blec_result_t blec_channel_selector_set_map(blec_channel_selector_t *s,
                                            const uint8_t channel_map[5])
{
    if (s == NULL || channel_map == NULL) {
        return BLEC_ERR_NULL_ARG;
    }
    uint32_t count = 0;
    blec_result_t r = blec_decode_channel_map(channel_map, s->used, &count);
    if (r != BLEC_OK) {
        return r;
    }
    s->used_count = count;
    if (count > 0u) {
        blec_build_remapping_table(s->used, s->remap);
    }
    return BLEC_OK;
}

int32_t blec_next_channel(blec_channel_selector_t *s,
                          uint16_t connection_event_count)
{
    if (s == NULL || s->used_count == 0u) {
        return -1;
    }

    switch (s->algorithm) {
    case BLEC_CSA2: {
        /* Compute unmapped channel via PRNG, then remap if not used.
         * Mirrors BLEController::nextChannel case CSA2 (line 466). */
        uint16_t unmapped = blec_csa2_unmapped_channel(connection_event_count,
                                                       s->csa2_chan_id);
        s->last_unmapped = (int32_t)unmapped;
        if (s->used[unmapped]) {
            return (int32_t)unmapped;
        }
        uint32_t remap_idx = ((uint32_t)s->used_count
                              * (uint32_t)blec_csa2_prne(connection_event_count,
                                                         s->csa2_chan_id))
                             / 0x10000u;
        if (remap_idx >= s->used_count) {
            remap_idx = s->used_count - 1u;  /* defensive; cannot happen */
        }
        return s->remap[remap_idx];
    }
    case BLEC_CSA1:
    default: {
        /* Legacy hop: unmapped = (last + hop_increment) mod 37; remap if
         * unused. Mirrors BLEController::nextChannel default case (line 483). */
        int32_t unmapped = (s->last_unmapped + (int32_t)s->hop_increment) % 37;
        if (unmapped < 0) {
            unmapped += 37;
        }
        s->last_unmapped = unmapped;
        if (s->used[unmapped]) {
            return unmapped;
        }
        int32_t remap_idx = unmapped % (int32_t)s->used_count;
        return s->remap[remap_idx];
    }
    }
}

/* ---- Access address candidate tracker -------------------------------- */

void blec_aa_candidates_reset(blec_aa_candidates_t *st)
{
    if (st == NULL) {
        return;
    }
    /* Mirrors BLEController::resetAccessAddressesCandidates (line 1033). */
    memset(st, 0, sizeof(*st));
}

bool blec_aa_candidates_is_known(blec_aa_candidates_t *st,
                                 uint32_t access_address)
{
    if (st == NULL) {
        return false;
    }
    /* Find AA, bump seen-count, return true iff seen > 1.
     * Mirrors BLEController::isAccessAddressKnown (line 1045). */
    for (uint32_t i = 0; i < BLE_MAX_AA_CANDIDATES; i++) {
        if (st->aa[i] == access_address) {
            st->seen[i]++;
            return st->seen[i] > 1u;
        }
    }
    return false;
}

blec_result_t blec_aa_candidates_add(blec_aa_candidates_t *st,
                                     uint32_t access_address)
{
    if (st == NULL) {
        return BLEC_ERR_NULL_ARG;
    }
    /* If we still have room, append with seen=1.
     * Mirrors BLEController::addCandidateAccessAddress (line 1068).
     * Cast preserves the firmware's int32_t count type (matches
     * blecontroller.h:130 CandidateAccessAddresses::count) while
     * suppressing sign-compare against the unsigned capacity. */
    if ((uint32_t)st->count < (uint32_t)BLE_MAX_AA_CANDIDATES) {
        st->seen[st->count]  = 1u;
        st->aa[st->count]    = access_address;
        st->count++;
        return BLEC_OK;
    }

    /* Table full: bubble-sort descending by seen-count, halve, append.
     * The firmware uses a do/while with `change` flag; we mirror it
     * exactly so behavior under fuzzing matches production. */
    bool changed;
    do {
        changed = false;
        for (int32_t i = 0; i < (st->count - 1); i++) {
            for (int32_t j = i + 1; j < st->count; j++) {
                if (st->seen[i] < st->seen[j]) {
                    uint32_t tmp_seen = st->seen[i];
                    st->seen[i] = st->seen[j];
                    st->seen[j] = tmp_seen;
                    uint32_t tmp_aa = st->aa[i];
                    st->aa[i] = st->aa[j];
                    st->aa[j] = tmp_aa;
                    changed = true;
                }
            }
        }
    } while (changed);

    st->count /= 2;
    if ((uint32_t)st->count >= (uint32_t)BLE_MAX_AA_CANDIDATES) {
        return BLEC_ERR_FULL;
    }
    st->aa[st->count]   = access_address;
    st->seen[st->count] = 1u;
    st->count++;
    return BLEC_OK;
}

/* ---- Payload bounds clamp -------------------------------------------- */

uint32_t blec_clamp_payload_size(uint32_t requested_size)
{
    /* Mirrors the four clamp sites at blecontroller.cpp:623/633/646/2058. */
    return (requested_size > BLE_PAYLOAD_MAX_BYTES)
               ? BLE_PAYLOAD_MAX_BYTES
               : requested_size;
}

/* ---- CCM counter packing --------------------------------------------- */

void blec_pack_ccm_counter(uint8_t counter_out[5], uint32_t counter)
{
    if (counter_out == NULL) {
        return;
    }
    /* 32-bit LE into bytes 0..3; byte 4 always 0 (event counts < 2^32).
     * Mirrors blecontroller.cpp:set_ccm_counter (line 9). */
    counter_out[0] = (uint8_t)(counter);
    counter_out[1] = (uint8_t)(counter >> 8);
    counter_out[2] = (uint8_t)(counter >> 16);
    counter_out[3] = (uint8_t)(counter >> 24);
    counter_out[4] = 0u;
}

#ifdef __cplusplus
}
#endif
