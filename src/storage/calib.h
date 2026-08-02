/*
 * calib.h - Calibration/profile/runtime persistence firmware wrapper.
 *
 * Wraps calib_eval.{h,c} (pure-logic) with QSPI journal operations.
 * Requires an adopted QSPI device (Todo 28) and an initialized
 * QspiJournal (Todo 29).
 *
 * Provides:
 *   - Store/load IMU/MAG/PROFILE calib records in the calib partition
 *   - Store/load runtime mode in the config partition
 *   - QSPI-backed runtime store functions for runtime.h backend
 *
 * Never persists without adoption. Never applies invalid stored data.
 *
 * Compiled only under BOARD_CLUE.
 */
#ifndef CALIB_H
#define CALIB_H

#ifdef BOARD_CLUE

#include <stdint.h>
#include <stdbool.h>

#include "calib_eval.h"
#include "qspi_journal.h"
#include "qspi_journal_eval.h"
#include "runtime.h"

#ifdef __cplusplus

class CalibManager {
public:
	CalibManager();

	/* Initialize with journal reference. Call after QspiJournal::init(). */
	void init(QspiJournal *journal);

	bool isReady(void) const { return m_journal != NULL; }

	/* Store a calibration record. Returns true on success.
	 * Fails silently if not adopted (storage not ready). */
	bool storeImu(uint8_t sensor_id, const uint8_t *blob, uint16_t len);
	bool storeMag(uint8_t sensor_id, const uint8_t *blob, uint16_t len);
	bool storeProfile(uint8_t profile_id, const uint8_t *blob, uint16_t len);

	/* Load the latest valid calibration record for the given kind+ref_id.
	 * Returns true if a valid record was found. */
	bool loadImu(uint8_t sensor_id, uint8_t *blob_out, uint16_t *len_out);
	bool loadMag(uint8_t sensor_id, uint8_t *blob_out, uint16_t *len_out);
	bool loadProfile(uint8_t profile_id, uint8_t *blob_out, uint16_t *len_out);

	/* Runtime mode persistence (config partition). */
	bool storeRuntimeMode(runtime_mode_t mode);
	bool loadRuntimeMode(runtime_mode_t *out_mode);

private:
	QspiJournal *m_journal;

	bool storeRecord(uint8_t journal_type, uint8_t kind, uint8_t ref_id,
			 const uint8_t *blob, uint16_t blob_len);
	bool loadRecord(uint8_t journal_type, uint8_t kind, uint8_t ref_id,
			uint8_t *blob_out, uint16_t *len_out);
};

/* C-callable wrappers for the runtime.h store_read/store_write backend. */
runtime_store_result_t calib_runtime_store_read(runtime_mode_t *out_mode);
runtime_store_result_t calib_runtime_store_write(runtime_mode_t mode);

/* Global accessor — set by main.cpp after CalibManager is constructed. */
void calib_set_global_manager(CalibManager *mgr);

#endif /* __cplusplus */
#endif /* BOARD_CLUE */
#endif /* CALIB_H */
