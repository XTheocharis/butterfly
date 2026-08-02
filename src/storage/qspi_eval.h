/*
 * qspi_eval.h - Pure-C QSPI storage evaluation functions (host-testable).
 *
 * Contains ALL decision logic for the QSPI probe + adoption flow:
 *   - JEDEC RDID validation (C8 40 15 = GigaDevice GD25Q16C 2 MiB)
 *   - Pre/post-adoption command trace classification
 *   - Nonce lifecycle FSM (generate → valid → consumed/expired/invalidated)
 *   - Adoption state machine (UNADOPTED → … → ADOPTED)
 *   - Superblock format (magic/version/CRC/commit-marker)
 *   - CRC32 (ISO-HDLC: reflected poly 0xEDB88320)
 *
 * No SDK deps. Compiled from both the firmware wrapper (qspi.cpp) and
 * host tests (test_qspi.cpp). The superblock prefix/golden vector is
 * FROZEN here — Todo 29 (journal) extends this format.
 *
 * NOR flash safety invariant: the commit marker is written LAST. A
 * power-cut at any point before the final commit program operation
 * leaves the device UNADOPTED. This is enforced by NOR AND semantics:
 * program can only clear bits (1→0), so 0xFF (erased) → 0xC0 (commit)
 * is a one-way transition that cannot be faked by partial writes.
 */
#ifndef QSPI_EVAL_H
#define QSPI_EVAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- GD25Q16C hardware constants ---- */

#define QSPI_JEDEC_MANUFACTURER   0xC8u  /* GigaDevice */
#define QSPI_JEDEC_TYPE           0x40u
#define QSPI_JEDEC_CAPACITY       0x15u  /* 2^21 = 2 MiB */
#define QSPI_FLASH_SIZE_BYTES     (2u * 1024u * 1024u)
#define QSPI_SECTOR_SIZE          4096u
#define QSPI_SECTOR_COUNT         (QSPI_FLASH_SIZE_BYTES / QSPI_SECTOR_SIZE)

/* Superblock occupies sectors 0 and 1 (8 KiB reserved). */
#define QSPI_SUPERBLOCK_SECTORS   2u
#define QSPI_DATA_SECTOR_START    QSPI_SUPERBLOCK_SECTORS

/* ---- SPI opcodes ---- */

/* JEDEC RDID is 0x9F — NOT fast-read 0x0B. */
#define QSPI_OP_JEDEC_ID          0x9Fu

/* Post-adoption allowed opcodes (single-I/O baseline). */
#define QSPI_OP_WRITE_ENABLE      0x06u  /* mandatory before page program */
#define QSPI_OP_FAST_READ         0x0Bu
#define QSPI_OP_PAGE_PROGRAM      0x02u

/* Forbidden pre-adoption opcodes. */
#define QSPI_OP_SECTOR_ERASE_4K   0x20u
#define QSPI_OP_BLOCK_ERASE_32K   0x52u
#define QSPI_OP_BLOCK_ERASE_64K   0xD8u
#define QSPI_OP_CHIP_ERASE        0xC7u
#define QSPI_OP_WRITE_STATUS1     0x01u
#define QSPI_OP_WRITE_STATUS2     0x31u

/* ---- Nonce constants ---- */

#define QSPI_NONCE_SIZE           16u
#define QSPI_NONCE_TTL_US         (60ULL * 1000ULL * 1000ULL)  /* 60 s */

/* ---- Superblock format (FROZEN — Todo 29 extends) ---- */

/* Header is 32 bytes. Layout:
 *   [0..4)   Magic "BTFQ"
 *   [4..6)   Version (u16 LE)
 *   [6..9)   JEDEC ID (C8 40 15)
 *   [9]      Flags
 *   [10..14) Capacity bytes (u32 LE)
 *   [14..16) Sector size (u16 LE)
 *   [16..20) Sector count (u32 LE)
 *   [20..24) Reserved (0xFF 0xFF 0xFF 0xFF)
 *   [24..28) CRC32 (u32 LE, computed over [0..24))
 *   [28]     Commit marker (0xFF=uncommitted, 0xC0=committed)
 *   [29..32) Reserved (0xFF 0xFF 0xFF)
 */
