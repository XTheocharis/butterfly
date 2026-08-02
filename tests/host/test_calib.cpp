/*
 * test_calib.cpp - Calibration persistence host tests.
 *
 * Covers: record round-trip, validation (bad version, wrong sensor, NaN/
 * out-of-range values, bad CRC via journal), fault injection (corrupted
 * new record → previous record selected), runtime precedence, non-adopted
 * rejection, profile persistence, runtime mode pack/unpack.
 *
 * Uses mock_flash.c for NOR-semantics fault injection at the journal level.
 */
#include "test_framework.h"
#include "calib_eval.h"
#include "qspi_journal_eval.h"
#include "mocks/mock_flash.h"
#include <string.h>

/* Include eval sources directly. */
extern "C" {
#include "../../src/storage/qspi_eval.c"
#include "../../src/storage/qspi_journal_eval.c"
#include "../../src/storage/calib_eval.c"
}

/* ---- Helpers -------------------------------------------------------- */

/* Build a valid IMU calib blob (40 bytes). */
static void make_imu_blob(uint8_t out[40])
{
	memset(out, 0, 40);
	out[0] = 1;   /* IMU_CALIB_VERSION */
	out[1] = 1;   /* LSM6DS33 */
	/* gyro_bias[3] at [4..16): zeros = valid */
	/* accel_bias[3] at [16..28): zeros = valid */
	out[28] = 1;  /* stationary */
	/* crc32 at [32..36) — left as zero for validation test */
}

/* Build a valid MAG calib blob (60 bytes). */
static void make_mag_blob(uint8_t out[60])
{
	memset(out, 0, 60);
	out[0] = 1;   /* MAG_CALIB_VERSION */
	out[1] = 2;   /* DONE */
	/* hard_iron[3] at [4..16): zeros = valid */
	/* soft_iron_q16[3][3] at [16..52): only diagonal is non-zero,
	 * set diagonal to 65536 (1.0x); off-diagonal stays 0. */
	for (int i = 0; i < 3; i++) {
		uint16_t off = 16 + (uint16_t)(i * 3 + i) * 4;
		out[off]     = 0x00;
		out[off + 1] = 0x00;
		out[off + 2] = 0x01;
		out[off + 3] = 0x00; /* 65536 LE */
	}
}

static void make_profile_blob(uint8_t *out, uint16_t len)
{
	memset(out, 0, len);
	out[0] = 0;   /* PROF_ANDROID_TV */
	/* Rest is opaque — just needs non-zero length. */
	for (uint16_t i = 1; i < len; i++) {
		out[i] = (uint8_t)(i & 0x7F);
	}
}

/* ================================================================
 * Record init / serialize / deserialize roundtrip
 * ================================================================ */

static void test_record_init_imu(void)
{
	uint8_t blob[40];
	make_imu_blob(blob);
	calib_record_t rec;
	calib_rec_init(&rec, CALIB_KIND_IMU, 1, blob, 40);
	TEST_ASSERT(rec.magic == CALIB_MAGIC, "magic");
	TEST_ASSERT(rec.version == CALIB_VERSION, "version");
	TEST_ASSERT(rec.kind == CALIB_KIND_IMU, "kind");
	TEST_ASSERT(rec.ref_id == 1, "ref_id");
	TEST_ASSERT(rec.blob_len == 40, "blob_len");
	TEST_ASSERT(memcmp(rec.blob, blob, 40) == 0, "blob copy");
}

static void test_record_init_mag(void)
{
	uint8_t blob[60];
	make_mag_blob(blob);
	calib_record_t rec;
	calib_rec_init(&rec, CALIB_KIND_MAG, 3, blob, 60);
	TEST_ASSERT(rec.kind == CALIB_KIND_MAG, "kind");
	TEST_ASSERT(rec.ref_id == 3, "ref_id=3 (mag sensor)");
}

static void test_record_init_profile(void)
{
	uint8_t blob[50];
	make_profile_blob(blob, 50);
	calib_record_t rec;
	calib_rec_init(&rec, CALIB_KIND_PROFILE, 0, blob, 50);
	TEST_ASSERT(rec.kind == CALIB_KIND_PROFILE, "kind");
	TEST_ASSERT(rec.blob_len == 50, "blob_len");
}

static void test_record_init_runtime(void)
{
	uint8_t blob[1] = {1}; /* BLE_HID */
	calib_record_t rec;
	calib_rec_init(&rec, CALIB_KIND_RUNTIME, 0, blob, 1);
	TEST_ASSERT(rec.kind == CALIB_KIND_RUNTIME, "kind");
	TEST_ASSERT(rec.blob[0] == 1, "mode=BLE_HID");
}

