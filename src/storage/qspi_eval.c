/* allow: SIZE_OK — cohesive QSPI storage eval: CRC32 + trace + JEDEC +
 * adoption FSM + nonce FSM + superblock ops. Same indivisible-domain
 * exception as pinRegistry.cpp (276) and menu.cpp (338). */
/*
 * qspi_eval.c - Pure-logic QSPI storage evaluation functions.
 *
 * No SDK deps. Included from qspi.cpp (firmware) and test_qspi.cpp (host).
 * All decision logic lives here: the firmware wrapper only translates
 * between nrfx_qspi hardware operations and these pure functions.
 */
#include "qspi_eval.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- CRC32 (ISO-HDLC / zlib) ---- */

uint32_t qspi_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            uint32_t mask = -(crc & 1u);
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

/* ---- Command trace classification ---- */

qspi_trace_result_t qspi_eval_trace_pre_adopt(
    const qspi_cmd_trace_t *trace, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        if (trace[i].opcode != QSPI_OP_JEDEC_ID) {
            return QSPI_TRACE_FAIL;
        }
    }
    return QSPI_TRACE_PASS;
}

qspi_trace_result_t qspi_eval_trace_post_adopt(
    const qspi_cmd_trace_t *trace, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        uint8_t op = trace[i].opcode;
        if (op == QSPI_OP_FAST_READ ||
            op == QSPI_OP_PAGE_PROGRAM ||
            op == QSPI_OP_WRITE_ENABLE) {
            continue;
        }
        return QSPI_TRACE_FAIL;
    }
    return QSPI_TRACE_PASS;
}

/* ---- JEDEC ID validation ---- */

bool qspi_eval_jedec_match(const uint8_t rdid[3])
{
    return rdid[0] == QSPI_JEDEC_MANUFACTURER &&
           rdid[1] == QSPI_JEDEC_TYPE &&
           rdid[2] == QSPI_JEDEC_CAPACITY;
}

/* ---- Adoption state machine ---- */

qspi_state_t qspi_eval_adopt_transition(
    qspi_state_t current, qspi_event_t evt)
{
    switch (current) {
    case QSPI_ST_UNADOPTED:
        if (evt == QSPI_EVT_PROBE_OK) return QSPI_ST_PROBING;
        return QSPI_ST_UNADOPTED;

    case QSPI_ST_PROBING:
        if (evt == QSPI_EVT_ID_MATCH) return QSPI_ST_VALIDATED;
        return QSPI_ST_UNADOPTED;

    case QSPI_ST_VALIDATED:
        if (evt == QSPI_EVT_NONCE_ISSUED) return QSPI_ST_NONCE_ISSUED;
        if (evt == QSPI_EVT_RESET) return QSPI_ST_UNADOPTED;
        return QSPI_ST_VALIDATED;

    case QSPI_ST_NONCE_ISSUED:
        if (evt == QSPI_EVT_CONFIRMED) return QSPI_ST_CONFIRMED;
        if (evt == QSPI_EVT_CONFIRM_FAIL) return QSPI_ST_UNADOPTED;
        if (evt == QSPI_EVT_RESET) return QSPI_ST_UNADOPTED;
        return QSPI_ST_NONCE_ISSUED;

    case QSPI_ST_CONFIRMED:
        if (evt == QSPI_EVT_ERASE_SB_OK) return QSPI_ST_ERASING_SUPERBLOCK;
        if (evt == QSPI_EVT_ERASE_SB_FAIL) return QSPI_ST_UNADOPTED;
        return QSPI_ST_UNADOPTED;

    case QSPI_ST_ERASING_SUPERBLOCK:
        if (evt == QSPI_EVT_ERASE_REM_OK) return QSPI_ST_ERASING_REMAINING;
        if (evt == QSPI_EVT_ERASE_REM_FAIL) return QSPI_ST_UNADOPTED;
        return QSPI_ST_UNADOPTED;

    case QSPI_ST_ERASING_REMAINING:
        if (evt == QSPI_EVT_VERIFY_OK) return QSPI_ST_VERIFYING;
        if (evt == QSPI_EVT_VERIFY_FAIL) return QSPI_ST_UNADOPTED;
        return QSPI_ST_UNADOPTED;

    case QSPI_ST_VERIFYING:
        if (evt == QSPI_EVT_WRITE_SB_OK) return QSPI_ST_WRITING_SUPERBLOCK;
        if (evt == QSPI_EVT_WRITE_SB_FAIL) return QSPI_ST_UNADOPTED;
        return QSPI_ST_UNADOPTED;

    case QSPI_ST_WRITING_SUPERBLOCK:
        if (evt == QSPI_EVT_COMMIT_OK) return QSPI_ST_COMMITTING;
        return QSPI_ST_UNADOPTED;

    case QSPI_ST_COMMITTING:
        if (evt == QSPI_EVT_COMMIT_OK) return QSPI_ST_ADOPTED;
        return QSPI_ST_UNADOPTED;

    case QSPI_ST_ADOPTED:
        return QSPI_ST_ADOPTED;

    default:
        return QSPI_ST_UNADOPTED;
    }
}

