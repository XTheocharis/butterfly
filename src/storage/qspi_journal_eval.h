/*
 * qspi_journal_eval.h - Pure-C QSPI journal evaluation (host-testable).
 *
 * Extends Todo 28's frozen superblock with four partitions:
 *   8 KiB superblock (alternating) / 56 KiB config / 64 KiB calib / 1920 KiB log
 *
 * All decision logic lives here; the firmware wrapper (qspi_journal.cpp)
 * translates between nrfx_qspi hardware operations and these pure functions.
 *
 * CRC32: reuses qspi_crc32() from qspi_eval.h (ISO-HDLC variant).
 *
 * NOR commit invariant: state byte transitions 0xFF->0x01 (valid) ->
 * 0x00 (committed). NOR program can only clear bits, so commit is
 * always a 1->0 transition that cannot be faked by partial writes.
 */
#ifndef QSPI_JOURNAL_EVAL_H
#define QSPI_JOURNAL_EVAL_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "qspi_eval.h"  /* qspi_crc32, QSPI_SECTOR_SIZE, QSPI_FLASH_SIZE_BYTES */

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Partition layout (2 MiB total, 512 x 4 KiB sectors) ---- */

#define JOURNAL_SB_SECTORS        2u   /* sectors 0-1 */
#define JOURNAL_SB_SIZE           (JOURNAL_SB_SECTORS * QSPI_SECTOR_SIZE)
#define JOURNAL_SB0_ADDR          0u
#define JOURNAL_SB1_ADDR          QSPI_SECTOR_SIZE

#define JOURNAL_CONFIG_START      JOURNAL_SB_SIZE
#define JOURNAL_CONFIG_SECTORS    14u
#define JOURNAL_CONFIG_SIZE       (JOURNAL_CONFIG_SECTORS * QSPI_SECTOR_SIZE)

#define JOURNAL_CALIB_START       (JOURNAL_CONFIG_START + JOURNAL_CONFIG_SIZE)
#define JOURNAL_CALIB_SECTORS     16u
#define JOURNAL_CALIB_SIZE        (JOURNAL_CALIB_SECTORS * QSPI_SECTOR_SIZE)

#define JOURNAL_LOG_START         (JOURNAL_CALIB_START + JOURNAL_CALIB_SIZE)
#define JOURNAL_LOG_SECTORS \
	(QSPI_SECTOR_COUNT - JOURNAL_SB_SECTORS - JOURNAL_CONFIG_SECTORS - \
	 JOURNAL_CALIB_SECTORS)
#define JOURNAL_LOG_SIZE          (JOURNAL_LOG_SECTORS * QSPI_SECTOR_SIZE)

/* ---- Record format (little-endian) ----
 * [0..4)    Magic "BTFR"
 * [4]       format_version
 * [5]       header_size (= JOURNAL_REC_HEADER_SIZE)
 * [6]       record_type
 * [7..9)    payload_length u16 LE
 * [9..17)   sequence u64 LE (modular)
 * [17..25)  timestamp u64 LE
 * [25]      state/commit byte (0xFF=erased, 0x01=valid, 0x00=committed)
 * [26..30)  erase_generation u32 LE
 * [30..34)  header_crc32 u32 LE (covers [0..30))
 * [34..38)  payload_crc32 u32 LE (covers payload)
 * [38]      padding_byte (0xFF)
 * [39..N)   payload (<=1024 bytes)
 */

#define JOURNAL_REC_MAGIC_0       'B'
#define JOURNAL_REC_MAGIC_1       'T'
#define JOURNAL_REC_MAGIC_2       'F'
#define JOURNAL_REC_MAGIC_3       'R'
#define JOURNAL_REC_VERSION       1u
#define JOURNAL_REC_HEADER_SIZE   39u
#define JOURNAL_REC_MAX_PAYLOAD   1024u
#define JOURNAL_REC_MAX_SIZE      (JOURNAL_REC_HEADER_SIZE + JOURNAL_REC_MAX_PAYLOAD)

#define JOURNAL_REC_OFF_MAGIC      0u
#define JOURNAL_REC_OFF_VERSION    4u
#define JOURNAL_REC_OFF_HDR_SIZE   5u
#define JOURNAL_REC_OFF_TYPE       6u
#define JOURNAL_REC_OFF_PAYLEN     7u
#define JOURNAL_REC_OFF_SEQ        9u
#define JOURNAL_REC_OFF_TIMESTAMP  17u
#define JOURNAL_REC_OFF_STATE      25u
#define JOURNAL_REC_OFF_ERASE_GEN  26u
#define JOURNAL_REC_OFF_HDR_CRC    30u
#define JOURNAL_REC_OFF_PAY_CRC    34u
#define JOURNAL_REC_OFF_PADDING    38u
#define JOURNAL_REC_OFF_PAYLOAD    39u
#define JOURNAL_REC_HDR_CRC_SPAN   30u  /* bytes [0..30) */