static void test_serialize_deserialize_roundtrip(void)
{
	uint8_t blob[40];
	make_imu_blob(blob);
	calib_record_t rec;
	calib_rec_init(&rec, CALIB_KIND_IMU, 1, blob, 40);

	uint8_t buf[CALIB_PAYLOAD_MAX];
	uint16_t written = calib_rec_serialize(&rec, buf, sizeof(buf));
	TEST_ASSERT(written == CALIB_HDR_SIZE + 40, "serialized size");

	calib_record_t rec2;
	TEST_ASSERT(calib_rec_deserialize(buf, written, &rec2), "deserialize");
	TEST_ASSERT(rec2.magic == rec.magic, "magic match");
	TEST_ASSERT(rec2.version == rec.version, "version match");
	TEST_ASSERT(rec2.kind == rec.kind, "kind match");
	TEST_ASSERT(rec2.ref_id == rec.ref_id, "ref_id match");
	TEST_ASSERT(rec2.blob_len == rec.blob_len, "blob_len match");
	TEST_ASSERT(memcmp(rec2.blob, rec.blob, 40) == 0, "blob match");
}

static void test_serialize_truncated_buffer(void)
{
	uint8_t blob[40];
	make_imu_blob(blob);
	calib_record_t rec;
	calib_rec_init(&rec, CALIB_KIND_IMU, 1, blob, 40);

	uint8_t buf[10]; /* too small */
	uint16_t written = calib_rec_serialize(&rec, buf, sizeof(buf));
	TEST_ASSERT(written == 0, "serialize fails on small buffer");
}

/* ================================================================
 * Validation
 * ================================================================ */

static void test_validate_valid_imu(void)
{
	uint8_t blob[40];
	make_imu_blob(blob);
	calib_record_t rec;
	calib_rec_init(&rec, CALIB_KIND_IMU, 1, blob, 40);
	TEST_ASSERT(calib_rec_validate(&rec) == CALIB_OK, "valid IMU");
}

static void test_validate_valid_mag(void)
{
	uint8_t blob[60];
	make_mag_blob(blob);
	calib_record_t rec;
	calib_rec_init(&rec, CALIB_KIND_MAG, 3, blob, 60);
	TEST_ASSERT(calib_rec_validate(&rec) == CALIB_OK, "valid MAG");
}

static void test_validate_valid_profile(void)
{
	uint8_t blob[50];
	make_profile_blob(blob, 50);
	calib_record_t rec;
	calib_rec_init(&rec, CALIB_KIND_PROFILE, 0, blob, 50);
	TEST_ASSERT(calib_rec_validate(&rec) == CALIB_OK, "valid profile");
}

static void test_validate_valid_runtime_raw(void)
{
	uint8_t blob[1] = {0}; /* RAW_WHAD */
	calib_record_t rec;
	calib_rec_init(&rec, CALIB_KIND_RUNTIME, 0, blob, 1);
	TEST_ASSERT(calib_rec_validate(&rec) == CALIB_OK, "valid runtime RAW");
}

static void test_validate_valid_runtime_ble(void)
{
	uint8_t blob[1] = {1}; /* BLE_HID */
	calib_record_t rec;
	calib_rec_init(&rec, CALIB_KIND_RUNTIME, 0, blob, 1);
	TEST_ASSERT(calib_rec_validate(&rec) == CALIB_OK, "valid runtime BLE");
}

static void test_validate_bad_magic(void)
{
	uint8_t blob[40];
	make_imu_blob(blob);
	calib_record_t rec;
	calib_rec_init(&rec, CALIB_KIND_IMU, 1, blob, 40);
	rec.magic = 0x00;
	TEST_ASSERT(calib_rec_validate(&rec) == CALIB_ERR_BAD_MAGIC, "bad magic");
}

static void test_validate_bad_version(void)
{
	uint8_t blob[40];
	make_imu_blob(blob);
	calib_record_t rec;
	calib_rec_init(&rec, CALIB_KIND_IMU, 1, blob, 40);
	rec.version = 99;
	TEST_ASSERT(calib_rec_validate(&rec) == CALIB_ERR_BAD_VERSION, "bad version");
}

static void test_validate_bad_kind(void)
{
	uint8_t blob[40];
	make_imu_blob(blob);
	calib_record_t rec;
	calib_rec_init(&rec, 99, 1, blob, 40);
	TEST_ASSERT(calib_rec_validate(&rec) == CALIB_ERR_BAD_KIND, "bad kind");
}

static void test_validate_imu_wrong_len(void)
{
	uint8_t blob[40];
	make_imu_blob(blob);
	calib_record_t rec;
	calib_rec_init(&rec, CALIB_KIND_IMU, 1, blob, 20); /* wrong len */
	TEST_ASSERT(calib_rec_validate(&rec) == CALIB_ERR_BAD_LEN, "wrong IMU len");
}

static void test_validate_imu_bad_variant(void)
{
	uint8_t blob[40];
	make_imu_blob(blob);
	blob[1] = 5; /* invalid variant */
	calib_record_t rec;
	calib_rec_init(&rec, CALIB_KIND_IMU, 1, blob, 40);
	TEST_ASSERT(calib_rec_validate(&rec) == CALIB_ERR_BAD_VALUE, "bad variant");
}

