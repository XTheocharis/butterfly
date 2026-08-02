/* allow: SIZE_OK — cohesive QSPI journal domain logic: record format,
 * CRC, sequence, boot scan, circular log, GC, queue, filter, cursor,
 * erase FSM. Same indivisible-domain exception as qspi_eval.c (333). */
/*
 * qspi_journal_eval.c - Pure-logic QSPI journal evaluation.
 *
 * No SDK deps. Included from qspi_journal.cpp (firmware) and
 * test_qspi_journal.cpp (host). All decision logic lives here.
 * The firmware wrapper translates between nrfx_qspi and these functions.
 *
 * Requires qspi_crc32() from qspi_eval.c (ISO-HDLC variant).
 */
#include "qspi_journal_eval.h"
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Modular sequence comparison ---- */

bool journal_seq_after(uint64_t a, uint64_t b)
{
	/* (a - b) < (b - a) means a is after b in modular space.
	 * Unsigned subtraction wraps mod 2^64. */
	return (a - b) < (b - a);
}

/* ---- Record init / CRC / serialize / deserialize / validate ---- */

void journal_rec_init(journal_record_t *rec, uint8_t type,
		      uint64_t seq, uint64_t ts,
		      const uint8_t *payload, uint16_t len)
{
	memset(rec, 0, sizeof(*rec));
	rec->magic[0] = JOURNAL_REC_MAGIC_0;
	rec->magic[1] = JOURNAL_REC_MAGIC_1;
	rec->magic[2] = JOURNAL_REC_MAGIC_2;
	rec->magic[3] = JOURNAL_REC_MAGIC_3;
	rec->version = JOURNAL_REC_VERSION;
	rec->header_size = JOURNAL_REC_HEADER_SIZE;
	rec->record_type = type;
	rec->payload_length = len;
	rec->sequence = seq;
	rec->timestamp = ts;
	rec->state = JOURNAL_REC_STATE_VALID;
	rec->erase_generation = 0;
	rec->padding = 0xFF;
	if (payload && len > 0) {
		memcpy(rec->payload, payload, len);
	}
	rec->header_crc32 = journal_rec_header_crc(rec);
	rec->payload_crc32 = journal_rec_payload_crc(rec);
}

uint32_t journal_rec_header_crc(const journal_record_t *rec)
{
	uint8_t buf[JOURNAL_REC_HDR_CRC_SPAN];
	buf[0]  = rec->magic[0];   buf[1]  = rec->magic[1];
	buf[2]  = rec->magic[2];   buf[3]  = rec->magic[3];
	buf[4]  = rec->version;
	buf[5]  = rec->header_size;
	buf[6]  = rec->record_type;
	buf[7]  = (uint8_t)(rec->payload_length & 0xFF);
	buf[8]  = (uint8_t)((rec->payload_length >> 8) & 0xFF);
	for (int i = 0; i < 8; i++)
		buf[9 + i] = (uint8_t)((rec->sequence >> (i * 8)) & 0xFF);
	for (int i = 0; i < 8; i++)
		buf[17 + i] = (uint8_t)((rec->timestamp >> (i * 8)) & 0xFF);
	/* CRC uses fixed COMMITTED state so the CRC is invariant across
	 * the NOR 0xFF->0x01->0x00 state transition. The state byte
	 * itself is verified separately via journal_rec_is_committed(). */
	buf[25] = JOURNAL_REC_STATE_COMMITTED;
	for (int i = 0; i < 4; i++)
		buf[26 + i] = (uint8_t)((rec->erase_generation >> (i * 8)) & 0xFF);
	return qspi_crc32(buf, JOURNAL_REC_HDR_CRC_SPAN);
}

uint32_t journal_rec_payload_crc(const journal_record_t *rec)
{
	if (rec->payload_length == 0) return 0;
	return qspi_crc32(rec->payload, rec->payload_length);
}