/* NOR-compatible state byte transitions: 0xFF -> 0x01 -> 0x00 */
#define JOURNAL_REC_STATE_ERASED    0xFFu
#define JOURNAL_REC_STATE_VALID     0x01u
#define JOURNAL_REC_STATE_COMMITTED 0x00u

/* ---- Journal superblock extension (bytes [32..128) of SB sector) ----
 * [32..36)  Magic "JRNL"
 * [36]      version
 * [37]      active_sb_sector (0 or 1)
 * [38..42)  erase_generation u32 LE
 * [42..46)  log_write_off u32 LE (relative to LOG_START)
 * [46..50)  log_read_off  u32 LE (relative to LOG_START)
 * [50..58)  next_sequence u64 LE
 * [58..62)  log_erase_gen u32 LE
 * [62..66)  config_write_off u32 LE (relative to CONFIG_START)
 * [66..70)  config_erase_gen u32 LE
 * [70..74)  calib_write_off u32 LE (relative to CALIB_START)
 * [74..78)  calib_erase_gen u32 LE
 * [78..82)  log_record_count u32 LE
 * [82..86)  journal_crc32 u32 LE (covers [32..82))
 * [86]      journal_commit marker (0xFF=invalid, 0xC0=committed)
 * [87..128) reserved (0xFF)
 */
#define JOURNAL_SB_EXT_OFF         32u
#define JOURNAL_SB_EXT_SIZE        96u  /* [32..128) */
#define JOURNAL_SB_EXT_MAGIC       {'J','R','N','L'}
#define JOURNAL_SB_EXT_VERSION     1u
#define JOURNAL_SB_EXT_OFF_MAGIC   32u
#define JOURNAL_SB_EXT_OFF_VERSION 36u
#define JOURNAL_SB_EXT_OFF_ACTIVE  37u
#define JOURNAL_SB_EXT_OFF_ERASE   38u
#define JOURNAL_SB_EXT_OFF_LOG_WR  42u
#define JOURNAL_SB_EXT_OFF_LOG_RD  46u
#define JOURNAL_SB_EXT_OFF_SEQ     50u
#define JOURNAL_SB_EXT_OFF_LOG_EG  58u
#define JOURNAL_SB_EXT_OFF_CFG_WR  62u
#define JOURNAL_SB_EXT_OFF_CFG_EG  66u
#define JOURNAL_SB_EXT_OFF_CAL_WR  70u
#define JOURNAL_SB_EXT_OFF_CAL_EG  74u
#define JOURNAL_SB_EXT_OFF_COUNT   78u
#define JOURNAL_SB_EXT_OFF_CRC     82u
#define JOURNAL_SB_EXT_OFF_COMMIT  86u
#define JOURNAL_SB_EXT_CRC_SPAN    50u  /* [32..82) = 50 bytes */
#define JOURNAL_SB_EXT_COMMIT_VAL  0xC0u

/* ---- Record types ---- */

typedef enum {
	JOURNAL_REC_TYPE_INVALID = 0,
	JOURNAL_REC_TYPE_CONFIG  = 1,
	JOURNAL_REC_TYPE_CALIB   = 2,
	JOURNAL_REC_TYPE_LOG_EVT = 3,
	JOURNAL_REC_TYPE_LOG_PKT = 4,
	JOURNAL_REC_TYPE_GC_MARK = 5,
} journal_rec_type_t;

/* ---- Content categories for log filter ---- */

typedef enum {
	JOURNAL_CAT_BLOCKED     = 0,
	JOURNAL_CAT_BOARD_EVENT = 1,
	JOURNAL_CAT_PKT_META    = 2,
	JOURNAL_CAT_PKT_PAYLOAD = 3,
	JOURNAL_CAT_AUDIO_RAW   = 4,  /* ALWAYS excluded */
	JOURNAL_CAT_SECRET      = 5,  /* ALWAYS excluded */
	JOURNAL_CAT_NONCE       = 6,  /* ALWAYS excluded */
} journal_content_cat_t;

/* ---- Log queue (in-memory, bounded 16, drop-newest) ---- */