static void test_validate_imu_bias_out_of_range(void)
{
	uint8_t blob[40];
	make_imu_blob(blob);
	/* Set gyro_bias[0] to 300000 (exceeds 200000 limit). */
	blob[4] = 0xE0; blob[5] = 0x93; blob[6] = 0x04; blob[7] = 0x00;
	calib_record_t rec;
	calib_rec_init(&rec, CALIB_KIND_IMU, 1, blob, 40);
	TEST_ASSERT(calib_rec_validate(&rec) == CALIB_ERR_BAD_VALUE, "bias overflow");
}

static void test_validate_mag_soft_iron_too_small(void)
{
	uint8_t blob[60];
	make_mag_blob(blob);
	/* Set diagonal[0][0] to 100 (< 16384 minimum). blob[16] is matrix[0][0]. */
	blob[16] = 100; blob[17] = 0; blob[18] = 0; blob[19] = 0;
	calib_record_t rec;
	calib_rec_init(&rec, CALIB_KIND_MAG, 3, blob, 60);
	TEST_ASSERT(calib_rec_validate(&rec) == CALIB_ERR_BAD_VALUE, "soft_iron too small");
}

static void test_validate_mag_soft_iron_too_large(void)
{
	uint8_t blob[60];
	make_mag_blob(blob);
	/* Set diagonal[0][0] to 300000 (> 262144 maximum). */
	blob[16] = 0xE0; blob[17] = 0x93; blob[18] = 0x04; blob[19] = 0x00;
	calib_record_t rec;
	calib_rec_init(&rec, CALIB_KIND_MAG, 3, blob, 60);
	TEST_ASSERT(calib_rec_validate(&rec) == CALIB_ERR_BAD_VALUE, "soft_iron too large");
}

static void test_validate_runtime_invalid_mode(void)
{
	uint8_t blob[1] = {5}; /* invalid mode */
	calib_record_t rec;
	calib_rec_init(&rec, CALIB_KIND_RUNTIME, 0, blob, 1);
	TEST_ASSERT(calib_rec_validate(&rec) == CALIB_ERR_BAD_VALUE, "invalid runtime mode");
}

static void test_validate_profile_empty(void)
{
	calib_record_t rec;
	calib_rec_init(&rec, CALIB_KIND_PROFILE, 0, NULL, 0);
	TEST_ASSERT(calib_rec_validate(&rec) == CALIB_ERR_BAD_LEN, "empty profile");
}

static void test_validate_imu_wrong_version_byte(void)
{
	uint8_t blob[40];
	make_imu_blob(blob);
	blob[0] = 2; /* wrong IMU_CALIB_VERSION */
	calib_record_t rec;
	calib_rec_init(&rec, CALIB_KIND_IMU, 1, blob, 40);
	TEST_ASSERT(calib_rec_validate(&rec) == CALIB_ERR_BAD_VERSION, "wrong blob version");
}

/* ================================================================
 * Record selection (fault recovery)
 * ================================================================ */

static void test_select_latest_newer_wins(void)
{
	uint8_t blob[40];
	make_imu_blob(blob);
	calib_record_t old_rec, new_rec, out_rec;
	calib_rec_init(&old_rec, CALIB_KIND_IMU, 1, blob, 40);
	calib_rec_init(&new_rec, CALIB_KIND_IMU, 1, blob, 40);
	uint64_t out_seq;
	TEST_ASSERT(calib_eval_select_latest(&new_rec, 10, &old_rec, 5,
					      &out_rec, &out_seq), "select succeeded");
	TEST_ASSERT(out_seq == 10, "newer seq selected");
}

static void test_select_latest_older_loses(void)
{
	uint8_t blob[40];
	make_imu_blob(blob);
	calib_record_t old_rec, new_rec, out_rec;
	calib_rec_init(&old_rec, CALIB_KIND_IMU, 1, blob, 40);
	calib_rec_init(&new_rec, CALIB_KIND_IMU, 1, blob, 40);
	uint64_t out_seq;
	TEST_ASSERT(calib_eval_select_latest(&new_rec, 3, &old_rec, 7,
					      &out_rec, &out_seq), "select succeeded");
	TEST_ASSERT(out_seq == 7, "older rec has higher seq");
}

static void test_select_latest_corrupt_new_falls_back(void)
{
	uint8_t blob[40];
	make_imu_blob(blob);
	calib_record_t old_rec, new_rec, out_rec;
	calib_rec_init(&old_rec, CALIB_KIND_IMU, 1, blob, 40);
	calib_rec_init(&new_rec, CALIB_KIND_IMU, 1, blob, 40);
	new_rec.magic = 0x00; /* corrupt the new record */
	uint64_t out_seq;
	TEST_ASSERT(calib_eval_select_latest(&new_rec, 10, &old_rec, 5,
					      &out_rec, &out_seq), "fallback succeeded");
	TEST_ASSERT(out_seq == 5, "fell back to old record");
	TEST_ASSERT(out_rec.magic == CALIB_MAGIC, "old record is valid");
}