void journal_rec_serialize(const journal_record_t *rec, uint8_t *buf)
{
	buf[0]  = rec->magic[0];   buf[1]  = rec->magic[1];
	buf[2]  = rec->magic[2];   buf[3]  = rec->magic[3];
	buf[4]  = rec->version;
	buf[5]  = rec->header_size;
	buf[6]  = rec->record_type;
	buf[7]  = (uint8_t)(rec->payload_length & 0xFF);
	buf[8]  = (uint8_t)((rec->payload_length >> 8) & 0xFF);
	for (int i = 0; i < 8; i++)
		buf[9 + i] = (uint8_t)((rec->sequence >> (i * 8)) & 0xFF);
	for (int i = 0; i < 8; i++)
		buf[17 + i] = (uint8_t)((rec->timestamp >> (i * 8)) & 0xFF);
	buf[25] = rec->state;
	for (int i = 0; i < 4; i++)
		buf[26 + i] = (uint8_t)((rec->erase_generation >> (i * 8)) & 0xFF);
	for (int i = 0; i < 4; i++)
		buf[30 + i] = (uint8_t)((rec->header_crc32 >> (i * 8)) & 0xFF);
	for (int i = 0; i < 4; i++)
		buf[34 + i] = (uint8_t)((rec->payload_crc32 >> (i * 8)) & 0xFF);
	buf[38] = rec->padding;
	if (rec->payload_length > 0) {
		memcpy(&buf[JOURNAL_REC_OFF_PAYLOAD],
		       rec->payload, rec->payload_length);
	}
}

bool journal_rec_deserialize(const uint8_t *buf, size_t buf_len,
			     journal_record_t *rec)
{
	if (buf_len < JOURNAL_REC_HEADER_SIZE) return false;
	rec->magic[0] = buf[0]; rec->magic[1] = buf[1];
	rec->magic[2] = buf[2]; rec->magic[3] = buf[3];
	if (rec->magic[0] != JOURNAL_REC_MAGIC_0 ||
	    rec->magic[1] != JOURNAL_REC_MAGIC_1 ||
	    rec->magic[2] != JOURNAL_REC_MAGIC_2 ||
	    rec->magic[3] != JOURNAL_REC_MAGIC_3)
		return false;
	rec->version = buf[4];
	rec->header_size = buf[5];
	rec->record_type = buf[6];
	rec->payload_length = (uint16_t)buf[7] | ((uint16_t)buf[8] << 8);
	rec->sequence = 0;
	for (int i = 0; i < 8; i++)
		rec->sequence |= ((uint64_t)buf[9 + i]) << (i * 8);
	rec->timestamp = 0;
	for (int i = 0; i < 8; i++)
		rec->timestamp |= ((uint64_t)buf[17 + i]) << (i * 8);
	rec->state = buf[25];
	rec->erase_generation = 0;
	for (int i = 0; i < 4; i++)
		rec->erase_generation |= ((uint32_t)buf[26 + i]) << (i * 8);
	rec->header_crc32 = 0;
	for (int i = 0; i < 4; i++)
		rec->header_crc32 |= ((uint32_t)buf[30 + i]) << (i * 8);
	rec->payload_crc32 = 0;
	for (int i = 0; i < 4; i++)
		rec->payload_crc32 |= ((uint32_t)buf[34 + i]) << (i * 8);
	rec->padding = buf[38];
	if (rec->payload_length > JOURNAL_REC_MAX_PAYLOAD)
		return false;
	if (buf_len < (size_t)(JOURNAL_REC_HEADER_SIZE + rec->payload_length))
		return false;
	if (rec->payload_length > 0)
		memcpy(rec->payload, &buf[JOURNAL_REC_OFF_PAYLOAD],
		       rec->payload_length);
	return true;
}

bool journal_rec_validate(const journal_record_t *rec)
{
	if (rec->magic[0] != JOURNAL_REC_MAGIC_0 ||
	    rec->magic[1] != JOURNAL_REC_MAGIC_1 ||
	    rec->magic[2] != JOURNAL_REC_MAGIC_2 ||
	    rec->magic[3] != JOURNAL_REC_MAGIC_3)
		return false;
	if (rec->version != JOURNAL_REC_VERSION) return false;
	if (rec->header_size != JOURNAL_REC_HEADER_SIZE) return false;
	if (rec->payload_length > JOURNAL_REC_MAX_PAYLOAD) return false;
	if (rec->header_crc32 != journal_rec_header_crc(rec)) return false;
	if (rec->payload_crc32 != journal_rec_payload_crc(rec)) return false;
	return true;
}

