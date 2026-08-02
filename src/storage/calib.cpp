/*
 * calib.cpp - Calibration/profile/runtime persistence firmware wrapper.
 *
 * Wraps calib_eval.{h,c} with QSPI journal operations.
 * Compiled only under BOARD_CLUE.
 */
#ifdef BOARD_CLUE

#include "calib.h"
#include "qspi_eval.h"
#include <string.h>

/* Include the pure-logic eval. */
#include "calib_eval.c"

/* ---- Global manager (for runtime store backend) -------------------- */

static CalibManager *s_calib_mgr = NULL;

void calib_set_global_manager(CalibManager *mgr)
{
	s_calib_mgr = mgr;
	if (mgr != NULL) {
		calib_eval_set_adopted(mgr->isReady());
	}
}

/* ---- C-callable runtime store backend ------------------------------- */

runtime_store_result_t calib_runtime_store_read(runtime_mode_t *out_mode)
{
	if (s_calib_mgr == NULL || !s_calib_mgr->isReady()) {
		return RUNTIME_STORE_UNAVAILABLE;
	}
	runtime_mode_t mode;
	if (s_calib_mgr->loadRuntimeMode(&mode)) {
		if (runtime_mode_is_valid(mode)) {
			*out_mode = mode;
			return RUNTIME_STORE_OK;
		}
		return RUNTIME_STORE_INVALID;
	}
	return RUNTIME_STORE_UNAVAILABLE;
}

runtime_store_result_t calib_runtime_store_write(runtime_mode_t mode)
{
	if (s_calib_mgr == NULL || !s_calib_mgr->isReady()) {
		return RUNTIME_STORE_UNAVAILABLE;
	}
	if (!runtime_mode_is_valid(mode)) {
		return RUNTIME_STORE_INVALID;
	}
	if (s_calib_mgr->storeRuntimeMode(mode)) {
		return RUNTIME_STORE_OK;
	}
	return RUNTIME_STORE_IO_ERROR;
}

/* ---- CalibManager --------------------------------------------------- */

CalibManager::CalibManager()
	: m_journal(NULL)
{
}

void CalibManager::init(QspiJournal *journal)
{
	m_journal = journal;
	calib_eval_set_adopted(journal != NULL && journal->isReady());
}

bool CalibManager::storeRecord(uint8_t journal_type, uint8_t kind,
			       uint8_t ref_id,
			       const uint8_t *blob, uint16_t blob_len)
{
	if (m_journal == NULL || !m_journal->isReady()) {
		return false;
	}

	calib_record_t rec;
	calib_rec_init(&rec, kind, ref_id, blob, blob_len);

	calib_result_t vr = calib_rec_validate(&rec);
	if (vr != CALIB_OK) {
		return false;
	}

	uint8_t payload[CALIB_PAYLOAD_MAX];
	uint16_t plen = calib_rec_serialize(&rec, payload, sizeof(payload));
	if (plen == 0) {
		return false;
	}

	switch (journal_type) {
	case JOURNAL_REC_TYPE_CALIB:
		return m_journal->appendCalib(kind, payload, plen);
	case JOURNAL_REC_TYPE_CONFIG:
		return m_journal->appendConfig(kind, payload, plen);
	default:
		return m_journal->appendLog(kind, payload, plen);
	}
}

bool CalibManager::loadRecord(uint8_t journal_type, uint8_t kind,
			      uint8_t ref_id,
			      uint8_t *blob_out, uint16_t *len_out)
{
	if (m_journal == NULL || !m_journal->isReady()) {
		return false;
	}

	journal_part_t part;
	switch (journal_type) {
	case JOURNAL_REC_TYPE_CALIB:  part = JOURNAL_PART_CALIB;  break;
	case JOURNAL_REC_TYPE_CONFIG: part = JOURNAL_PART_CONFIG; break;
	default: return false;
	}

	uint32_t off = 0;
	uint32_t write_off = m_journal->partitionWriteOff(part);
	calib_record_t best;
	uint64_t best_seq = 0;
	bool found = false;

	while (off + JOURNAL_REC_HEADER_SIZE <= write_off) {
		journal_record_t jrec;
		if (!m_journal->readPartitionRecord(part, off, &jrec))
			break;

		calib_record_t crec;
		if (calib_rec_deserialize(jrec.payload, jrec.payload_length,
					  &crec) &&
		    crec.kind == kind && crec.ref_id == ref_id) {
			calib_record_t winner;
			uint64_t winner_seq;
			if (calib_eval_select_latest(&crec, jrec.sequence,
						     found ? &best : NULL,
						     found ? best_seq : 0,
						     &winner, &winner_seq)) {
				best = winner;
				best_seq = winner_seq;
				found = true;
			}
		}
		off += JOURNAL_REC_HEADER_SIZE + jrec.payload_length;
	}

	if (!found) return false;

	uint16_t expected = calib_expected_blob_len(kind);
	if (expected > 0 && best.blob_len != expected) return false;

	memcpy(blob_out, best.blob, best.blob_len);
	*len_out = best.blob_len;
	return true;
}

bool CalibManager::storeImu(uint8_t sensor_id,
			    const uint8_t *blob, uint16_t len)
{
	return storeRecord(JOURNAL_REC_TYPE_CALIB, CALIB_KIND_IMU,
			   sensor_id, blob, len);
}

bool CalibManager::storeMag(uint8_t sensor_id,
			    const uint8_t *blob, uint16_t len)
{
	return storeRecord(JOURNAL_REC_TYPE_CALIB, CALIB_KIND_MAG,
			   sensor_id, blob, len);
}

bool CalibManager::storeProfile(uint8_t profile_id,
				const uint8_t *blob, uint16_t len)
{
	return storeRecord(JOURNAL_REC_TYPE_CALIB, CALIB_KIND_PROFILE,
			   profile_id, blob, len);
}

bool CalibManager::loadImu(uint8_t sensor_id,
			   uint8_t *blob_out, uint16_t *len_out)
{
	return loadRecord(JOURNAL_REC_TYPE_CALIB, CALIB_KIND_IMU,
			  sensor_id, blob_out, len_out);
}

bool CalibManager::loadMag(uint8_t sensor_id,
			   uint8_t *blob_out, uint16_t *len_out)
{
	return loadRecord(JOURNAL_REC_TYPE_CALIB, CALIB_KIND_MAG,
			  sensor_id, blob_out, len_out);
}

bool CalibManager::loadProfile(uint8_t profile_id,
			       uint8_t *blob_out, uint16_t *len_out)
{
	return loadRecord(JOURNAL_REC_TYPE_CALIB, CALIB_KIND_PROFILE,
			  profile_id, blob_out, len_out);
}

bool CalibManager::storeRuntimeMode(runtime_mode_t mode)
{
	uint8_t blob[1];
	calib_runtime_pack(blob, (uint8_t)mode);
	return storeRecord(JOURNAL_REC_TYPE_CONFIG, CALIB_KIND_RUNTIME,
			   0, blob, 1);
}

bool CalibManager::loadRuntimeMode(runtime_mode_t *out_mode)
{
	uint8_t blob[1];
	uint16_t len = 0;
	if (!loadRecord(JOURNAL_REC_TYPE_CONFIG, CALIB_KIND_RUNTIME, 0,
			blob, &len)) {
		return false;
	}
	uint8_t mode;
	if (!calib_runtime_unpack(blob, len, &mode)) {
		return false;
	}
	if (!runtime_mode_is_valid((runtime_mode_t)mode)) {
		return false;
	}
	*out_mode = (runtime_mode_t)mode;
	return true;
}

#endif /* BOARD_CLUE */