static void test_select_latest_both_corrupt_fails(void)
{
	uint8_t blob[40];
	make_imu_blob(blob);
	calib_record_t old_rec, new_rec, out_rec;
	calib_rec_init(&old_rec, CALIB_KIND_IMU, 1, blob, 40);
	calib_rec_init(&new_rec, CALIB_KIND_IMU, 1, blob, 40);
	old_rec.magic = 0x00;
	new_rec.magic = 0x00;
	TEST_ASSERT(!calib_eval_select_latest(&new_rec, 10, &old_rec, 5,
					       &out_rec, NULL), "both corrupt → fail");
}

static void test_select_latest_new_only(void)
{
	uint8_t blob[40];
	make_imu_blob(blob);
	calib_record_t new_rec, out_rec;
	calib_rec_init(&new_rec, CALIB_KIND_IMU, 1, blob, 40);
	TEST_ASSERT(calib_eval_select_latest(&new_rec, 1, NULL, 0,
					      &out_rec, NULL), "new only");
	TEST_ASSERT(out_rec.kind == CALIB_KIND_IMU, "got new record");
}

static void test_select_latest_old_only(void)
{
	uint8_t blob[40];
	make_imu_blob(blob);
	calib_record_t old_rec, out_rec;
	calib_rec_init(&old_rec, CALIB_KIND_IMU, 1, blob, 40);
	TEST_ASSERT(calib_eval_select_latest(NULL, 0, &old_rec, 5,
					      &out_rec, NULL), "old only");
	TEST_ASSERT(out_rec.kind == CALIB_KIND_IMU, "got old record");
}

/* ================================================================
 * Runtime mode pack/unpack
 * ================================================================ */

static void test_runtime_pack_raw(void)
{
	uint8_t blob[1];
	calib_runtime_pack(blob, 0);
	TEST_ASSERT(blob[0] == 0, "raw mode packed");
}

static void test_runtime_pack_ble(void)
{
	uint8_t blob[1];
	calib_runtime_pack(blob, 1);
	TEST_ASSERT(blob[0] == 1, "BLE mode packed");
}

static void test_runtime_unpack_valid_raw(void)
{
	uint8_t blob[1] = {0};
	uint8_t mode;
	TEST_ASSERT(calib_runtime_unpack(blob, 1, &mode), "unpack raw");
	TEST_ASSERT(mode == 0, "mode is raw");
}

static void test_runtime_unpack_valid_ble(void)
{
	uint8_t blob[1] = {1};
	uint8_t mode;
	TEST_ASSERT(calib_runtime_unpack(blob, 1, &mode), "unpack BLE");
	TEST_ASSERT(mode == 1, "mode is BLE");
}

static void test_runtime_unpack_invalid_mode(void)
{
	uint8_t blob[1] = {5};
	uint8_t mode;
	TEST_ASSERT(!calib_runtime_unpack(blob, 1, &mode), "invalid mode rejected");
}

static void test_runtime_unpack_too_short(void)
{
	uint8_t mode;
	TEST_ASSERT(!calib_runtime_unpack(NULL, 0, &mode), "null blob rejected");
}

/* ================================================================
 * Adopted flag
 * ================================================================ */

static void test_adopted_flag_default_false(void)
{
	calib_eval_set_adopted(false);
	TEST_ASSERT(!calib_eval_is_adopted(), "default not adopted");
}

static void test_adopted_flag_set_true(void)
{
	calib_eval_set_adopted(true);
	TEST_ASSERT(calib_eval_is_adopted(), "adopted flag set");
	calib_eval_set_adopted(false); /* reset */
}

/* ================================================================
 * Expected blob length
 * ================================================================ */

static void test_expected_blob_len_imu(void)
{
	TEST_ASSERT(calib_expected_blob_len(CALIB_KIND_IMU) == 40, "IMU blob len");
}

static void test_expected_blob_len_mag(void)
{
	TEST_ASSERT(calib_expected_blob_len(CALIB_KIND_MAG) == 60, "MAG blob len");
}

static void test_expected_blob_len_runtime(void)
{
	TEST_ASSERT(calib_expected_blob_len(CALIB_KIND_RUNTIME) == 1, "runtime blob len");
}

static void test_expected_blob_len_profile_variable(void)
{
	TEST_ASSERT(calib_expected_blob_len(CALIB_KIND_PROFILE) == 0, "profile variable len");
}

/* ================================================================
 * Journal-level integration: calib record in journal payload
 * ================================================================ */