bool journal_rec_is_committed(const journal_record_t *rec)
{
	return rec->state == JOURNAL_REC_STATE_COMMITTED;
}

bool journal_rec_is_erased(const uint8_t *buf, size_t len)
{
	if (len < JOURNAL_REC_HEADER_SIZE) return true;
	for (size_t i = 0; i < JOURNAL_REC_HEADER_SIZE; i++) {
		if (buf[i] != 0xFF) return false;
	}
	return true;
}

/* ---- Superblock extension ---- */

void journal_sb_ext_init(journal_sb_info_t *sb)
{
	memset(sb, 0, sizeof(*sb));
	sb->valid = false;
	sb->active_sb_sector = 0;
	sb->log_write_off = 0;
	sb->log_read_off = 0;
	sb->next_sequence = 1;
}

uint32_t journal_sb_ext_compute_crc(const journal_sb_info_t *sb)
{
	uint8_t buf[JOURNAL_SB_EXT_CRC_SPAN];
	buf[0] = 'J'; buf[1] = 'R'; buf[2] = 'N'; buf[3] = 'L';
	buf[4] = JOURNAL_SB_EXT_VERSION;
	buf[5] = sb->active_sb_sector;
	for (int i = 0; i < 4; i++)
		buf[6 + i] = (uint8_t)((sb->erase_generation >> (i * 8)) & 0xFF);
	for (int i = 0; i < 4; i++)
		buf[10 + i] = (uint8_t)((sb->log_write_off >> (i * 8)) & 0xFF);
	for (int i = 0; i < 4; i++)
		buf[14 + i] = (uint8_t)((sb->log_read_off >> (i * 8)) & 0xFF);
	for (int i = 0; i < 8; i++)
		buf[18 + i] = (uint8_t)((sb->next_sequence >> (i * 8)) & 0xFF);
	for (int i = 0; i < 4; i++)
		buf[26 + i] = (uint8_t)((sb->log_erase_gen >> (i * 8)) & 0xFF);
	for (int i = 0; i < 4; i++)
		buf[30 + i] = (uint8_t)((sb->config_write_off >> (i * 8)) & 0xFF);
	for (int i = 0; i < 4; i++)
		buf[34 + i] = (uint8_t)((sb->config_erase_gen >> (i * 8)) & 0xFF);
	for (int i = 0; i < 4; i++)
		buf[38 + i] = (uint8_t)((sb->calib_write_off >> (i * 8)) & 0xFF);
	for (int i = 0; i < 4; i++)
		buf[42 + i] = (uint8_t)((sb->calib_erase_gen >> (i * 8)) & 0xFF);
	for (int i = 0; i < 4; i++)
		buf[46 + i] = (uint8_t)((sb->log_record_count >> (i * 8)) & 0xFF);
	return qspi_crc32(buf, JOURNAL_SB_EXT_CRC_SPAN);
}