#define QSPI_SB_MAGIC_0           'B'
#define QSPI_SB_MAGIC_1           'T'
#define QSPI_SB_MAGIC_2           'F'
#define QSPI_SB_MAGIC_3           'Q'
#define QSPI_SB_VERSION           1u
#define QSPI_SB_HEADER_SIZE       32u
#define QSPI_SB_CRC_OFFSET        24u
#define QSPI_SB_COMMIT_OFFSET     28u
#define QSPI_SB_COMMIT_VALUE      0xC0u
#define QSPI_SB_COMMIT_ERASED     0xFFu
#define QSPI_SB_ADDR              0u

/* ---- Types ---- */

/* Storage adoption state (maps to Board proto StorageState). */
typedef enum {
    QSPI_ST_UNADOPTED = 0,
    QSPI_ST_PROBING,
    QSPI_ST_VALIDATED,
    QSPI_ST_NONCE_ISSUED,
    QSPI_ST_CONFIRMED,
    QSPI_ST_ERASING_SUPERBLOCK,
    QSPI_ST_ERASING_REMAINING,
    QSPI_ST_VERIFYING,
    QSPI_ST_WRITING_SUPERBLOCK,
    QSPI_ST_COMMITTING,
    QSPI_ST_ADOPTED,
} qspi_state_t;

/* Adoption FSM events. */
typedef enum {
    QSPI_EVT_PROBE_OK = 0,
    QSPI_EVT_PROBE_FAIL,
    QSPI_EVT_ID_MATCH,
    QSPI_EVT_ID_MISMATCH,
    QSPI_EVT_NONCE_ISSUED,
    QSPI_EVT_CONFIRMED,
    QSPI_EVT_CONFIRM_FAIL,
    QSPI_EVT_ERASE_SB_OK,
    QSPI_EVT_ERASE_SB_FAIL,
    QSPI_EVT_ERASE_REM_OK,
    QSPI_EVT_ERASE_REM_FAIL,
    QSPI_EVT_VERIFY_OK,
    QSPI_EVT_VERIFY_FAIL,
    QSPI_EVT_WRITE_SB_OK,
    QSPI_EVT_WRITE_SB_FAIL,
    QSPI_EVT_COMMIT_OK,
    QSPI_EVT_COMMIT_FAIL,
    QSPI_EVT_RESET,
} qspi_event_t;

/* Nonce lifecycle states. */
typedef enum {
    QSPI_NONCE_EMPTY = 0,
    QSPI_NONCE_VALID,
    QSPI_NONCE_CONSUMED,
    QSPI_NONCE_EXPIRED,
    QSPI_NONCE_INVALIDATED,
} qspi_nonce_state_t;

/* Nonce check result. */
typedef enum {
    QSPI_NONCE_NO_MATCH = 0,
    QSPI_NONCE_MATCH_OK,
    QSPI_NONCE_RESULT_EXPIRED,
    QSPI_NONCE_ALREADY_USED,
    QSPI_NONCE_NONE_ISSUED,
} qspi_nonce_result_t;

/* Command trace entry for classification. */
typedef struct {
    uint8_t opcode;
    bool    is_write;
} qspi_cmd_trace_t;

typedef enum {
    QSPI_TRACE_PASS = 0,
    QSPI_TRACE_FAIL,
} qspi_trace_result_t;

/* Nonce tracker (stateful). */
typedef struct {
    qspi_nonce_state_t state;
    uint8_t  bytes[QSPI_NONCE_SIZE];
    uint64_t issued_us;
    uint64_t consumed_us;
} qspi_nonce_tracker_t;