static void test_calib_in_journal_roundtrip(void)
{
	/* Build a calib record, serialize it, put it in a journal record,
	 * serialize the journal, deserialize, and verify. */
	uint8_t blob[40];
	make_imu_blob(blob);
	calib_record_t crec;
	calib_rec_init(&crec, CALIB_KIND_IMU, 1, blob, 40);

	uint8_t payload[CALIB_PAYLOAD_MAX];
	uint16_t plen = calib_rec_serialize(&crec, payload, sizeof(payload));
	TEST_ASSERT(plen > 0, "serialize calib");

	journal_record_t jrec;
	journal_rec_init(&jrec, JOURNAL_REC_TYPE_CALIB, 1, 1000, payload, plen);
	TEST_ASSERT(journal_rec_validate(&jrec), "journal validates");

	/* Deserialize journal record payload back to calib record. */
	calib_record_t crec2;
	TEST_ASSERT(calib_rec_deserialize(jrec.payload, jrec.payload_length, &crec2),
		    "deserialize from journal payload");
	TEST_ASSERT(calib_rec_validate(&crec2) == CALIB_OK, "calib validates from journal");
	TEST_ASSERT(crec2.kind == CALIB_KIND_IMU, "kind preserved");
	TEST_ASSERT(crec2.ref_id == 1, "ref_id preserved");
}

static void test_runtime_config_in_journal_roundtrip(void)
{
	uint8_t blob[1] = {1}; /* BLE_HID */
	calib_record_t crec;
	calib_rec_init(&crec, CALIB_KIND_RUNTIME, 0, blob, 1);

	uint8_t payload[CALIB_PAYLOAD_MAX];
	uint16_t plen = calib_rec_serialize(&crec, payload, sizeof(payload));

	journal_record_t jrec;
	journal_rec_init(&jrec, JOURNAL_REC_TYPE_CONFIG, 5, 2000, payload, plen);
	TEST_ASSERT(journal_rec_validate(&jrec), "journal validates");

	calib_record_t crec2;
	TEST_ASSERT(calib_rec_deserialize(jrec.payload, jrec.payload_length, &crec2),
		    "deserialize runtime from journal");
	TEST_ASSERT(calib_rec_validate(&crec2) == CALIB_OK, "runtime validates");
	TEST_ASSERT(crec2.kind == CALIB_KIND_RUNTIME, "kind is runtime");
	TEST_ASSERT(crec2.blob[0] == 1, "mode is BLE");
}

/* ================================================================
 * Fault injection: corrupt journal header CRC, recover previous
 * ================================================================ */

static void test_fault_corrupt_header_crc_previous_wins(void)
{
	/* Write two records to the calib partition using mock flash.
	 * Corrupt the second record's header CRC. Recovery should
	 * select the first (previous valid) record. */
	mock_flash_reset();

	/* Build two journal records with calib payloads. */
	uint8_t blob1[40], blob2[40];
	make_imu_blob(blob1);
	make_imu_blob(blob2);

	calib_record_t crec1, crec2;
	calib_rec_init(&crec1, CALIB_KIND_IMU, 1, blob1, 40);
	calib_rec_init(&crec2, CALIB_KIND_IMU, 1, blob2, 40);

	uint8_t p1[CALIB_PAYLOAD_MAX], p2[CALIB_PAYLOAD_MAX];
	uint16_t plen1 = calib_rec_serialize(&crec1, p1, sizeof(p1));
	uint16_t plen2 = calib_rec_serialize(&crec2, p2, sizeof(p2));

	journal_record_t jrec1, jrec2;
	journal_rec_init(&jrec1, JOURNAL_REC_TYPE_CALIB, 1, 1000, p1, plen1);
	journal_rec_init(&jrec2, JOURNAL_REC_TYPE_CALIB, 2, 2000, p2, plen2);

	/* Serialize both to flash at the calib partition start. */
	uint8_t buf[JOURNAL_REC_MAX_SIZE];
	uint16_t s1 = JOURNAL_REC_HEADER_SIZE + jrec1.payload_length;
	uint16_t s2 = JOURNAL_REC_HEADER_SIZE + jrec2.payload_length;

	journal_rec_serialize(&jrec1, buf);
	mock_flash_program(JOURNAL_CALIB_START, buf, s1);

	journal_rec_serialize(&jrec2, buf);
	/* Corrupt the header CRC bytes of rec2 (offset 30 in serialized record). */
	buf[30] ^= 0xFF;
	mock_flash_program(JOURNAL_CALIB_START + s1, buf, s2);

	/* Read back both records directly from flash. */
	uint8_t rbuf[JOURNAL_REC_MAX_SIZE];
	journal_record_t r1, r2;

	mock_flash_read(JOURNAL_CALIB_START, rbuf, s1);
	bool ok_rec1 = journal_rec_deserialize(rbuf, s1, &r1);
	TEST_ASSERT(ok_rec1, "rec1 deserialized");
	TEST_ASSERT(journal_rec_validate(&r1), "rec1 validates");

	mock_flash_read(JOURNAL_CALIB_START + s1, rbuf, s2);
	bool ok_rec2 = journal_rec_deserialize(rbuf, s2, &r2);
	bool rec2_valid = ok_rec2 && journal_rec_validate(&r2);

	/* The corrupted record should fail validation. */
	TEST_ASSERT(!rec2_valid, "corrupt rec2 fails validate");

	/* Select between them: even though rec2 has higher seq, it's corrupt,
	 * so rec1 should win. */
	calib_record_t cr1, cr2, cout;
	TEST_ASSERT(calib_rec_deserialize(r1.payload, r1.payload_length, &cr1),
		    "calib from rec1");

	if (rec2_valid) {
		calib_rec_deserialize(r2.payload, r2.payload_length, &cr2);
		TEST_ASSERT(calib_eval_select_latest(&cr2, 2, &cr1, 1, &cout, NULL),
			    "select succeeded");
	} else {
		TEST_ASSERT(calib_eval_select_latest(NULL, 2, &cr1, 1, &cout, NULL),
			    "select with corrupt new");
	}
	TEST_ASSERT(cout.magic == CALIB_MAGIC, "selected valid record");
}