void journal_sb_ext_serialize(const journal_sb_info_t *sb, uint8_t *buf)
{
	/* buf must be at least 128 bytes (sector [32..128)) */
	memset(buf, 0xFF, JOURNAL_SB_EXT_SIZE);
	buf[0] = 'J'; buf[1] = 'R'; buf[2] = 'N'; buf[3] = 'L';
	buf[4] = JOURNAL_SB_EXT_VERSION;
	buf[5] = sb->active_sb_sector;
	for (int i = 0; i < 4; i++)
		buf[6 + i] = (uint8_t)((sb->erase_generation >> (i * 8)) & 0xFF);
	for (int i = 0; i < 4; i++)
		buf[10 + i] = (uint8_t)((sb->log_write_off >> (i * 8)) & 0xFF);
	for (int i = 0; i < 4; i++)
		buf[14 + i] = (uint8_t)((sb->log_read_off >> (i * 8)) & 0xFF);
	for (int i = 0; i < 8; i++)
		buf[18 + i] = (uint8_t)((sb->next_sequence >> (i * 8)) & 0xFF);
	for (int i = 0; i < 4; i++)
		buf[26 + i] = (uint8_t)((sb->log_erase_gen >> (i * 8)) & 0xFF);
	for (int i = 0; i < 4; i++)
		buf[30 + i] = (uint8_t)((sb->config_write_off >> (i * 8)) & 0xFF);
	for (int i = 0; i < 4; i++)
		buf[34 + i] = (uint8_t)((sb->config_erase_gen >> (i * 8)) & 0xFF);
	for (int i = 0; i < 4; i++)
		buf[38 + i] = (uint8_t)((sb->calib_write_off >> (i * 8)) & 0xFF);
	for (int i = 0; i < 4; i++)
		buf[42 + i] = (uint8_t)((sb->calib_erase_gen >> (i * 8)) & 0xFF);
	for (int i = 0; i < 4; i++)
		buf[46 + i] = (uint8_t)((sb->log_record_count >> (i * 8)) & 0xFF);
	uint32_t crc = journal_sb_ext_compute_crc(sb);
	for (int i = 0; i < 4; i++)
		buf[50 + i] = (uint8_t)((crc >> (i * 8)) & 0xFF);
	buf[54] = JOURNAL_SB_EXT_COMMIT_VAL;
}

bool journal_sb_ext_deserialize(const uint8_t *buf, journal_sb_info_t *sb)
{
	/* buf points to bytes [32..128) of a superblock sector */
	if (buf[0] != 'J' || buf[1] != 'R' ||
	    buf[2] != 'N' || buf[3] != 'L')
		return false;
	if (buf[4] != JOURNAL_SB_EXT_VERSION) return false;
	if (buf[54] != JOURNAL_SB_EXT_COMMIT_VAL) return false; /* not committed */
	sb->active_sb_sector = buf[5];
	sb->erase_generation = 0;
	for (int i = 0; i < 4; i++)
		sb->erase_generation |= ((uint32_t)buf[6 + i]) << (i * 8);
	sb->log_write_off = 0;
	for (int i = 0; i < 4; i++)
		sb->log_write_off |= ((uint32_t)buf[10 + i]) << (i * 8);
	sb->log_read_off = 0;
	for (int i = 0; i < 4; i++)
		sb->log_read_off |= ((uint32_t)buf[14 + i]) << (i * 8);
	sb->next_sequence = 0;
	for (int i = 0; i < 8; i++)
		sb->next_sequence |= ((uint64_t)buf[18 + i]) << (i * 8);
	sb->log_erase_gen = 0;
	for (int i = 0; i < 4; i++)
		sb->log_erase_gen |= ((uint32_t)buf[26 + i]) << (i * 8);
	sb->config_write_off = 0;
	for (int i = 0; i < 4; i++)
		sb->config_write_off |= ((uint32_t)buf[30 + i]) << (i * 8);
	sb->config_erase_gen = 0;
	for (int i = 0; i < 4; i++)
		sb->config_erase_gen |= ((uint32_t)buf[34 + i]) << (i * 8);
	sb->calib_write_off = 0;
	for (int i = 0; i < 4; i++)
		sb->calib_write_off |= ((uint32_t)buf[38 + i]) << (i * 8);
	sb->calib_erase_gen = 0;
	for (int i = 0; i < 4; i++)
		sb->calib_erase_gen |= ((uint32_t)buf[42 + i]) << (i * 8);
	sb->log_record_count = 0;
	for (int i = 0; i < 4; i++)
		sb->log_record_count |= ((uint32_t)buf[46 + i]) << (i * 8);
	/* Validate CRC */
	uint32_t stored_crc = 0;
	for (int i = 0; i < 4; i++)
		stored_crc |= ((uint32_t)buf[50 + i]) << (i * 8);
	if (stored_crc != journal_sb_ext_compute_crc(sb)) return false;
	sb->valid = true;
	return true;
}

bool journal_sb_ext_validate(const journal_sb_info_t *sb)
{
	return sb->valid && sb->active_sb_sector < JOURNAL_SB_SECTORS;
}