#define JOURNAL_LOG_QUEUE_SIZE 16u
#define JOURNAL_QUEUE_NEWEST_EMPTY 0xFFu

typedef struct {
	uint8_t  data[JOURNAL_REC_MAX_PAYLOAD];
	uint16_t len;
	uint8_t  type;
	bool     occupied;
} journal_queue_entry_t;

typedef struct {
	journal_queue_entry_t entries[JOURNAL_LOG_QUEUE_SIZE];
	uint32_t overrun_count;
	uint32_t count;
	uint8_t  newest_index;  /* slot of most recent push; 0xFF if empty */
} journal_log_queue_t;

/* ---- Partition selector ---- */

typedef enum {
	JOURNAL_PART_LOG    = 0,
	JOURNAL_PART_CONFIG = 1,
	JOURNAL_PART_CALIB  = 2,
} journal_part_t;

/* ---- Flash backend (pluggable for host/firmware) ---- */

typedef int (*journal_flash_read_fn)(uint32_t addr, void *buf, size_t len);
typedef int (*journal_flash_program_fn)(uint32_t addr, const void *buf, size_t len);
typedef int (*journal_flash_erase_fn)(uint32_t sector_addr);

typedef struct {
	journal_flash_read_fn   read;
	journal_flash_program_fn program;
	journal_flash_erase_fn  erase;
} journal_flash_backend_t;

/* ---- In-memory structs ---- */

typedef struct {
	uint8_t  magic[4];
	uint8_t  version;
	uint8_t  header_size;
	uint8_t  record_type;
	uint16_t payload_length;
	uint64_t sequence;
	uint64_t timestamp;
	uint8_t  state;
	uint32_t erase_generation;
	uint32_t header_crc32;
	uint32_t payload_crc32;
	uint8_t  padding;
	uint8_t  payload[JOURNAL_REC_MAX_PAYLOAD];
} journal_record_t;

typedef struct {
	bool     valid;
	uint8_t  active_sb_sector;
	uint32_t erase_generation;
	uint32_t log_write_off;
	uint32_t log_read_off;
	uint64_t next_sequence;
	uint32_t log_erase_gen;
	uint32_t config_write_off;
	uint32_t config_erase_gen;
	uint32_t calib_write_off;
	uint32_t calib_erase_gen;
	uint32_t log_record_count;
} journal_sb_info_t;

/* ---- StorageReadLog cursor ---- */

typedef struct {
	uint32_t read_off;
	uint32_t records_read;
	bool     eof;
} journal_read_cursor_t;

/* ---- StorageEraseLog FSM ---- */

typedef enum {
	JOURNAL_ERASE_IDLE     = 0,
	JOURNAL_ERASE_ACCEPTED,
	JOURNAL_ERASE_ERASING,
	JOURNAL_ERASE_DONE,
	JOURNAL_ERASE_ERROR,
} journal_erase_state_t;

typedef enum {
	JOURNAL_ERASE_EVT_CONFIRM = 0,
	JOURNAL_ERASE_EVT_PROGRESS,
	JOURNAL_ERASE_EVT_COMPLETE,
	JOURNAL_ERASE_EVT_FAIL,
	JOURNAL_ERASE_EVT_CANCEL,
} journal_erase_event_t;

/* ---- GC decision ---- */

typedef enum {
	JOURNAL_GC_NOT_NEEDED = 0,
	JOURNAL_GC_NEEDED,
} journal_gc_result_t;

/* ---- Queue push result ---- */

typedef enum {
	JOURNAL_QUEUE_OK       = 0,
	JOURNAL_QUEUE_DROPPED,
	JOURNAL_QUEUE_FULL,
} journal_queue_result_t;

/* ---- Functions ---- */

/* Modular sequence: returns true if a is strictly after b (handles wrap). */
bool journal_seq_after(uint64_t a, uint64_t b);

/* Record init / serialize / deserialize / validate */
void journal_rec_init(journal_record_t *rec, uint8_t type,
		      uint64_t seq, uint64_t ts,
		      const uint8_t *payload, uint16_t len);
uint32_t journal_rec_header_crc(const journal_record_t *rec);
uint32_t journal_rec_payload_crc(const journal_record_t *rec);
void journal_rec_serialize(const journal_record_t *rec, uint8_t *buf);
bool journal_rec_deserialize(const uint8_t *buf, size_t buf_len,
			     journal_record_t *rec);
bool journal_rec_validate(const journal_record_t *rec);
bool journal_rec_is_committed(const journal_record_t *rec);
bool journal_rec_is_erased(const uint8_t *buf, size_t len);