/* ================================================================
 * Profile persistence roundtrip
 * ================================================================ */

static void test_profile_persistence_roundtrip(void)
{
	uint8_t blob[80];
	make_profile_blob(blob, 80);

	calib_record_t rec;
	calib_rec_init(&rec, CALIB_KIND_PROFILE, 2, blob, 80);

	uint8_t buf[CALIB_PAYLOAD_MAX];
	uint16_t written = calib_rec_serialize(&rec, buf, sizeof(buf));
	TEST_ASSERT(written == CALIB_HDR_SIZE + 80, "profile serialized");

	calib_record_t rec2;
	TEST_ASSERT(calib_rec_deserialize(buf, written, &rec2), "deserialize");
	TEST_ASSERT(calib_rec_validate(&rec2) == CALIB_OK, "profile validates");
	TEST_ASSERT(rec2.kind == CALIB_KIND_PROFILE, "kind match");
	TEST_ASSERT(rec2.ref_id == 2, "profile_id match");
	TEST_ASSERT(rec2.blob_len == 80, "blob_len match");
}

/* ================================================================
 * Full round-trip: append calib → commit SB → reboot → load
 * ================================================================ */

static void write_initial_superblock(void)
{
	mock_flash_erase_sector(0);
	qspi_superblock_t sb28;
	qspi_eval_sb_init(&sb28);
	sb28.crc32 = qspi_eval_sb_compute_crc(&sb28);
	uint8_t sb28_buf[QSPI_SB_HEADER_SIZE];
	qspi_eval_sb_serialize(&sb28, sb28_buf);
	mock_flash_program(0, sb28_buf, QSPI_SB_COMMIT_OFFSET);
	uint8_t commit = QSPI_SB_COMMIT_VALUE;
	mock_flash_program(QSPI_SB_COMMIT_OFFSET, &commit, 1);

	journal_sb_info_t sb;
	journal_sb_ext_init(&sb);
	sb.valid = true;
	sb.active_sb_sector = 0;
	sb.erase_generation = 1;
	sb.next_sequence = 1;
	uint8_t ext_buf[JOURNAL_SB_EXT_SIZE];
	journal_sb_ext_serialize(&sb, ext_buf);
	mock_flash_program(JOURNAL_SB_EXT_OFF, ext_buf, JOURNAL_SB_EXT_SIZE);
}

static int rt_read(uint32_t a, void *b, size_t l) { return mock_flash_read(a, b, l); }
static int rt_program(uint32_t a, const void *b, size_t l) { return mock_flash_program(a, b, l); }
static int rt_erase(uint32_t a) { return mock_flash_erase_sector(a); }

static void test_store_reset_load_calib_roundtrip(void)
{
	mock_flash_reset();
	write_initial_superblock();

	for (uint32_t s = 0; s < JOURNAL_CALIB_SECTORS; s++)
		mock_flash_erase_sector(JOURNAL_CALIB_START +
					s * QSPI_SECTOR_SIZE);

	journal_sb_info_t sb;
	journal_sb_ext_init(&sb);
	sb.valid = true;
	sb.active_sb_sector = 0;
	sb.erase_generation = 1;
	sb.next_sequence = 1;

	uint8_t blob[40];
	make_imu_blob(blob);
	calib_record_t crec;
	calib_rec_init(&crec, CALIB_KIND_IMU, 1, blob, 40);

	uint8_t payload[CALIB_PAYLOAD_MAX];
	uint16_t plen = calib_rec_serialize(&crec, payload, sizeof(payload));
	TEST_ASSERT(plen > 0, "serialize");

	journal_record_t jrec;
	journal_rec_init(&jrec, JOURNAL_REC_TYPE_CALIB, 0, 0, payload, plen);

	journal_flash_backend_t backend = {rt_read, rt_program, rt_erase};
	TEST_ASSERT(journal_partition_append(&backend, &sb,
		JOURNAL_PART_CALIB, &jrec), "append");
	TEST_ASSERT(journal_commit_superblock(&backend, &sb), "commit SB");

	journal_sb_info_t recovered;
	TEST_ASSERT(journal_scan_superblocks(mock_flash_read, &recovered),
		"reboot scan");

	uint32_t off = 0;
	calib_record_t best;
	memset(&best, 0, sizeof(best));
	uint64_t best_seq = 0;
	bool found = false;

	while (off + JOURNAL_REC_HEADER_SIZE <= recovered.calib_write_off) {
		journal_record_t jr;
		if (!journal_partition_read_one(mock_flash_read, &recovered,
		    JOURNAL_PART_CALIB, off, &jr))
			break;
		calib_record_t cr;
		if (calib_rec_deserialize(jr.payload, jr.payload_length, &cr) &&
		    cr.kind == CALIB_KIND_IMU && cr.ref_id == 1) {
			calib_record_t winner;
			uint64_t winner_seq;
			if (calib_eval_select_latest(&cr, jr.sequence,
			    found ? &best : NULL,
			    found ? best_seq : 0,
			    &winner, &winner_seq)) {
				best = winner;
				best_seq = winner_seq;
				found = true;
			}
		}
		off += JOURNAL_REC_HEADER_SIZE + jr.payload_length;
	}

	TEST_ASSERT(found, "found record after reboot");
	TEST_ASSERT_EQ_INT(40, (int)best.blob_len);
	TEST_ASSERT_EQ_INT(0, memcmp(best.blob, blob, 40));
}