/* ---- Boot scan ---- */

bool journal_scan_superblocks(journal_flash_read_fn read_fn,
			      journal_sb_info_t *out)
{
	journal_sb_info_t sb0, sb1;
	bool ok0 = false, ok1 = false;
	uint8_t ext_buf[JOURNAL_SB_EXT_SIZE];

	/* Read SB sector 0: first check Todo 28 commit at offset 28,
	 * then read journal extension at offset 32. */
	uint8_t sb_header[QSPI_SB_HEADER_SIZE];
	if (read_fn(JOURNAL_SB0_ADDR, sb_header, sizeof(sb_header)) == 0) {
		if (qspi_eval_sb_buf_is_committed(sb_header)) {
			if (read_fn(JOURNAL_SB0_ADDR + JOURNAL_SB_EXT_OFF,
				    ext_buf, JOURNAL_SB_EXT_SIZE) == 0) {
				ok0 = journal_sb_ext_deserialize(ext_buf, &sb0);
			}
		}
	}

	/* Read SB sector 1 */
	if (read_fn(JOURNAL_SB1_ADDR, sb_header, sizeof(sb_header)) == 0) {
		if (qspi_eval_sb_buf_is_committed(sb_header)) {
			if (read_fn(JOURNAL_SB1_ADDR + JOURNAL_SB_EXT_OFF,
				    ext_buf, JOURNAL_SB_EXT_SIZE) == 0) {
				ok1 = journal_sb_ext_deserialize(ext_buf, &sb1);
			}
		}
	}

	if (ok0 && ok1) {
		if (sb0.erase_generation >= sb1.erase_generation)
			*out = sb0;
		else
			*out = sb1;
		return true;
	}
	if (ok0) { *out = sb0; return true; }
	if (ok1) { *out = sb1; return true; }
	return false;
}

/* ---- Circular log ---- */

bool journal_log_is_full(const journal_sb_info_t *sb)
{
	/* Full when write_off wrapped around to meet read_off.
	 * We track this by comparing write_off to LOG_SIZE. */
	if (sb->log_write_off >= JOURNAL_LOG_SIZE) return true;
	return false;
}

bool journal_log_append(journal_flash_backend_t *backend,
			journal_sb_info_t *sb,
			const journal_record_t *rec)
{
	if (journal_log_is_full(sb)) {
		/* Wrap: erase the sector containing read_off and advance */
		uint32_t wrap_sect = sb->log_read_off / QSPI_SECTOR_SIZE;
		if (backend->erase(JOURNAL_LOG_START +
				   wrap_sect * QSPI_SECTOR_SIZE) != 0)
			return false;
		sb->log_read_off += QSPI_SECTOR_SIZE;
		if (sb->log_read_off >= JOURNAL_LOG_SIZE)
			sb->log_read_off = 0;
		sb->log_write_off = 0;
		sb->log_erase_gen++;
	}

	uint32_t addr = JOURNAL_LOG_START + sb->log_write_off;
	uint8_t buf[JOURNAL_REC_MAX_SIZE];
	journal_record_t to_write = *rec;
	to_write.sequence = sb->next_sequence;
	to_write.state = JOURNAL_REC_STATE_VALID;
	to_write.erase_generation = sb->log_erase_gen;
	to_write.header_crc32 = journal_rec_header_crc(&to_write);
	to_write.payload_crc32 = journal_rec_payload_crc(&to_write);
	journal_rec_serialize(&to_write, buf);
	size_t total = JOURNAL_REC_HEADER_SIZE + to_write.payload_length;

	/* Write header + payload (state = VALID = 0x01). */
	if (backend->program(addr, buf, total) != 0)
		return false;

	/* Commit: program state byte 0x00 over 0x01. */
	uint8_t commit = JOURNAL_REC_STATE_COMMITTED;
	if (backend->program(addr + JOURNAL_REC_OFF_STATE, &commit, 1) != 0)
		return false;

	sb->log_write_off += (uint32_t)total;
	sb->next_sequence++;
	sb->log_record_count++;
	return true;
}