bool qspi_eval_state_is_adopted(qspi_state_t state)
{
    return state == QSPI_ST_ADOPTED;
}

bool qspi_eval_state_is_unadopted(qspi_state_t state)
{
    return state == QSPI_ST_UNADOPTED;
}

/* ---- Nonce lifecycle ---- */

void qspi_eval_nonce_init(qspi_nonce_tracker_t *t)
{
    memset(t, 0, sizeof(*t));
    t->state = QSPI_NONCE_EMPTY;
}

void qspi_eval_nonce_issue(qspi_nonce_tracker_t *t,
                           const uint8_t bytes[QSPI_NONCE_SIZE],
                           uint64_t now_us)
{
    if (t->state == QSPI_NONCE_VALID) {
        t->state = QSPI_NONCE_INVALIDATED;
    }
    memcpy(t->bytes, bytes, QSPI_NONCE_SIZE);
    t->issued_us = now_us;
    t->consumed_us = 0;
    t->state = QSPI_NONCE_VALID;
}

qspi_nonce_result_t qspi_eval_nonce_check(qspi_nonce_tracker_t *t,
                                          const uint8_t *candidate,
                                          size_t len,
                                          uint64_t now_us)
{
    if (t->state == QSPI_NONCE_EMPTY) {
        return QSPI_NONCE_NONE_ISSUED;
    }
    if (t->state == QSPI_NONCE_CONSUMED) {
        return QSPI_NONCE_ALREADY_USED;
    }
    if (t->state == QSPI_NONCE_INVALIDATED || t->state == QSPI_NONCE_EXPIRED) {
        return QSPI_NONCE_NO_MATCH;
    }
    /* QSPI_NONCE_VALID */
    if (len != QSPI_NONCE_SIZE) {
        return QSPI_NONCE_NO_MATCH;
    }
    if (memcmp(t->bytes, candidate, QSPI_NONCE_SIZE) != 0) {
        return QSPI_NONCE_NO_MATCH;
    }
    /* Match — check TTL */
    if ((now_us - t->issued_us) > QSPI_NONCE_TTL_US) {
        t->state = QSPI_NONCE_EXPIRED;
        return QSPI_NONCE_RESULT_EXPIRED;
    }
    /* Consume */
    t->state = QSPI_NONCE_CONSUMED;
    t->consumed_us = now_us;
    return QSPI_NONCE_MATCH_OK;
}

void qspi_eval_nonce_invalidate(qspi_nonce_tracker_t *t)
{
    if (t->state == QSPI_NONCE_VALID || t->state == QSPI_NONCE_CONSUMED) {
        t->state = QSPI_NONCE_INVALIDATED;
    }
}

bool qspi_eval_nonce_is_valid(const qspi_nonce_tracker_t *t,
                              uint64_t now_us)
{
    if (t->state != QSPI_NONCE_VALID) return false;
    if ((now_us - t->issued_us) > QSPI_NONCE_TTL_US) return false;
    return true;
}

/* ---- Superblock operations ---- */

void qspi_eval_sb_init(qspi_superblock_t *sb)
{
    memset(sb, 0, sizeof(*sb));
    sb->magic[0] = QSPI_SB_MAGIC_0;
    sb->magic[1] = QSPI_SB_MAGIC_1;
    sb->magic[2] = QSPI_SB_MAGIC_2;
    sb->magic[3] = QSPI_SB_MAGIC_3;
    sb->version = QSPI_SB_VERSION;
    sb->jedec_id[0] = QSPI_JEDEC_MANUFACTURER;
    sb->jedec_id[1] = QSPI_JEDEC_TYPE;
    sb->jedec_id[2] = QSPI_JEDEC_CAPACITY;
    sb->flags = 0;
    sb->capacity_bytes = QSPI_FLASH_SIZE_BYTES;
    sb->sector_size = (uint16_t)QSPI_SECTOR_SIZE;
    sb->sector_count = QSPI_SECTOR_COUNT;
    sb->crc32 = 0;
    sb->commit_marker = QSPI_SB_COMMIT_ERASED;
}

/* Fill bytes [0..24) of a header buffer from the in-memory struct.
 * Shared by compute_crc and serialize to avoid divergence. */