static void test_store_reset_load_config_roundtrip(void)
{
	mock_flash_reset();
	write_initial_superblock();

	mock_flash_erase_sector(JOURNAL_CONFIG_START);

	journal_sb_info_t sb;
	journal_sb_ext_init(&sb);
	sb.valid = true;
	sb.active_sb_sector = 0;
	sb.erase_generation = 1;
	sb.next_sequence = 1;

	uint8_t mode_blob[1] = {1};
	calib_record_t crec;
	calib_rec_init(&crec, CALIB_KIND_RUNTIME, 0, mode_blob, 1);

	uint8_t payload[CALIB_PAYLOAD_MAX];
	uint16_t plen = calib_rec_serialize(&crec, payload, sizeof(payload));

	journal_record_t jrec;
	journal_rec_init(&jrec, JOURNAL_REC_TYPE_CONFIG, 0, 0, payload, plen);

	journal_flash_backend_t backend = {rt_read, rt_program, rt_erase};
	TEST_ASSERT(journal_partition_append(&backend, &sb,
		JOURNAL_PART_CONFIG, &jrec), "append config");
	TEST_ASSERT(journal_commit_superblock(&backend, &sb), "commit SB");

	journal_sb_info_t recovered;
	TEST_ASSERT(journal_scan_superblocks(mock_flash_read, &recovered),
		"reboot scan");

	uint32_t off = 0;
	calib_record_t best;
	memset(&best, 0, sizeof(best));
	uint64_t best_seq = 0;
	bool found = false;

	while (off + JOURNAL_REC_HEADER_SIZE <= recovered.config_write_off) {
		journal_record_t jr;
		if (!journal_partition_read_one(mock_flash_read, &recovered,
		    JOURNAL_PART_CONFIG, off, &jr))
			break;
		calib_record_t cr;
		if (calib_rec_deserialize(jr.payload, jr.payload_length, &cr) &&
		    cr.kind == CALIB_KIND_RUNTIME && cr.ref_id == 0) {
			calib_record_t winner;
			uint64_t winner_seq;
			if (calib_eval_select_latest(&cr, jr.sequence,
			    found ? &best : NULL,
			    found ? best_seq : 0,
			    &winner, &winner_seq)) {
				best = winner;
				best_seq = winner_seq;
				found = true;
			}
		}
		off += JOURNAL_REC_HEADER_SIZE + jr.payload_length;
	}

	TEST_ASSERT(found, "found config record after reboot");
	TEST_ASSERT_EQ_INT(1, (int)best.blob_len);
	TEST_ASSERT_EQ_INT(1, best.blob[0]);
}

