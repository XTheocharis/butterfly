/*
 * qspi_journal.cpp - QSPI journal firmware wrapper (BOARD_CLUE only).
 *
 * Translates between nrfx_qspi hardware operations and the pure-logic
 * functions in qspi_journal_eval.c. The firmware build is currently
 * blocked by NRFX_PWM0_INST_IDX (Todo 1 issue); this file is
 * structurally correct and will compile when that is resolved.
 *
 * Includes qspi_journal_eval.c directly (no separate SRC_FILE — avoids
 * duplicate symbols with test builds that include the same .c file).
 */
#ifdef BOARD_CLUE

#include "qspi_journal.h"

/* Include the pure-logic eval directly. qspi_eval.c is already included
 * by qspi.cpp, so qspi_crc32 is available at link time. */
extern "C" {
#include "qspi_journal_eval.c"
}

#include "nrfx_qspi.h"
#include <string.h>

/* ---- Static flash backend (routes to nrfx_qspi) ---- */

int QspiJournal::s_flashRead(uint32_t addr, void *buf, size_t len)
{
	nrfx_qspi_read(buf, len, addr);
	return 0;
}

int QspiJournal::s_flashProgram(uint32_t addr, const void *buf, size_t len)
{
	/* GD25Q16C page size = 256 bytes. Page program crosses page
	 * boundaries correctly on this chip (continuous program). */
	const uint8_t *p = (const uint8_t *)buf;
	uint32_t off = addr;
	size_t remaining = len;
	while (remaining > 0) {
		size_t chunk = (remaining > 256) ? 256 : remaining;
		nrfx_qspi_write(p, chunk, off);
		p += chunk;
		off += chunk;
		remaining -= chunk;
	}
	return 0;
}

int QspiJournal::s_flashErase(uint32_t sector_addr)
{
	nrfx_qspi_erase(NRF_QSPI_ERASE_LEN_4KB, sector_addr);
	return 0;
}

/* ---- Constructor ---- */

QspiJournal::QspiJournal()
	: m_ready(false)
	, m_logEnabled(false)
	, m_groupLease(0)
	, m_eraseState(JOURNAL_ERASE_IDLE)
	, m_eraseProgressSector(0)
{
	memset(&m_sb, 0, sizeof(m_sb));
	journal_queue_init(&m_queue);
}

/* ---- Init ---- */

bool QspiJournal::init(pinreg_token_t group_lease)
{
	m_groupLease = group_lease;

	/* Scan both superblock sectors for the active journal state. */
	journal_flash_read_fn read_fn = s_flashRead;
	if (!journal_scan_superblocks(read_fn, &m_sb)) {
		/* No valid journal — device was adopted but journal never
		 * initialized. Start fresh. */
		journal_sb_ext_init(&m_sb);
		m_sb.valid = false;
		m_ready = false;
		return false;
	}

	m_ready = true;
	return true;
}

/* ---- Log append (via in-memory queue) ---- */

bool QspiJournal::appendLog(uint8_t type, const uint8_t *data, uint16_t len)
{
	if (!m_ready || !m_logEnabled) return false;
	if (len > JOURNAL_REC_MAX_PAYLOAD) return false;

	journal_queue_result_t r =
		journal_queue_push(&m_queue, type, data, len);
	return (r == JOURNAL_QUEUE_OK || r == JOURNAL_QUEUE_DROPPED);
}

/* ---- Synchronous partition append (calib/config) ---- */

bool QspiJournal::appendCalib(uint8_t kind, const uint8_t *payload,
			      uint16_t len)
{
	if (!m_ready || len > JOURNAL_REC_MAX_PAYLOAD) return false;

	journal_record_t rec;
	journal_rec_init(&rec, kind, m_sb.next_sequence, 0, payload, len);

	journal_flash_backend_t backend = {
		s_flashRead, s_flashProgram, s_flashErase
	};
	if (!journal_partition_append(&backend, &m_sb,
				      JOURNAL_PART_CALIB, &rec))
		return false;

	return journal_commit_superblock(&backend, &m_sb);
}

bool QspiJournal::appendConfig(uint8_t kind, const uint8_t *payload,
			       uint16_t len)
{
	if (!m_ready || len > JOURNAL_REC_MAX_PAYLOAD) return false;

	journal_record_t rec;
	journal_rec_init(&rec, kind, m_sb.next_sequence, 0, payload, len);

	journal_flash_backend_t backend = {
		s_flashRead, s_flashProgram, s_flashErase
	};
	if (!journal_partition_append(&backend, &m_sb,
				      JOURNAL_PART_CONFIG, &rec))
		return false;

	return journal_commit_superblock(&backend, &m_sb);
}