/* In-memory superblock representation. */
typedef struct {
    uint8_t  magic[4];
    uint16_t version;
    uint8_t  jedec_id[3];
    uint8_t  flags;
    uint32_t capacity_bytes;
    uint16_t sector_size;
    uint32_t sector_count;
    uint32_t crc32;
    uint8_t  commit_marker;
} qspi_superblock_t;

/* ---- CRC32 (ISO-HDLC: poly 0xEDB88320 reflected, init 0xFFFFFFFF, final XOR 0xFFFFFFFF) ---- */

uint32_t qspi_crc32(const uint8_t *data, size_t len);

/* ---- Command trace classification ---- */

qspi_trace_result_t qspi_eval_trace_pre_adopt(
    const qspi_cmd_trace_t *trace, size_t count);

qspi_trace_result_t qspi_eval_trace_post_adopt(
    const qspi_cmd_trace_t *trace, size_t count);

/* ---- JEDEC ID validation ---- */

bool qspi_eval_jedec_match(const uint8_t rdid[3]);

/* ---- Adoption state machine ---- */

qspi_state_t qspi_eval_adopt_transition(
    qspi_state_t current, qspi_event_t evt);

bool qspi_eval_state_is_adopted(qspi_state_t state);
bool qspi_eval_state_is_unadopted(qspi_state_t state);

/* ---- Nonce lifecycle ---- */

void qspi_eval_nonce_init(qspi_nonce_tracker_t *t);

/* Issue a new nonce. If a nonce was already VALID, the old one is
 * INVALIDATED (new request supersedes old). State → VALID. */
void qspi_eval_nonce_issue(qspi_nonce_tracker_t *t,
                           const uint8_t bytes[QSPI_NONCE_SIZE],
                           uint64_t now_us);

/* Check a candidate nonce. Returns MATCH_OK + consumes if valid match.
 * Returns EXPIRED if matched but TTL exceeded (also invalidates).
 * Returns ALREADY_USED if nonce was CONSUMED.
 * Returns NO_MATCH if bytes differ or state is INVALIDATED.
 * Returns NONE_ISSUED if state is EMPTY. */
qspi_nonce_result_t qspi_eval_nonce_check(qspi_nonce_tracker_t *t,
                                          const uint8_t *candidate,
                                          size_t len,
                                          uint64_t now_us);

/* Force-invalidate the nonce (device reset, failed attempt). */
void qspi_eval_nonce_invalidate(qspi_nonce_tracker_t *t);

/* Check if nonce is currently valid and not expired. */
bool qspi_eval_nonce_is_valid(const qspi_nonce_tracker_t *t,
                              uint64_t now_us);

/* ---- Superblock operations ---- */

void qspi_eval_sb_init(qspi_superblock_t *sb);

/* Compute CRC32 over the header fields [0..CRC_OFFSET).
 * Call this BEFORE setting crc32 in the struct. */
uint32_t qspi_eval_sb_compute_crc(const qspi_superblock_t *sb);

/* Serialize to a 32-byte buffer (caller provides QSPI_SB_HEADER_SIZE bytes). */
void qspi_eval_sb_serialize(const qspi_superblock_t *sb,
                            uint8_t buf[QSPI_SB_HEADER_SIZE]);

/* Deserialize from a 32-byte buffer. Returns false if magic mismatch. */
bool qspi_eval_sb_deserialize(const uint8_t buf[QSPI_SB_HEADER_SIZE],
                              qspi_superblock_t *sb);

/* Validate magic, version, JEDEC ID, capacity, CRC32. */
bool qspi_eval_sb_validate(const qspi_superblock_t *sb);

/* Check if commit marker indicates a committed superblock. */
bool qspi_eval_sb_is_committed(const qspi_superblock_t *sb);

/* Check if a raw byte buffer at the commit offset indicates committed. */
bool qspi_eval_sb_buf_is_committed(const uint8_t buf[QSPI_SB_HEADER_SIZE]);

#ifdef __cplusplus
}
#endif

#endif /* QSPI_EVAL_H */
