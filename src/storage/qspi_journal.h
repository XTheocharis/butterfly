/*
 * qspi_journal.h - QSPI journal firmware manager (BOARD_CLUE only).
 *
 * Wraps qspi_journal_eval.{h,c} (pure-logic) with nrfx_qspi hardware
 * operations. Requires an adopted QSPI device (qspi.h QspiManager).
 *
 * Provides:
 *   - Four-partition journal (superblock / config / calib / log)
 *   - CRC32-verified records with NOR-compatible commit
 *   - Circular log with power-loss recovery
 *   - 16-entry bounded log queue with drop-newest overrun counting
 *   - StorageReadLog / StorageEraseLog command handlers
 *
 * The journal never writes before adoption. Pre-erase operations run
 * only from scheduler-approved idle state.
 */
#ifndef QSPI_JOURNAL_H
#define QSPI_JOURNAL_H

#ifdef BOARD_CLUE

#include <stdint.h>
#include <stdbool.h>

#include "qspi_journal_eval.h"
#include "pinRegistry.h"

#ifdef __cplusplus

class QspiJournal {
public:
	QspiJournal();

	/* Initialize after QspiManager::isAdopted() returns true.
	 * Scans both superblock sectors and recovers journal state.
	 * Returns false if no valid committed superblock found. */
	bool init(pinreg_token_t group_lease);

	bool isReady(void) const { return m_ready; }

	/* Log operations (only when adopted + enabled by SetRuntimeConfig). */
	bool logEnabled(void) const { return m_logEnabled; }
	void setLogEnabled(bool ena) { m_logEnabled = ena; }

	/* Append a Board event or raw-WHAD packet metadata to the log.
	 * Returns false if not adopted, not enabled, or queue full. */
	bool appendLog(uint8_t type, const uint8_t *data, uint16_t len);

	/* Synchronous append to the calib/config partition (no queue).
	 * Commits the superblock after the write so the record survives
	 * power-cycle. Returns false if not ready or flash failure. */
	bool appendCalib(uint8_t kind, const uint8_t *payload, uint16_t len);
	bool appendConfig(uint8_t kind, const uint8_t *payload, uint16_t len);

	/* Read one committed record at byte offset within a partition.
	 * Used by CalibManager::loadRecord to iterate the calib/config
	 * partition on boot. Returns false if no valid record at offset. */
	bool readPartitionRecord(journal_part_t part, uint32_t off,
				 journal_record_t *out) const;
	uint32_t partitionWriteOff(journal_part_t part) const;

	/* Drain the in-memory log queue to flash. Call from main loop. */
	void tick(void);

	/* StorageReadLog: reads log records from cursor, fragments into
	 * LogChunk messages <= WHAD_MAX_ENCODED_MESSAGE_SIZE (1019). */
	bool readLog(journal_read_cursor_t *cursor,
		     uint8_t *chunk_out, uint16_t max_chunk,
		     uint16_t *actual_len, bool *eof);

	/* StorageEraseLog: erases the log partition.
	 * Requires explicit confirmation. */
	bool beginEraseLog(void);
	journal_erase_state_t eraseState(void) const { return m_eraseState; }
	bool tickEraseLog(void);

	/* Queue overrun counter for diagnostics. */
	uint32_t overrunCount(void) const { return m_queue.overrun_count; }

	/* Journal superblock info (for storage dashboard). */
	const journal_sb_info_t *sbInfo(void) const { return &m_sb; }

private:
	static int s_flashRead(uint32_t addr, void *buf, size_t len);
	static int s_flashProgram(uint32_t addr, const void *buf, size_t len);
	static int s_flashErase(uint32_t sector_addr);

	bool commitSuperblock(void);

	bool              m_ready;
	bool              m_logEnabled;
	pinreg_token_t    m_groupLease;
	journal_sb_info_t m_sb;
	journal_log_queue_t m_queue;
	journal_erase_state_t m_eraseState;
	uint32_t          m_eraseProgressSector;
};

#endif /* __cplusplus */
#endif /* BOARD_CLUE */
#endif /* QSPI_JOURNAL_H */