bool journal_log_read_one(journal_flash_read_fn read_fn,
			  const journal_sb_info_t *sb,
			  uint32_t log_off,
			  journal_record_t *out)
{
	(void)sb;
	uint8_t buf[JOURNAL_REC_MAX_SIZE];
	uint32_t addr = JOURNAL_LOG_START + log_off;

	if (read_fn(addr, buf, JOURNAL_REC_HEADER_SIZE) != 0)
		return false;
	if (buf[0] != JOURNAL_REC_MAGIC_0 ||
	    buf[1] != JOURNAL_REC_MAGIC_1 ||
	    buf[2] != JOURNAL_REC_MAGIC_2 ||
	    buf[3] != JOURNAL_REC_MAGIC_3)
		return false;

	uint16_t paylen = (uint16_t)buf[7] | ((uint16_t)buf[8] << 8);
	if (paylen > JOURNAL_REC_MAX_PAYLOAD)
		return false;
	if (paylen > 0) {
		if (read_fn(addr + JOURNAL_REC_OFF_PAYLOAD,
			    buf + JOURNAL_REC_OFF_PAYLOAD, paylen) != 0)
			return false;
	}

	if (!journal_rec_deserialize(buf,
				     JOURNAL_REC_HEADER_SIZE + paylen, out))
		return false;
	if (!journal_rec_validate(out)) return false;
	if (!journal_rec_is_committed(out)) return false;
	return true;
}

/* ---- Partition append / read (config, calib) ---- */

bool journal_partition_append(journal_flash_backend_t *backend,
			      journal_sb_info_t *sb,
			      journal_part_t part,
			      const journal_record_t *rec)
{
	uint32_t base, size;
	uint32_t *write_off;
	uint32_t erase_gen;

	switch (part) {
	case JOURNAL_PART_CONFIG:
		base = JOURNAL_CONFIG_START;
		size = JOURNAL_CONFIG_SIZE;
		write_off = &sb->config_write_off;
		erase_gen = sb->config_erase_gen;
		break;
	case JOURNAL_PART_CALIB:
		base = JOURNAL_CALIB_START;
		size = JOURNAL_CALIB_SIZE;
		write_off = &sb->calib_write_off;
		erase_gen = sb->calib_erase_gen;
		break;
	default:
		return false;
	}

	uint32_t total = JOURNAL_REC_HEADER_SIZE + rec->payload_length;
	if (*write_off + total > size) return false;

	journal_record_t to_write = *rec;
	to_write.sequence = sb->next_sequence;
	to_write.state = JOURNAL_REC_STATE_VALID;
	to_write.erase_generation = erase_gen;
	to_write.header_crc32 = journal_rec_header_crc(&to_write);
	to_write.payload_crc32 = journal_rec_payload_crc(&to_write);

	uint8_t buf[JOURNAL_REC_MAX_SIZE];
	journal_rec_serialize(&to_write, buf);

	uint32_t addr = base + *write_off;
	if (backend->program(addr, buf, total) != 0)
		return false;

	uint8_t commit = JOURNAL_REC_STATE_COMMITTED;
	if (backend->program(addr + JOURNAL_REC_OFF_STATE, &commit, 1) != 0)
		return false;

	*write_off += total;
	sb->next_sequence++;
	return true;
}