/* Superblock extension init / serialize / deserialize / validate */
void journal_sb_ext_init(journal_sb_info_t *sb);
uint32_t journal_sb_ext_compute_crc(const journal_sb_info_t *sb);
void journal_sb_ext_serialize(const journal_sb_info_t *sb, uint8_t *buf);
bool journal_sb_ext_deserialize(const uint8_t *buf, journal_sb_info_t *sb);
bool journal_sb_ext_validate(const journal_sb_info_t *sb);

/* Boot scan: reads both superblock sectors, returns the active one.
 * Requires Todo 28 commit marker at [28] and journal commit at [86]. */
bool journal_scan_superblocks(journal_flash_read_fn read_fn,
			      journal_sb_info_t *out);

/* Commit the journal superblock to flash. Alternates to the other SB
 * sector (0->1 or 1->0), bumps erase_generation, copies the qspi_eval
 * header from the current sector, and writes the serialized journal
 * extension with the 0xC0 commit marker. The previous sector remains
 * valid until the new sector is fully written, so a power-cut during
 * commit leaves the old SB intact. */
bool journal_commit_superblock(journal_flash_backend_t *backend,
			       journal_sb_info_t *sb);

/* Circular log: append a record at log_write_off, advancing the write
 * pointer. Returns false if write fails or log is full and cannot
 * reclaim (all sectors consumed). On wrap, log_read_off advances. */
bool journal_log_append(journal_flash_backend_t *backend,
			journal_sb_info_t *sb,
			const journal_record_t *rec);

/* Append a record to a non-log partition (config or calib).
 * Synchronous: writes header + payload, then commit byte.
 * Updates sb->{config,calib}_write_off and sb->next_sequence.
 * Returns false on flash failure or partition full. */
bool journal_partition_append(journal_flash_backend_t *backend,
			      journal_sb_info_t *sb,
			      journal_part_t part,
			      const journal_record_t *rec);

/* Read one record at byte offset within a partition.
 * Returns false if no valid committed record at that offset. */
bool journal_partition_read_one(journal_flash_read_fn read_fn,
				const journal_sb_info_t *sb,
				journal_part_t part,
				uint32_t off,
				journal_record_t *out);

/* Read one record from the log at the given offset.
 * Returns false if no valid committed record at that offset. */
bool journal_log_read_one(journal_flash_read_fn read_fn,
			  const journal_sb_info_t *sb,
			  uint32_t log_off,
			  journal_record_t *out);

/* Check if log partition is full (write_off would pass read_off). */
bool journal_log_is_full(const journal_sb_info_t *sb);

/* GC decision: is garbage collection needed for a partition sector
 * based on its write offset? */
journal_gc_result_t journal_gc_check(uint32_t write_off, uint32_t part_size);

/* Evaluate record replacement: new record should supersede old
 * if same type and sequence is later. */
bool journal_gc_should_replace(const journal_record_t *old_rec,
			       const journal_record_t *new_rec);

/* Log queue */
void journal_queue_init(journal_log_queue_t *q);
journal_queue_result_t journal_queue_push(journal_log_queue_t *q,
					  uint8_t type,
					  const uint8_t *data,
					  uint16_t len);
bool journal_queue_pop(journal_log_queue_t *q,
		       uint8_t *type_out,
		       uint8_t *data_out,
		       uint16_t *len_out);

/* Content filter: determines if a category should be logged. */
bool journal_filter_allows(journal_content_cat_t cat);

/* StorageReadLog: compute the next chunk fragment from a cursor.
 * max_chunk is <= WHAD_MAX_ENCODED_MESSAGE_SIZE (1019).
 * Returns the fragment size; sets eof when no more data. */
uint16_t journal_readlog_chunk(const uint8_t *data, uint16_t data_len,
			       uint16_t offset, uint16_t max_chunk,
			       uint8_t *out, bool *eof);

/* StorageEraseLog FSM */
journal_erase_state_t journal_eraselog_transition(
	journal_erase_state_t current, journal_erase_event_t evt);

/* Pre-erase idle check: returns true only when no timing-critical
 * operation is active. busy_flags is a bitmask; bit 0 = radio active,
 * bit 1 = BLE connected, bit 2 = sensor sampling, bit 3 = display update. */
bool journal_eval_is_idle(uint32_t busy_flags);

#ifdef __cplusplus
}
#endif

#endif /* QSPI_JOURNAL_EVAL_H */