static void sb_fill_header(const qspi_superblock_t *sb,
                           uint8_t buf[QSPI_SB_CRC_OFFSET])
{
    buf[0]  = sb->magic[0];  buf[1]  = sb->magic[1];
    buf[2]  = sb->magic[2];  buf[3]  = sb->magic[3];
    buf[4]  = (uint8_t)(sb->version & 0xFF);
    buf[5]  = (uint8_t)((sb->version >> 8) & 0xFF);
    buf[6]  = sb->jedec_id[0]; buf[7]  = sb->jedec_id[1];
    buf[8]  = sb->jedec_id[2]; buf[9]  = sb->flags;
    buf[10] = (uint8_t)(sb->capacity_bytes & 0xFF);
    buf[11] = (uint8_t)((sb->capacity_bytes >> 8) & 0xFF);
    buf[12] = (uint8_t)((sb->capacity_bytes >> 16) & 0xFF);
    buf[13] = (uint8_t)((sb->capacity_bytes >> 24) & 0xFF);
    buf[14] = (uint8_t)(sb->sector_size & 0xFF);
    buf[15] = (uint8_t)((sb->sector_size >> 8) & 0xFF);
    buf[16] = (uint8_t)(sb->sector_count & 0xFF);
    buf[17] = (uint8_t)((sb->sector_count >> 8) & 0xFF);
    buf[18] = (uint8_t)((sb->sector_count >> 16) & 0xFF);
    buf[19] = (uint8_t)((sb->sector_count >> 24) & 0xFF);
    buf[20] = buf[21] = buf[22] = buf[23] = 0xFF;
}

uint32_t qspi_eval_sb_compute_crc(const qspi_superblock_t *sb)
{
    uint8_t buf[QSPI_SB_CRC_OFFSET];
    sb_fill_header(sb, buf);
    return qspi_crc32(buf, QSPI_SB_CRC_OFFSET);
}

void qspi_eval_sb_serialize(const qspi_superblock_t *sb,
                            uint8_t buf[QSPI_SB_HEADER_SIZE])
{
    sb_fill_header(sb, buf);
    buf[24] = (uint8_t)(sb->crc32 & 0xFF);
    buf[25] = (uint8_t)((sb->crc32 >> 8) & 0xFF);
    buf[26] = (uint8_t)((sb->crc32 >> 16) & 0xFF);
    buf[27] = (uint8_t)((sb->crc32 >> 24) & 0xFF);
    buf[28] = sb->commit_marker;
    buf[29] = buf[30] = buf[31] = 0xFF;
}

bool qspi_eval_sb_deserialize(const uint8_t buf[QSPI_SB_HEADER_SIZE],
                              qspi_superblock_t *sb)
{
    sb->magic[0] = buf[0];
    sb->magic[1] = buf[1];
    sb->magic[2] = buf[2];
    sb->magic[3] = buf[3];
    if (sb->magic[0] != QSPI_SB_MAGIC_0 ||
        sb->magic[1] != QSPI_SB_MAGIC_1 ||
        sb->magic[2] != QSPI_SB_MAGIC_2 ||
        sb->magic[3] != QSPI_SB_MAGIC_3) {
        return false;
    }
    sb->version = (uint16_t)buf[4] | ((uint16_t)buf[5] << 8);
    sb->jedec_id[0] = buf[6];
    sb->jedec_id[1] = buf[7];
    sb->jedec_id[2] = buf[8];
    sb->flags = buf[9];
    sb->capacity_bytes = (uint32_t)buf[10] | ((uint32_t)buf[11] << 8) |
                         ((uint32_t)buf[12] << 16) | ((uint32_t)buf[13] << 24);
    sb->sector_size = (uint16_t)buf[14] | ((uint16_t)buf[15] << 8);
    sb->sector_count = (uint32_t)buf[16] | ((uint32_t)buf[17] << 8) |
                       ((uint32_t)buf[18] << 16) | ((uint32_t)buf[19] << 24);
    sb->crc32 = (uint32_t)buf[24] | ((uint32_t)buf[25] << 8) |
                ((uint32_t)buf[26] << 16) | ((uint32_t)buf[27] << 24);
    sb->commit_marker = buf[28];
    return true;
}

bool qspi_eval_sb_validate(const qspi_superblock_t *sb)
{
    if (sb->magic[0] != QSPI_SB_MAGIC_0 ||
        sb->magic[1] != QSPI_SB_MAGIC_1 ||
        sb->magic[2] != QSPI_SB_MAGIC_2 ||
        sb->magic[3] != QSPI_SB_MAGIC_3) {
        return false;
    }
    if (sb->version != QSPI_SB_VERSION) return false;
    if (sb->jedec_id[0] != QSPI_JEDEC_MANUFACTURER ||
        sb->jedec_id[1] != QSPI_JEDEC_TYPE ||
        sb->jedec_id[2] != QSPI_JEDEC_CAPACITY) return false;
    if (sb->capacity_bytes != QSPI_FLASH_SIZE_BYTES) return false;
    if (sb->sector_size != QSPI_SECTOR_SIZE) return false;
    if (sb->sector_count != QSPI_SECTOR_COUNT) return false;
    if (sb->crc32 != qspi_eval_sb_compute_crc(sb)) return false;
    return true;
}

bool qspi_eval_sb_is_committed(const qspi_superblock_t *sb)
{
    return sb->commit_marker == QSPI_SB_COMMIT_VALUE;
}

bool qspi_eval_sb_buf_is_committed(const uint8_t buf[QSPI_SB_HEADER_SIZE])
{
    return buf[QSPI_SB_COMMIT_OFFSET] == QSPI_SB_COMMIT_VALUE;
}

#ifdef __cplusplus
}
#endif