bool journal_partition_read_one(journal_flash_read_fn read_fn,
				const journal_sb_info_t *sb,
				journal_part_t part,
				uint32_t off,
				journal_record_t *out)
{
	(void)sb;
	uint32_t base;
	switch (part) {
	case JOURNAL_PART_CONFIG: base = JOURNAL_CONFIG_START; break;
	case JOURNAL_PART_CALIB:  base = JOURNAL_CALIB_START;  break;
	case JOURNAL_PART_LOG:    base = JOURNAL_LOG_START;    break;
	default: return false;
	}

	uint8_t buf[JOURNAL_REC_MAX_SIZE];
	if (read_fn(base + off, buf, JOURNAL_REC_HEADER_SIZE) != 0)
		return false;
	if (buf[0] != JOURNAL_REC_MAGIC_0 ||
	    buf[1] != JOURNAL_REC_MAGIC_1 ||
	    buf[2] != JOURNAL_REC_MAGIC_2 ||
	    buf[3] != JOURNAL_REC_MAGIC_3)
		return false;

	uint16_t paylen = (uint16_t)buf[7] | ((uint16_t)buf[8] << 8);
	if (paylen > JOURNAL_REC_MAX_PAYLOAD) return false;
	if (paylen > 0) {
		if (read_fn(base + off + JOURNAL_REC_OFF_PAYLOAD,
			    buf + JOURNAL_REC_OFF_PAYLOAD, paylen) != 0)
			return false;
	}

	if (!journal_rec_deserialize(buf,
				     JOURNAL_REC_HEADER_SIZE + paylen, out))
		return false;
	if (!journal_rec_validate(out)) return false;
	if (!journal_rec_is_committed(out)) return false;
	return true;
}

/* ---- Superblock commit (alternate-sector atomic publish) ---- */

bool journal_commit_superblock(journal_flash_backend_t *backend,
			       journal_sb_info_t *sb)
{
	uint32_t active_addr = (sb->active_sb_sector == 0)
		? JOURNAL_SB0_ADDR : JOURNAL_SB1_ADDR;

	uint8_t sb_header[QSPI_SB_HEADER_SIZE];
	if (backend->read(active_addr, sb_header, sizeof(sb_header)) != 0)
		return false;

	uint8_t new_sector = (sb->active_sb_sector == 0) ? 1 : 0;
	uint32_t new_addr = (new_sector == 0)
		? JOURNAL_SB0_ADDR : JOURNAL_SB1_ADDR;

	if (backend->erase(new_addr) != 0) return false;

	if (backend->program(new_addr, sb_header, sizeof(sb_header)) != 0)
		return false;

	sb->active_sb_sector = new_sector;
	sb->erase_generation++;

	uint8_t ext_buf[JOURNAL_SB_EXT_SIZE];
	journal_sb_ext_serialize(sb, ext_buf);
	if (backend->program(new_addr + JOURNAL_SB_EXT_OFF,
			     ext_buf, sizeof(ext_buf)) != 0)
		return false;

	return true;
}

/* ---- GC ---- */

journal_gc_result_t journal_gc_check(uint32_t write_off, uint32_t part_size)
{
	/* GC needed when a sector is nearly full (>75% used). */
	if (write_off > (part_size * 3) / 4)
		return JOURNAL_GC_NEEDED;
	return JOURNAL_GC_NOT_NEEDED;
}

bool journal_gc_should_replace(const journal_record_t *old_rec,
			       const journal_record_t *new_rec)
{
	if (old_rec->record_type != new_rec->record_type) return false;
	return journal_seq_after(new_rec->sequence, old_rec->sequence);
}

/* ---- Log queue ---- */

void journal_queue_init(journal_log_queue_t *q)
{
	memset(q, 0, sizeof(*q));
	q->newest_index = JOURNAL_QUEUE_NEWEST_EMPTY;
}

journal_queue_result_t journal_queue_push(journal_log_queue_t *q,
					  uint8_t type,
					  const uint8_t *data,
					  uint16_t len)
{
	if (len > JOURNAL_REC_MAX_PAYLOAD) return JOURNAL_QUEUE_FULL;

	uint32_t slot;
	bool found = false;
	for (uint32_t i = 0; i < JOURNAL_LOG_QUEUE_SIZE; i++) {
		if (!q->entries[i].occupied) {
			slot = i;
			found = true;
			break;
		}
	}

	if (!found) {
		/* Queue full: drop-newest replaces the entry at newest_index. */
		q->overrun_count++;
		slot = q->newest_index;
		q->entries[slot].type = type;
		q->entries[slot].len = len;
		if (data && len > 0)
			memcpy(q->entries[slot].data, data, len);
		return JOURNAL_QUEUE_DROPPED;
	}

	q->count++;
	q->entries[slot].occupied = true;
	q->entries[slot].type = type;
	q->entries[slot].len = len;
	if (data && len > 0)
		memcpy(q->entries[slot].data, data, len);
	q->newest_index = (uint8_t)slot;
	return JOURNAL_QUEUE_OK;
}