/* ---- Partition read (for CalibManager::loadRecord) ---- */

bool QspiJournal::readPartitionRecord(journal_part_t part, uint32_t off,
				      journal_record_t *out) const
{
	return journal_partition_read_one(s_flashRead, &m_sb, part, off, out);
}

uint32_t QspiJournal::partitionWriteOff(journal_part_t part) const
{
	switch (part) {
	case JOURNAL_PART_CONFIG: return m_sb.config_write_off;
	case JOURNAL_PART_CALIB:  return m_sb.calib_write_off;
	case JOURNAL_PART_LOG:    return m_sb.log_write_off;
	default:                  return 0;
	}
}

/* ---- Superblock commit (alternate-sector publish) ---- */

bool QspiJournal::commitSuperblock(void)
{
	journal_flash_backend_t backend = {
		s_flashRead, s_flashProgram, s_flashErase
	};
	return journal_commit_superblock(&backend, &m_sb);
}

/* ---- Tick: drain queue to flash ---- */

void QspiJournal::tick(void)
{
	if (!m_ready || !m_logEnabled) return;

	uint8_t type;
	uint8_t data[JOURNAL_REC_MAX_PAYLOAD];
	uint16_t len;
	bool any_appended = false;

	while (journal_queue_pop(&m_queue, &type, data, &len)) {
		journal_record_t rec;
		uint64_t ts = 0; /* timebase_now_us() — firmware provides */
		journal_rec_init(&rec, type, m_sb.next_sequence, ts, data, len);

		journal_flash_backend_t backend = {
			s_flashRead, s_flashProgram, s_flashErase
		};
		if (!journal_log_append(&backend, &m_sb, &rec))
			break; /* flash write failed — try next tick */
		any_appended = true;
	}

	if (any_appended)
		commitSuperblock();
}

/* ---- StorageReadLog ---- */

bool QspiJournal::readLog(journal_read_cursor_t *cursor,
			  uint8_t *chunk_out, uint16_t max_chunk,
			  uint16_t *actual_len, bool *eof)
{
	if (!m_ready) return false;

	journal_record_t rec;
	if (!journal_log_read_one(s_flashRead, &m_sb,
				  cursor->read_off, &rec)) {
		*eof = true;
		*actual_len = 0;
		return false;
	}

	/* Serialize record and fragment into chunks. */
	uint8_t buf[JOURNAL_REC_MAX_SIZE];
	journal_rec_serialize(&rec, buf);
	uint16_t total = JOURNAL_REC_HEADER_SIZE + rec.payload_length;

	uint16_t chunk = journal_readlog_chunk(buf, total, cursor->read_off % max_chunk,
					       max_chunk, chunk_out, eof);
	*actual_len = chunk;

	/* Advance cursor to next record. */
	if (*eof) {
		cursor->read_off += total;
		cursor->records_read++;
	}

	return true;
}

/* ---- StorageEraseLog ---- */

bool QspiJournal::beginEraseLog(void)
{
	if (!m_ready) return false;
	m_eraseState = journal_eraselog_transition(
		m_eraseState, JOURNAL_ERASE_EVT_CONFIRM);
	m_eraseProgressSector = 0;
	return m_eraseState == JOURNAL_ERASE_ACCEPTED;
}

bool QspiJournal::tickEraseLog(void)
{
	if (m_eraseState == JOURNAL_ERASE_ACCEPTED) {
		/* Check idle before erasing. */
		if (!journal_eval_is_idle(0)) /* busy_flags from scheduler */
			return true;
		m_eraseState = journal_eraselog_transition(
			m_eraseState, JOURNAL_ERASE_EVT_PROGRESS);
	}

	if (m_eraseState == JOURNAL_ERASE_ERASING) {
		/* Erase one log sector per tick. */
		if (m_eraseProgressSector < JOURNAL_LOG_SECTORS) {
			uint32_t addr = JOURNAL_LOG_START +
				m_eraseProgressSector * QSPI_SECTOR_SIZE;
			s_flashErase(addr);
			m_eraseProgressSector++;
			return true; /* more work */
		}
		/* Done — reset log pointers. */
		m_sb.log_write_off = 0;
		m_sb.log_read_off = 0;
		m_sb.next_sequence = 1;
		m_sb.log_record_count = 0;
		m_eraseState = journal_eraselog_transition(
			m_eraseState, JOURNAL_ERASE_EVT_COMPLETE);
		commitSuperblock();
	}

	return m_eraseState == JOURNAL_ERASE_ERASING;
}

#endif /* BOARD_CLUE */