static void test_store_two_records_latest_wins_after_reboot(void)
{
	mock_flash_reset();
	write_initial_superblock();

	for (uint32_t s = 0; s < JOURNAL_CALIB_SECTORS; s++)
		mock_flash_erase_sector(JOURNAL_CALIB_START +
					s * QSPI_SECTOR_SIZE);

	journal_sb_info_t sb;
	journal_sb_ext_init(&sb);
	sb.valid = true;
	sb.active_sb_sector = 0;
	sb.erase_generation = 1;
	sb.next_sequence = 1;

	journal_flash_backend_t backend = {rt_read, rt_program, rt_erase};

	uint8_t blob_old[40], blob_new[40];
	make_imu_blob(blob_old);
	make_imu_blob(blob_new);
	blob_new[4] = 0x42;

	calib_record_t crec_old, crec_new;
	calib_rec_init(&crec_old, CALIB_KIND_IMU, 1, blob_old, 40);
	calib_rec_init(&crec_new, CALIB_KIND_IMU, 1, blob_new, 40);

	uint8_t p_old[CALIB_PAYLOAD_MAX], p_new[CALIB_PAYLOAD_MAX];
	uint16_t plen_old = calib_rec_serialize(&crec_old, p_old, sizeof(p_old));
	uint16_t plen_new = calib_rec_serialize(&crec_new, p_new, sizeof(p_new));

	journal_record_t jrec_old, jrec_new;
	journal_rec_init(&jrec_old, JOURNAL_REC_TYPE_CALIB, 0, 0, p_old, plen_old);
	journal_rec_init(&jrec_new, JOURNAL_REC_TYPE_CALIB, 0, 0, p_new, plen_new);

	TEST_ASSERT(journal_partition_append(&backend, &sb,
		JOURNAL_PART_CALIB, &jrec_old), "append old");
	TEST_ASSERT(journal_partition_append(&backend, &sb,
		JOURNAL_PART_CALIB, &jrec_new), "append new");
	TEST_ASSERT(journal_commit_superblock(&backend, &sb), "commit SB");

	journal_sb_info_t recovered;
	TEST_ASSERT(journal_scan_superblocks(mock_flash_read, &recovered),
		"reboot scan");

	uint32_t off = 0;
	calib_record_t best;
	memset(&best, 0, sizeof(best));
	uint64_t best_seq = 0;
	bool found = false;

	while (off + JOURNAL_REC_HEADER_SIZE <= recovered.calib_write_off) {
		journal_record_t jr;
		if (!journal_partition_read_one(mock_flash_read, &recovered,
		    JOURNAL_PART_CALIB, off, &jr))
			break;
		calib_record_t cr;
		if (calib_rec_deserialize(jr.payload, jr.payload_length, &cr) &&
		    cr.kind == CALIB_KIND_IMU && cr.ref_id == 1) {
			calib_record_t winner;
			uint64_t winner_seq;
			if (calib_eval_select_latest(&cr, jr.sequence,
			    found ? &best : NULL,
			    found ? best_seq : 0,
			    &winner, &winner_seq)) {
				best = winner;
				best_seq = winner_seq;
				found = true;
			}
		}
		off += JOURNAL_REC_HEADER_SIZE + jr.payload_length;
	}

	TEST_ASSERT(found, "found record after reboot");
	TEST_ASSERT_EQ_INT(2, (int)best_seq);
	TEST_ASSERT_EQ_INT(0x42, best.blob[4]);
}

/* ================================================================
 * Main
 * ================================================================ */

int main(void)
{
	test_framework_init();

	RUN_TEST(test_record_init_imu);
	RUN_TEST(test_record_init_mag);
	RUN_TEST(test_record_init_profile);
	RUN_TEST(test_record_init_runtime);
	RUN_TEST(test_serialize_deserialize_roundtrip);
	RUN_TEST(test_serialize_truncated_buffer);

	RUN_TEST(test_validate_valid_imu);
	RUN_TEST(test_validate_valid_mag);
	RUN_TEST(test_validate_valid_profile);
	RUN_TEST(test_validate_valid_runtime_raw);
	RUN_TEST(test_validate_valid_runtime_ble);
	RUN_TEST(test_validate_bad_magic);
	RUN_TEST(test_validate_bad_version);
	RUN_TEST(test_validate_bad_kind);
	RUN_TEST(test_validate_imu_wrong_len);
	RUN_TEST(test_validate_imu_bad_variant);
	RUN_TEST(test_validate_imu_bias_out_of_range);
	RUN_TEST(test_validate_mag_soft_iron_too_small);
	RUN_TEST(test_validate_mag_soft_iron_too_large);
	RUN_TEST(test_validate_runtime_invalid_mode);
	RUN_TEST(test_validate_profile_empty);
	RUN_TEST(test_validate_imu_wrong_version_byte);

	RUN_TEST(test_select_latest_newer_wins);
	RUN_TEST(test_select_latest_older_loses);
	RUN_TEST(test_select_latest_corrupt_new_falls_back);
	RUN_TEST(test_select_latest_both_corrupt_fails);
	RUN_TEST(test_select_latest_new_only);
	RUN_TEST(test_select_latest_old_only);

	RUN_TEST(test_runtime_pack_raw);
	RUN_TEST(test_runtime_pack_ble);
	RUN_TEST(test_runtime_unpack_valid_raw);
	RUN_TEST(test_runtime_unpack_valid_ble);
	RUN_TEST(test_runtime_unpack_invalid_mode);
	RUN_TEST(test_runtime_unpack_too_short);

	RUN_TEST(test_adopted_flag_default_false);
	RUN_TEST(test_adopted_flag_set_true);

	RUN_TEST(test_expected_blob_len_imu);
	RUN_TEST(test_expected_blob_len_mag);
	RUN_TEST(test_expected_blob_len_runtime);
	RUN_TEST(test_expected_blob_len_profile_variable);

	RUN_TEST(test_calib_in_journal_roundtrip);
	RUN_TEST(test_runtime_config_in_journal_roundtrip);
	RUN_TEST(test_fault_corrupt_header_crc_previous_wins);
	RUN_TEST(test_profile_persistence_roundtrip);

	RUN_TEST(test_store_reset_load_calib_roundtrip);
	RUN_TEST(test_store_reset_load_config_roundtrip);
	RUN_TEST(test_store_two_records_latest_wins_after_reboot);

	return test_framework_finish();
}