bool journal_queue_pop(journal_log_queue_t *q,
		       uint8_t *type_out,
		       uint8_t *data_out,
		       uint16_t *len_out)
{
	for (uint32_t i = 0; i < JOURNAL_LOG_QUEUE_SIZE; i++) {
		if (!q->entries[i].occupied)
			continue;

		*type_out = q->entries[i].type;
		*len_out = q->entries[i].len;
		if (data_out && q->entries[i].len > 0)
			memcpy(data_out, q->entries[i].data,
			       q->entries[i].len);
		q->entries[i].occupied = false;
		q->count--;

		if ((uint8_t)i == q->newest_index) {
			q->newest_index = JOURNAL_QUEUE_NEWEST_EMPTY;
			for (int j = JOURNAL_LOG_QUEUE_SIZE - 1; j >= 0; j--) {
				if (q->entries[j].occupied) {
					q->newest_index = (uint8_t)j;
					break;
				}
			}
		}
		return true;
	}
	return false;
}

/* ---- Content filter ---- */

bool journal_filter_allows(journal_content_cat_t cat)
{
	/* Categorically exclude raw audio, secrets, and nonces. */
	switch (cat) {
	case JOURNAL_CAT_AUDIO_RAW:
	case JOURNAL_CAT_SECRET:
	case JOURNAL_CAT_NONCE:
		return false;
	case JOURNAL_CAT_BOARD_EVENT:
	case JOURNAL_CAT_PKT_META:
	case JOURNAL_CAT_PKT_PAYLOAD:
		return true;
	default:
		return false;
	}
}

/* ---- StorageReadLog chunking ---- */

uint16_t journal_readlog_chunk(const uint8_t *data, uint16_t data_len,
			       uint16_t offset, uint16_t max_chunk,
			       uint8_t *out, bool *eof)
{
	if (offset >= data_len) {
		*eof = true;
		return 0;
	}
	uint16_t remaining = data_len - offset;
	uint16_t chunk = (remaining < max_chunk) ? remaining : max_chunk;
	memcpy(out, data + offset, chunk);
	*eof = (offset + chunk >= data_len);
	return chunk;
}

/* ---- StorageEraseLog FSM ---- */

journal_erase_state_t journal_eraselog_transition(
	journal_erase_state_t current, journal_erase_event_t evt)
{
	switch (current) {
	case JOURNAL_ERASE_IDLE:
		if (evt == JOURNAL_ERASE_EVT_CONFIRM)
			return JOURNAL_ERASE_ACCEPTED;
		return JOURNAL_ERASE_IDLE;
	case JOURNAL_ERASE_ACCEPTED:
		if (evt == JOURNAL_ERASE_EVT_PROGRESS)
			return JOURNAL_ERASE_ERASING;
		if (evt == JOURNAL_ERASE_EVT_CANCEL)
			return JOURNAL_ERASE_IDLE;
		return JOURNAL_ERASE_ACCEPTED;
	case JOURNAL_ERASE_ERASING:
		if (evt == JOURNAL_ERASE_EVT_COMPLETE)
			return JOURNAL_ERASE_DONE;
		if (evt == JOURNAL_ERASE_EVT_FAIL)
			return JOURNAL_ERASE_ERROR;
		return JOURNAL_ERASE_ERASING;
	case JOURNAL_ERASE_DONE:
		return JOURNAL_ERASE_DONE;
	case JOURNAL_ERASE_ERROR:
		return JOURNAL_ERASE_ERROR;
	default:
		return JOURNAL_ERASE_IDLE;
	}
}

/* ---- Pre-erase idle check ---- */

bool journal_eval_is_idle(uint32_t busy_flags)
{
	/* All timing-critical flags must be clear:
	 * bit 0 = radio active, bit 1 = BLE connected,
	 * bit 2 = sensor sampling, bit 3 = display update. */
	return (busy_flags & 0x0Fu) == 0;
}

#ifdef __cplusplus
}
#endif
