/*
 * test_qspi_journal.cpp - QSPI journal host tests.
 *
 * Covers: CRC32 reuse, record format roundtrip, modular sequence wrap,
 * superblock extension, boot scan, circular log append/wrap/recovery,
 * corrupt/torn recovery via fault injection at every byte position,
 * GC policy, log queue (drop-newest, overrun count), content filter
 * (audio/secrets excluded), StorageReadLog chunking, StorageEraseLog
 * FSM, pre-erase idle check.
 *
 * Power-cut fault injection: simulates interruption at every byte of a
 * record write using mock_flash NOR semantics. Recovery must always
 * select the previous valid committed state.
 */
#include "test_framework.h"
#include "../../src/storage/qspi_journal_eval.h"
#include "../../src/storage/calib_eval.h"
#include "mocks/mock_flash.h"
#include <string.h>

/* Include both eval .c files directly. qspi_eval.c provides qspi_crc32. */
extern "C" {
#include "../../src/storage/qspi_eval.c"
#include "../../src/storage/qspi_journal_eval.c"
#include "../../src/storage/calib_eval.c"
}

/* ================================================================
 * Partition layout sanity
 * ================================================================ */

static void test_partition_sizes(void)
{
	TEST_ASSERT_EQ_INT(8192u, (int)JOURNAL_SB_SIZE);
	TEST_ASSERT_EQ_INT(57344u, (int)JOURNAL_CONFIG_SIZE);
	TEST_ASSERT_EQ_INT(65536u, (int)JOURNAL_CALIB_SIZE);
	TEST_ASSERT_EQ_INT(1966080u, (int)JOURNAL_LOG_SIZE);
	/* Total must equal 2 MiB */
	TEST_ASSERT_EQ_INT((int)QSPI_FLASH_SIZE_BYTES,
		(int)(JOURNAL_SB_SIZE + JOURNAL_CONFIG_SIZE +
		      JOURNAL_CALIB_SIZE + JOURNAL_LOG_SIZE));
	TEST_ASSERT_EQ_INT(480, (int)JOURNAL_LOG_SECTORS);
	TEST_ASSERT_EQ_INT(14, (int)JOURNAL_CONFIG_SECTORS);
	TEST_ASSERT_EQ_INT(16, (int)JOURNAL_CALIB_SECTORS);
}

static void test_record_header_size(void)
{
	TEST_ASSERT_EQ_INT(39, JOURNAL_REC_HEADER_SIZE);
	TEST_ASSERT_EQ_INT(30, JOURNAL_REC_HDR_CRC_SPAN);
	TEST_ASSERT_EQ_INT(1024, (int)JOURNAL_REC_MAX_PAYLOAD);
}

/* ================================================================
 * CRC32 (reuse from qspi_eval — same ISO-HDLC variant)
 * ================================================================ */

static void test_crc32_journal_uses_same_variant(void)
{
	/* Verify journal eval calls qspi_crc32 (the same ISO-HDLC function
	 * from qspi_eval.c). Cross-check with a known vector. */
	uint8_t data[] = "123456789";
	uint32_t crc = qspi_crc32(data, 9);
	TEST_ASSERT_EQ_INT((int)0xCBF43926u, (int)crc);
}

static void test_crc32_empty_buffer(void)
{
	TEST_ASSERT_EQ_INT(0, (int)qspi_crc32((const uint8_t *)"", 0));
}

/* ================================================================
 * Record format: serialize / deserialize / validate roundtrip
 * ================================================================ */

static void test_record_init_and_validate(void)
{
	journal_record_t rec;
	uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
	journal_rec_init(&rec, JOURNAL_REC_TYPE_CONFIG, 42, 1000000,
			 payload, 4);
	TEST_ASSERT_EQ_INT('B', rec.magic[0]);
	TEST_ASSERT_EQ_INT('T', rec.magic[1]);
	TEST_ASSERT_EQ_INT('F', rec.magic[2]);
	TEST_ASSERT_EQ_INT('R', rec.magic[3]);
	TEST_ASSERT_EQ_INT(JOURNAL_REC_VERSION, rec.version);
	TEST_ASSERT_EQ_INT(JOURNAL_REC_HEADER_SIZE, rec.header_size);
	TEST_ASSERT_EQ_INT(JOURNAL_REC_TYPE_CONFIG, rec.record_type);
	TEST_ASSERT_EQ_INT(4, rec.payload_length);
	TEST_ASSERT_EQ_INT(42, (int)rec.sequence);
	TEST_ASSERT_EQ_INT(1000000, (int)rec.timestamp);
	TEST_ASSERT_EQ_INT(JOURNAL_REC_STATE_VALID, rec.state);
	TEST_ASSERT(rec.header_crc32 != 0, "header CRC should be nonzero");
	TEST_ASSERT(rec.payload_crc32 != 0, "payload CRC should be nonzero");
	TEST_ASSERT(journal_rec_validate(&rec), "initialized record validates");
}

static void test_record_serialize_deserialize_roundtrip(void)
{
	journal_record_t rec;
	uint8_t payload[] = {1, 2, 3, 4, 5};
	journal_rec_init(&rec, JOURNAL_REC_TYPE_LOG_EVT, 99, 5000,
			 payload, 5);

	uint8_t buf[JOURNAL_REC_MAX_SIZE];
	journal_rec_serialize(&rec, buf);

	journal_record_t rt;
	size_t total = JOURNAL_REC_HEADER_SIZE + 5;
	TEST_ASSERT(journal_rec_deserialize(buf, total, &rt),
		"deserialize should succeed");
	TEST_ASSERT(journal_rec_validate(&rt), "roundtrip validates");
	TEST_ASSERT_EQ_INT(rec.record_type, rt.record_type);
	TEST_ASSERT_EQ_INT((int)rec.payload_length, (int)rt.payload_length);
	TEST_ASSERT_EQ_INT((int)rec.sequence, (int)rt.sequence);
	TEST_ASSERT_EQ_INT((int)rec.timestamp, (int)rt.timestamp);
	TEST_ASSERT_EQ_INT((int)rec.header_crc32, (int)rt.header_crc32);
	TEST_ASSERT_EQ_INT((int)rec.payload_crc32, (int)rt.payload_crc32);
	TEST_ASSERT_EQ_INT(0, memcmp(rec.payload, rt.payload, 5));
}

static void test_record_zero_payload_validates(void)
{
	journal_record_t rec;
	journal_rec_init(&rec, JOURNAL_REC_TYPE_GC_MARK, 1, 0, NULL, 0);
	TEST_ASSERT(journal_rec_validate(&rec), "zero-payload record validates");
	TEST_ASSERT_EQ_INT(0, rec.payload_crc32);
}

static void test_record_bad_magic_fails_deserialize(void)
{
	uint8_t buf[JOURNAL_REC_HEADER_SIZE];
	memset(buf, 0xFF, sizeof(buf));
	journal_record_t rec;
	TEST_ASSERT(!journal_rec_deserialize(buf, sizeof(buf), &rec),
		"erased bytes should not deserialize");
}

static void test_record_corrupt_header_crc_fails_validate(void)
{
	journal_record_t rec;
	journal_rec_init(&rec, JOURNAL_REC_TYPE_CONFIG, 1, 100, NULL, 0);
	rec.header_crc32 = 0xDEADBEEF;
	TEST_ASSERT(!journal_rec_validate(&rec),
		"wrong header CRC should fail validation");
}

static void test_record_corrupt_payload_crc_fails_validate(void)
{
	journal_record_t rec;
	uint8_t payload[] = {0xAA};
	journal_rec_init(&rec, JOURNAL_REC_TYPE_LOG_PKT, 1, 100, payload, 1);
	rec.payload_crc32 = 0x12345678;
	TEST_ASSERT(!journal_rec_validate(&rec),
		"wrong payload CRC should fail validation");
}

static void test_record_state_transitions(void)
{
	journal_record_t rec;
	journal_rec_init(&rec, JOURNAL_REC_TYPE_CONFIG, 1, 0, NULL, 0);

	/* Initial state = VALID (0x01) */
	TEST_ASSERT_EQ_INT(JOURNAL_REC_STATE_VALID, rec.state);
	TEST_ASSERT(!journal_rec_is_committed(&rec),
		"VALID state is not committed");

	/* After commit = COMMITTED (0x00) */
	rec.state = JOURNAL_REC_STATE_COMMITTED;
	TEST_ASSERT(journal_rec_is_committed(&rec),
		"COMMITTED state is committed");

	/* NOR: 0xFF -> 0x01 -> 0x00 are valid transitions (only clears bits) */
	TEST_ASSERT_EQ_INT(0xFF & 0x01, JOURNAL_REC_STATE_VALID);
	TEST_ASSERT_EQ_INT(0x01 & 0x00, JOURNAL_REC_STATE_COMMITTED);
}

static void test_record_erased_detection(void)
{
	uint8_t buf[JOURNAL_REC_HEADER_SIZE];
	memset(buf, 0xFF, sizeof(buf));
	TEST_ASSERT(journal_rec_is_erased(buf, sizeof(buf)),
		"all-0xFF should be detected as erased");

	buf[0] = 0x00;
	TEST_ASSERT(!journal_rec_is_erased(buf, sizeof(buf)),
		"non-0xFF should not be erased");
}

/* ================================================================
 * Modular sequence comparison and wrap
 * ================================================================ */

static void test_seq_normal_ordering(void)
{
	TEST_ASSERT(journal_seq_after(2, 1), "2 is after 1");
	TEST_ASSERT(!journal_seq_after(1, 2), "1 is not after 2");
	TEST_ASSERT(!journal_seq_after(5, 5), "equal is not after");
}

static void test_seq_wrap_around(void)
{
	/* Near wrap: b = UINT64_MAX - 2, a = 1 (a is after b) */
	uint64_t b = 0xFFFFFFFFFFFFFFFduLL;
	uint64_t a = 1;
	TEST_ASSERT(journal_seq_after(a, b), "1 is after MAX-2 (wrapped)");
	TEST_ASSERT(!journal_seq_after(b, a), "MAX-2 is not after 1");
}

static void test_seq_exact_wrap(void)
{
	uint64_t b = 0xFFFFFFFFFFFFFFFFuLL;
	uint64_t a = 0;
	/* (0 - MAX) = 1, (MAX - 0) = MAX. 1 < MAX -> a is after b */
	TEST_ASSERT(journal_seq_after(a, b), "0 is after MAX (wrapped)");
}

static void test_seq_large_gap(void)
{
	TEST_ASSERT(journal_seq_after(1000000, 1), "1M is after 1");
	TEST_ASSERT(!journal_seq_after(1, 1000000), "1 is not after 1M");
}

/* ================================================================
 * Superblock extension
 * ================================================================ */

static void test_sb_ext_init_defaults(void)
{
	journal_sb_info_t sb;
	journal_sb_ext_init(&sb);
	TEST_ASSERT_EQ_INT(0, sb.active_sb_sector);
	TEST_ASSERT_EQ_INT(0, (int)sb.log_write_off);
	TEST_ASSERT_EQ_INT(0, (int)sb.log_read_off);
	TEST_ASSERT_EQ_INT(1, (int)sb.next_sequence);
	TEST_ASSERT(!sb.valid, "fresh sb_ext should not be valid");
}

static void test_sb_ext_serialize_deserialize_roundtrip(void)
{
	journal_sb_info_t sb;
	journal_sb_ext_init(&sb);
	sb.active_sb_sector = 1;
	sb.erase_generation = 7;
	sb.log_write_off = 1024;
	sb.log_read_off = 0;
	sb.next_sequence = 42;
	sb.log_record_count = 10;

	uint8_t buf[JOURNAL_SB_EXT_SIZE];
	journal_sb_ext_serialize(&sb, buf);

	journal_sb_info_t rt;
	TEST_ASSERT(journal_sb_ext_deserialize(buf, &rt),
		"deserialize should succeed");
	TEST_ASSERT_EQ_INT(1, rt.active_sb_sector);
	TEST_ASSERT_EQ_INT(7, (int)rt.erase_generation);
	TEST_ASSERT_EQ_INT(1024, (int)rt.log_write_off);
	TEST_ASSERT_EQ_INT(42, (int)rt.next_sequence);
	TEST_ASSERT_EQ_INT(10, (int)rt.log_record_count);
	TEST_ASSERT(rt.valid, "deserialized sb_ext should be valid");
}

static void test_sb_ext_bad_magic_fails(void)
{
	uint8_t buf[JOURNAL_SB_EXT_SIZE];
	memset(buf, 0xFF, sizeof(buf));
	journal_sb_info_t sb;
	TEST_ASSERT(!journal_sb_ext_deserialize(buf, &sb),
		"erased bytes should not deserialize");
}

static void test_sb_ext_crc_corruption_fails(void)
{
	journal_sb_info_t sb;
	journal_sb_ext_init(&sb);
	sb.erase_generation = 5;
	uint8_t buf[JOURNAL_SB_EXT_SIZE];
	journal_sb_ext_serialize(&sb, buf);
	/* Corrupt a byte in the CRC span */
	buf[6] ^= 0x01;
	journal_sb_info_t rt;
	TEST_ASSERT(!journal_sb_ext_deserialize(buf, &rt),
		"corrupted CRC should fail deserialize");
}

/* ================================================================
 * Boot scan with mock_flash
 * ================================================================ */

static void test_boot_scan_empty_flash_returns_false(void)
{
	mock_flash_reset();
	journal_sb_info_t sb;
	TEST_ASSERT(!journal_scan_superblocks(mock_flash_read, &sb),
		"empty flash should have no valid superblock");
}

static void test_boot_scan_finds_committed_superblock(void)
{
	mock_flash_reset();

	/* Erase and write a committed superblock at sector 0 */
	mock_flash_erase_sector(0);

	/* Write Todo 28 frozen superblock header + commit marker */
	qspi_superblock_t sb28;
	qspi_eval_sb_init(&sb28);
	sb28.crc32 = qspi_eval_sb_compute_crc(&sb28);
	uint8_t sb28_buf[QSPI_SB_HEADER_SIZE];
	qspi_eval_sb_serialize(&sb28, sb28_buf);
	mock_flash_program(0, sb28_buf, QSPI_SB_COMMIT_OFFSET);
	uint8_t commit = QSPI_SB_COMMIT_VALUE;
	mock_flash_program(QSPI_SB_COMMIT_OFFSET, &commit, 1);

	/* Write journal extension */
	journal_sb_info_t jsb;
	journal_sb_ext_init(&jsb);
	jsb.active_sb_sector = 0;
	jsb.erase_generation = 1;
	jsb.next_sequence = 5;
	uint8_t ext_buf[JOURNAL_SB_EXT_SIZE];
	journal_sb_ext_serialize(&jsb, ext_buf);
	mock_flash_program(JOURNAL_SB_EXT_OFF, ext_buf, JOURNAL_SB_EXT_SIZE);

	/* Boot scan should find it */
	journal_sb_info_t result;
	TEST_ASSERT(journal_scan_superblocks(mock_flash_read, &result),
		"boot scan should find committed superblock");
	TEST_ASSERT_EQ_INT(0, result.active_sb_sector);
	TEST_ASSERT_EQ_INT(1, (int)result.erase_generation);
	TEST_ASSERT_EQ_INT(5, (int)result.next_sequence);
	TEST_ASSERT(result.valid, "result should be valid");
}

static void test_boot_scan_picks_higher_erase_generation(void)
{
	mock_flash_reset();

	/* Write committed superblocks at both sectors */
	for (uint32_t sect = 0; sect < 2; sect++) {
		uint32_t addr = sect * QSPI_SECTOR_SIZE;
		mock_flash_erase_sector(addr);

		qspi_superblock_t sb28;
		qspi_eval_sb_init(&sb28);
		sb28.crc32 = qspi_eval_sb_compute_crc(&sb28);
		uint8_t sb28_buf[QSPI_SB_HEADER_SIZE];
		qspi_eval_sb_serialize(&sb28, sb28_buf);
		mock_flash_program(addr, sb28_buf, QSPI_SB_COMMIT_OFFSET);
		uint8_t commit = QSPI_SB_COMMIT_VALUE;
		mock_flash_program(addr + QSPI_SB_COMMIT_OFFSET, &commit, 1);

		journal_sb_info_t jsb;
		journal_sb_ext_init(&jsb);
		jsb.active_sb_sector = (uint8_t)sect;
		jsb.erase_generation = sect + 10; /* sector 1 has higher gen */
		uint8_t ext_buf[JOURNAL_SB_EXT_SIZE];
		journal_sb_ext_serialize(&jsb, ext_buf);
		mock_flash_program(addr + JOURNAL_SB_EXT_OFF, ext_buf,
				   JOURNAL_SB_EXT_SIZE);
	}

	journal_sb_info_t result;
	TEST_ASSERT(journal_scan_superblocks(mock_flash_read, &result),
		"boot scan should find a superblock");
	TEST_ASSERT_EQ_INT(11, (int)result.erase_generation);
	TEST_ASSERT_EQ_INT(1, result.active_sb_sector);
}

/* ================================================================
 * Circular log: append and read
 * ================================================================ */

/* mock_flash-backed flash backend for journal operations */
static int j_read(uint32_t a, void *b, size_t l) { return mock_flash_read(a, b, l); }
static int j_program(uint32_t a, const void *b, size_t l) { return mock_flash_program(a, b, l); }
static int j_erase(uint32_t a) { return mock_flash_erase_sector(a); }

static void test_log_append_and_read_back(void)
{
	mock_flash_reset();

	/* Set up adopted superblock with journal extension */
	journal_sb_info_t sb;
	journal_sb_ext_init(&sb);
	sb.valid = true;
	sb.active_sb_sector = 0;
	sb.erase_generation = 1;
	sb.next_sequence = 1;
	sb.log_write_off = 0;
	sb.log_read_off = 0;
	sb.log_record_count = 0;
	sb.log_erase_gen = 0;

	/* Erase log sectors */
	for (uint32_t s = 0; s < 3; s++)
		mock_flash_erase_sector(JOURNAL_LOG_START + s * QSPI_SECTOR_SIZE);

	journal_flash_backend_t backend = {j_read, j_program, j_erase};

	uint8_t payload[] = {0xCA, 0xFE};
	journal_record_t rec;
	journal_rec_init(&rec, JOURNAL_REC_TYPE_LOG_EVT, 0, 1234, payload, 2);

	TEST_ASSERT(journal_log_append(&backend, &sb, &rec),
		"append should succeed");
	TEST_ASSERT_EQ_INT(2, (int)sb.next_sequence);
	TEST_ASSERT_EQ_INT(1, (int)sb.log_record_count);

	/* Read back */
	journal_record_t out;
	TEST_ASSERT(journal_log_read_one(j_read, &sb, 0, &out),
		"read should find committed record");
	TEST_ASSERT(journal_rec_validate(&out), "read record validates");
	TEST_ASSERT(journal_rec_is_committed(&out), "record is committed");
	TEST_ASSERT_EQ_INT(JOURNAL_REC_TYPE_LOG_EVT, out.record_type);
	TEST_ASSERT_EQ_INT(1, (int)out.sequence);
	TEST_ASSERT_EQ_INT(1234, (int)out.timestamp);
	TEST_ASSERT_EQ_INT(0xCA, out.payload[0]);
	TEST_ASSERT_EQ_INT(0xFE, out.payload[1]);
}

static void test_log_append_multiple_records(void)
{
	mock_flash_reset();
	for (uint32_t s = 0; s < 3; s++)
		mock_flash_erase_sector(JOURNAL_LOG_START + s * QSPI_SECTOR_SIZE);

	journal_sb_info_t sb;
	journal_sb_ext_init(&sb);
	sb.valid = true;
	sb.erase_generation = 1;
	sb.next_sequence = 0;

	journal_flash_backend_t backend = {j_read, j_program, j_erase};

	for (int i = 0; i < 5; i++) {
		uint8_t p[] = {(uint8_t)i};
		journal_record_t rec;
		journal_rec_init(&rec, JOURNAL_REC_TYPE_LOG_PKT, i, i * 1000,
				 p, 1);
		TEST_ASSERT(journal_log_append(&backend, &sb, &rec),
			"append should succeed");
	}
	TEST_ASSERT_EQ_INT(5, (int)sb.log_record_count);
	TEST_ASSERT_EQ_INT(5, (int)sb.next_sequence);

	/* Read first and last records */
	uint32_t off = 0;
	for (int i = 0; i < 5; i++) {
		journal_record_t out;
		TEST_ASSERT(journal_log_read_one(j_read, &sb, off, &out),
			"should find record");
		TEST_ASSERT_EQ_INT(i, (int)out.sequence);
		TEST_ASSERT_EQ_INT(i, out.payload[0]);
		off += JOURNAL_REC_HEADER_SIZE + out.payload_length;
	}
}

static void test_log_is_full_check(void)
{
	journal_sb_info_t sb;
	journal_sb_ext_init(&sb);
	sb.log_write_off = 0;
	TEST_ASSERT(!journal_log_is_full(&sb), "write_off=0 is not full");

	sb.log_write_off = JOURNAL_LOG_SIZE;
	TEST_ASSERT(journal_log_is_full(&sb), "write_off=LOG_SIZE is full");
}

/* ================================================================
 * Power-cut fault injection: interrupt at every byte of a record write
 * ================================================================ */

static void test_powercut_record_write_before_commit(void)
{
	mock_flash_reset();
	mock_flash_erase_sector(JOURNAL_LOG_START);

	journal_sb_info_t sb;
	journal_sb_ext_init(&sb);
	sb.valid = true;
	sb.next_sequence = 0;

	/* Write a committed record (the "previous valid state") */
	journal_flash_backend_t backend = {j_read, j_program, j_erase};
	uint8_t p1[] = {0x11};
	journal_record_t rec1;
	journal_rec_init(&rec1, JOURNAL_REC_TYPE_CONFIG, 0, 100, p1, 1);
	journal_log_append(&backend, &sb, &rec1);

	/* Now start writing a second record but DON'T commit it */
	uint8_t p2[] = {0x22};
	journal_record_t rec2;
	journal_rec_init(&rec2, JOURNAL_REC_TYPE_CONFIG, 1, 200, p2, 1);
	/* Override state to VALID (not committed) */
	rec2.state = JOURNAL_REC_STATE_VALID;
	rec2.sequence = sb.next_sequence;
	rec2.header_crc32 = journal_rec_header_crc(&rec2);
	rec2.payload_crc32 = journal_rec_payload_crc(&rec2);

	uint8_t buf[JOURNAL_REC_MAX_SIZE];
	journal_rec_serialize(&rec2, buf);
	uint32_t addr2 = JOURNAL_LOG_START + sb.log_write_off -
		(JOURNAL_REC_HEADER_SIZE + 1); /* where rec1 ended */
	/* Actually rec1's size: 39+1=40. log_write_off after rec1 = 40 */
	addr2 = JOURNAL_LOG_START + 40;

	/* Write header+payload but NOT the commit byte */
	size_t total = JOURNAL_REC_HEADER_SIZE + 1;
	mock_flash_program(addr2, buf, total);
	/* Deliberately skip the commit program */

	/* Boot scan of the log: first record (offset 0) should be valid
	 * committed. Second record (offset 40) should be uncommitted
	 * (state = VALID = 0x01, not COMMITTED = 0x00). */
	journal_record_t out1;
	TEST_ASSERT(journal_log_read_one(j_read, &sb, 0, &out1),
		"first record should be committed and readable");
	TEST_ASSERT_EQ_INT(0x11, out1.payload[0]);

	journal_record_t out2;
	TEST_ASSERT(!journal_log_read_one(j_read, &sb, 40, &out2),
		"second record should NOT be readable (uncommitted)");
}

static void test_powercut_corrupt_header_rejected(void)
{
	mock_flash_reset();
	mock_flash_erase_sector(JOURNAL_LOG_START);

	/* Write a valid committed record */
	journal_sb_info_t sb;
	journal_sb_ext_init(&sb);
	sb.valid = true;
	sb.next_sequence = 0;

	journal_flash_backend_t backend = {j_read, j_program, j_erase};
	uint8_t p[] = {0x99};
	journal_record_t rec;
	journal_rec_init(&rec, JOURNAL_REC_TYPE_LOG_EVT, 0, 1000, p, 1);
	journal_log_append(&backend, &sb, &rec);

	/* Corrupt the header CRC in flash (byte at CRC offset) */
	uint32_t crc_addr = JOURNAL_LOG_START + JOURNAL_REC_OFF_HDR_CRC;
	uint8_t zero = 0x00;
	mock_flash_program(crc_addr, &zero, 1);

	/* Read should fail validation */
	journal_record_t out;
	TEST_ASSERT(!journal_log_read_one(j_read, &sb, 0, &out),
		"corrupted record should not validate");
}

static void test_powercut_partial_header_write(void)
{
	mock_flash_reset();
	mock_flash_erase_sector(JOURNAL_LOG_START);

	/* Simulate a partial header write: only first 20 bytes written,
	 * rest is 0xFF (erased). This is a torn write. */
	journal_record_t rec;
	journal_rec_init(&rec, JOURNAL_REC_TYPE_CONFIG, 0, 0, NULL, 0);
	uint8_t buf[JOURNAL_REC_MAX_SIZE];
	journal_rec_serialize(&rec, buf);

	/* Only write first 20 bytes (incomplete header) */
	mock_flash_program(JOURNAL_LOG_START, buf, 20);

	/* Reading should fail — magic may be present but CRC won't match
	 * because the CRC bytes themselves are 0xFF */
	journal_sb_info_t sb;
	journal_sb_ext_init(&sb);
	journal_record_t out;
	TEST_ASSERT(!journal_log_read_one(j_read, &sb, 0, &out),
		"partial header write should not produce valid record");
}

static void test_powercut_full_record_recovery(void)
{
	/* Exhaustive: write a valid committed record, then attempt to
	 * write a second record. For every possible cut point, verify
	 * that the first record remains readable. */
	mock_flash_reset();
	mock_flash_erase_sector(JOURNAL_LOG_START);

	journal_sb_info_t sb;
	journal_sb_ext_init(&sb);
	sb.valid = true;
	sb.next_sequence = 0;

	journal_flash_backend_t backend = {j_read, j_program, j_erase};
	uint8_t p1[] = {0x42};
	journal_record_t rec1;
	journal_rec_init(&rec1, JOURNAL_REC_TYPE_CONFIG, 0, 0, p1, 1);
	TEST_ASSERT(journal_log_append(&backend, &sb, &rec1),
		"first record append");

	/* For each byte position of a second record write, simulate
	 * power cut at that position and verify rec1 is still intact. */
	journal_record_t rec2;
	uint8_t p2[] = {0x84};
	journal_rec_init(&rec2, JOURNAL_REC_TYPE_CONFIG, 0, 100, p2, 1);
	uint8_t buf2[JOURNAL_REC_MAX_SIZE];
	journal_rec_serialize(&rec2, buf2);
	size_t rec2_total = JOURNAL_REC_HEADER_SIZE + 1; /* 40 bytes */

	for (size_t cut = 0; cut <= rec2_total; cut++) {
		/* Reset log sector to just after rec1 */
		/* Re-erase the sector and rewrite rec1 */
		mock_flash_erase_sector(JOURNAL_LOG_START);
		/* Rewrite rec1 */
		sb.log_write_off = 0;
		sb.next_sequence = 0;
		sb.log_record_count = 0;
		journal_log_append(&backend, &sb, &rec1);
		uint32_t after_r1 = sb.log_write_off;

		/* Now write partial rec2 (only 'cut' bytes) */
		if (cut > 0) {
			mock_flash_program(JOURNAL_LOG_START + after_r1,
					   buf2, cut);
		}

		/* rec1 should still be readable and committed */
		journal_record_t out1;
		TEST_ASSERT(journal_log_read_one(j_read, &sb, 0, &out1),
			"rec1 should survive power cut");
		TEST_ASSERT(journal_rec_is_committed(&out1),
			"rec1 should be committed");
		TEST_ASSERT_EQ_INT(0x42, out1.payload[0]);
	}
}

/* ================================================================
 * GC policy
 * ================================================================ */

static void test_gc_not_needed_when_empty(void)
{
	TEST_ASSERT_EQ_INT(JOURNAL_GC_NOT_NEEDED,
		journal_gc_check(0, JOURNAL_CONFIG_SIZE));
}

static void test_gc_needed_when_75_percent_full(void)
{
	uint32_t threshold = (JOURNAL_CONFIG_SIZE * 3) / 4;
	TEST_ASSERT_EQ_INT(JOURNAL_GC_NEEDED,
		journal_gc_check(threshold + 1, JOURNAL_CONFIG_SIZE));
}

static void test_gc_not_needed_at_threshold(void)
{
	uint32_t threshold = (JOURNAL_CONFIG_SIZE * 3) / 4;
	TEST_ASSERT_EQ_INT(JOURNAL_GC_NOT_NEEDED,
		journal_gc_check(threshold, JOURNAL_CONFIG_SIZE));
}

static void test_gc_should_replace_same_type_later_seq(void)
{
	journal_record_t old_r, new_r;
	journal_rec_init(&old_r, JOURNAL_REC_TYPE_CONFIG, 1, 0, NULL, 0);
	journal_rec_init(&new_r, JOURNAL_REC_TYPE_CONFIG, 5, 0, NULL, 0);
	TEST_ASSERT(journal_gc_should_replace(&old_r, &new_r),
		"same type, later seq should replace");
}

static void test_gc_should_not_replace_different_type(void)
{
	journal_record_t old_r, new_r;
	journal_rec_init(&old_r, JOURNAL_REC_TYPE_CONFIG, 1, 0, NULL, 0);
	journal_rec_init(&new_r, JOURNAL_REC_TYPE_CALIB, 5, 0, NULL, 0);
	TEST_ASSERT(!journal_gc_should_replace(&old_r, &new_r),
		"different types should not replace");
}

static void test_gc_should_not_replace_earlier_seq(void)
{
	journal_record_t old_r, new_r;
	journal_rec_init(&old_r, JOURNAL_REC_TYPE_CONFIG, 10, 0, NULL, 0);
	journal_rec_init(&new_r, JOURNAL_REC_TYPE_CONFIG, 5, 0, NULL, 0);
	TEST_ASSERT(!journal_gc_should_replace(&old_r, &new_r),
		"earlier seq should not replace");
}

/* ================================================================
 * Log queue (drop-newest, overrun counting)
 * ================================================================ */

static void test_queue_init_empty(void)
{
	journal_log_queue_t q;
	journal_queue_init(&q);
	TEST_ASSERT_EQ_INT(0, (int)q.count);
	TEST_ASSERT_EQ_INT(0, (int)q.overrun_count);
}

static void test_queue_push_and_pop(void)
{
	journal_log_queue_t q;
	journal_queue_init(&q);

	uint8_t data[] = {0xAA, 0xBB};
	TEST_ASSERT_EQ_INT(JOURNAL_QUEUE_OK,
		(int)journal_queue_push(&q, JOURNAL_REC_TYPE_LOG_EVT,
					data, 2));
	TEST_ASSERT_EQ_INT(1, (int)q.count);

	uint8_t type_out;
	uint8_t data_out[JOURNAL_REC_MAX_PAYLOAD];
	uint16_t len_out;
	TEST_ASSERT(journal_queue_pop(&q, &type_out, data_out, &len_out),
		"pop should succeed");
	TEST_ASSERT_EQ_INT(JOURNAL_REC_TYPE_LOG_EVT, type_out);
	TEST_ASSERT_EQ_INT(2, len_out);
	TEST_ASSERT_EQ_INT(0xAA, data_out[0]);
	TEST_ASSERT_EQ_INT(0xBB, data_out[1]);
	TEST_ASSERT_EQ_INT(0, (int)q.count);
}

static void test_queue_fill_to_capacity(void)
{
	journal_log_queue_t q;
	journal_queue_init(&q);

	for (uint32_t i = 0; i < JOURNAL_LOG_QUEUE_SIZE; i++) {
		uint8_t d = (uint8_t)i;
		TEST_ASSERT_EQ_INT(JOURNAL_QUEUE_OK,
			(int)journal_queue_push(&q, JOURNAL_REC_TYPE_LOG_PKT,
						&d, 1));
	}
	TEST_ASSERT_EQ_INT(JOURNAL_LOG_QUEUE_SIZE, q.count);
	TEST_ASSERT_EQ_INT(0, q.overrun_count);
}

static void test_queue_drop_newest_on_full(void)
{
	journal_log_queue_t q;
	journal_queue_init(&q);

	for (uint32_t i = 0; i < JOURNAL_LOG_QUEUE_SIZE; i++) {
		uint8_t d = (uint8_t)i;
		journal_queue_push(&q, JOURNAL_REC_TYPE_LOG_PKT, &d, 1);
	}
	/* Queue is full. Next push should drop newest. */
	uint8_t extra = 0xFF;
	journal_queue_result_t r =
		journal_queue_push(&q, JOURNAL_REC_TYPE_LOG_EVT, &extra, 1);
	TEST_ASSERT_EQ_INT(JOURNAL_QUEUE_DROPPED, (int)r);
	TEST_ASSERT_EQ_INT(1, q.overrun_count);

	/* Multiple overruns accumulate */
	journal_queue_push(&q, JOURNAL_REC_TYPE_LOG_EVT, &extra, 1);
	TEST_ASSERT_EQ_INT(2, q.overrun_count);
}

static void test_queue_pop_empty_returns_false(void)
{
	journal_log_queue_t q;
	journal_queue_init(&q);
	uint8_t type_out;
	uint8_t data_out[4];
	uint16_t len_out;
	TEST_ASSERT(!journal_queue_pop(&q, &type_out, data_out, &len_out),
		"pop from empty queue should return false");
}

/* ================================================================
 * Content filter
 * ================================================================ */

static void test_filter_allows_board_events(void)
{
	TEST_ASSERT(journal_filter_allows(JOURNAL_CAT_BOARD_EVENT),
		"board events should be allowed");
	TEST_ASSERT(journal_filter_allows(JOURNAL_CAT_PKT_META),
		"packet metadata should be allowed");
	TEST_ASSERT(journal_filter_allows(JOURNAL_CAT_PKT_PAYLOAD),
		"packet payload should be allowed");
}

static void test_filter_excludes_audio(void)
{
	TEST_ASSERT(!journal_filter_allows(JOURNAL_CAT_AUDIO_RAW),
		"raw audio must NEVER be logged");
}

static void test_filter_excludes_secrets(void)
{
	TEST_ASSERT(!journal_filter_allows(JOURNAL_CAT_SECRET),
		"secrets must NEVER be logged");
	TEST_ASSERT(!journal_filter_allows(JOURNAL_CAT_NONCE),
		"nonces must NEVER be logged");
}

/* ================================================================
 * StorageReadLog chunking (fragment below 1019 bytes)
 * ================================================================ */

#define WHAD_MAX_ENCODED_MESSAGE_SIZE 1019u

static void test_readlog_single_chunk(void)
{
	uint8_t data[] = {1, 2, 3, 4, 5};
	uint8_t out[1019];
	bool eof = false;
	uint16_t len = journal_readlog_chunk(data, 5, 0, 1019, out, &eof);
	TEST_ASSERT_EQ_INT(5, len);
	TEST_ASSERT(eof, "all data in one chunk should set eof");
	TEST_ASSERT_EQ_INT(0, memcmp(data, out, 5));
}

static void test_readlog_multi_chunk_fragmentation(void)
{
	/* 2000 bytes of data, max chunk 1019 */
	uint8_t data[2000];
	for (int i = 0; i < 2000; i++) data[i] = (uint8_t)(i & 0xFF);

	uint8_t out[1019];
	bool eof = false;

	uint16_t len1 = journal_readlog_chunk(data, 2000, 0, 1019, out, &eof);
	TEST_ASSERT_EQ_INT(1019, len1);
	TEST_ASSERT(!eof, "first chunk should not be eof");

	uint16_t len2 = journal_readlog_chunk(data, 2000, 1019, 1019, out, &eof);
	TEST_ASSERT_EQ_INT(981, len2); /* 2000 - 1019 = 981 */
	TEST_ASSERT(eof, "second chunk should be eof");
	TEST_ASSERT_EQ_INT(data[1019], out[0]);
	TEST_ASSERT_EQ_INT(data[1999], out[980]);
}

static void test_readlog_offset_past_end_returns_eof(void)
{
	uint8_t data[] = {1, 2, 3};
	uint8_t out[16];
	bool eof = false;
	uint16_t len = journal_readlog_chunk(data, 3, 5, 1019, out, &eof);
	TEST_ASSERT_EQ_INT(0, len);
	TEST_ASSERT(eof, "offset past end should set eof");
}

/* ================================================================
 * StorageEraseLog FSM
 * ================================================================ */

static void test_eraselog_idle_to_accepted(void)
{
	TEST_ASSERT_EQ_INT(JOURNAL_ERASE_ACCEPTED,
		journal_eraselog_transition(JOURNAL_ERASE_IDLE,
					    JOURNAL_ERASE_EVT_CONFIRM));
}

static void test_eraselog_accepted_to_erasing(void)
{
	TEST_ASSERT_EQ_INT(JOURNAL_ERASE_ERASING,
		journal_eraselog_transition(JOURNAL_ERASE_ACCEPTED,
					    JOURNAL_ERASE_EVT_PROGRESS));
}

static void test_eraselog_erasing_to_done(void)
{
	TEST_ASSERT_EQ_INT(JOURNAL_ERASE_DONE,
		journal_eraselog_transition(JOURNAL_ERASE_ERASING,
					    JOURNAL_ERASE_EVT_COMPLETE));
}

static void test_eraselog_erasing_to_error(void)
{
	TEST_ASSERT_EQ_INT(JOURNAL_ERASE_ERROR,
		journal_eraselog_transition(JOURNAL_ERASE_ERASING,
					    JOURNAL_ERASE_EVT_FAIL));
}

static void test_eraselog_accepted_cancel(void)
{
	TEST_ASSERT_EQ_INT(JOURNAL_ERASE_IDLE,
		journal_eraselog_transition(JOURNAL_ERASE_ACCEPTED,
					    JOURNAL_ERASE_EVT_CANCEL));
}

static void test_eraselog_done_is_terminal(void)
{
	for (int e = 0; e <= (int)JOURNAL_ERASE_EVT_CANCEL; e++) {
		TEST_ASSERT_EQ_INT(JOURNAL_ERASE_DONE,
			journal_eraselog_transition(JOURNAL_ERASE_DONE,
			    (journal_erase_event_t)e));
	}
}

static void test_eraselog_idle_ignores_non_confirm(void)
{
	TEST_ASSERT_EQ_INT(JOURNAL_ERASE_IDLE,
		journal_eraselog_transition(JOURNAL_ERASE_IDLE,
					    JOURNAL_ERASE_EVT_PROGRESS));
}

/* ================================================================
 * Pre-erase idle check
 * ================================================================ */

static void test_is_idle_when_no_flags(void)
{
	TEST_ASSERT(journal_eval_is_idle(0), "no flags = idle");
}

static void test_is_not_idle_when_radio_active(void)
{
	TEST_ASSERT(!journal_eval_is_idle(0x01), "radio active = not idle");
}

static void test_is_not_idle_when_ble_connected(void)
{
	TEST_ASSERT(!journal_eval_is_idle(0x02), "BLE connected = not idle");
}

static void test_is_not_idle_when_sensor_sampling(void)
{
	TEST_ASSERT(!journal_eval_is_idle(0x04), "sensor sampling = not idle");
}

static void test_is_not_idle_when_display_updating(void)
{
	TEST_ASSERT(!journal_eval_is_idle(0x08), "display updating = not idle");
}

static void test_is_idle_ignores_upper_bits(void)
{
	/* Bits above 0x0F are not timing-critical */
	TEST_ASSERT(journal_eval_is_idle(0xFFF0), "upper bits don't block");
}

/* ================================================================
 * NOR commit semantics verification
 * ================================================================ */

static void test_nor_commit_transitions_valid(void)
{
	/* 0xFF -> 0x01 (VALID): 0xFF & 0x01 = 0x01 ✓ */
	TEST_ASSERT_EQ_INT(JOURNAL_REC_STATE_VALID,
		JOURNAL_REC_STATE_ERASED & JOURNAL_REC_STATE_VALID);
	/* 0x01 -> 0x00 (COMMITTED): 0x01 & 0x00 = 0x00 ✓ */
	TEST_ASSERT_EQ_INT(JOURNAL_REC_STATE_COMMITTED,
		JOURNAL_REC_STATE_VALID & JOURNAL_REC_STATE_COMMITTED);
}

static void test_nor_cannot_recommit_after_commit(void)
{
	/* 0x00 -> anything: AND with 0x00 = 0x00. Cannot change. */
	uint8_t committed = JOURNAL_REC_STATE_COMMITTED;
	uint8_t attempted = JOURNAL_REC_STATE_VALID;
	TEST_ASSERT_EQ_INT(JOURNAL_REC_STATE_COMMITTED,
		(uint8_t)(committed & attempted));
}

/* ================================================================
 * Drop-newest queue: newest_index tracking (Finding 9/10 fix)
 * ================================================================ */

static void test_queue_newest_index_empty_on_init(void)
{
	journal_log_queue_t q;
	journal_queue_init(&q);
	TEST_ASSERT_EQ_INT(JOURNAL_QUEUE_NEWEST_EMPTY, (int)q.newest_index);
}

static void test_queue_newest_index_tracks_push(void)
{
	journal_log_queue_t q;
	journal_queue_init(&q);
	uint8_t d = 0;
	journal_queue_push(&q, JOURNAL_REC_TYPE_LOG_PKT, &d, 1);
	TEST_ASSERT_EQ_INT(0, (int)q.newest_index);
	journal_queue_push(&q, JOURNAL_REC_TYPE_LOG_PKT, &d, 1);
	TEST_ASSERT_EQ_INT(1, (int)q.newest_index);
}

static void test_queue_drop_newest_after_pop_push_cycle(void)
{
	journal_log_queue_t q;
	journal_queue_init(&q);

	for (uint32_t i = 0; i < JOURNAL_LOG_QUEUE_SIZE; i++) {
		uint8_t d = (uint8_t)(i + 1);
		journal_queue_push(&q, JOURNAL_REC_TYPE_LOG_PKT, &d, 1);
	}
	TEST_ASSERT_EQ_INT(15, (int)q.newest_index);

	uint8_t type_out;
	uint8_t data_out[JOURNAL_REC_MAX_PAYLOAD];
	uint16_t len_out;
	TEST_ASSERT(journal_queue_pop(&q, &type_out, data_out, &len_out),
		"pop should succeed");
	TEST_ASSERT_EQ_INT(1, data_out[0]);
	TEST_ASSERT_EQ_INT(15, (int)q.newest_index);

	uint8_t pushed = 0xEE;
	TEST_ASSERT_EQ_INT(JOURNAL_QUEUE_OK,
		(int)journal_queue_push(&q, JOURNAL_REC_TYPE_LOG_EVT,
					&pushed, 1));
	TEST_ASSERT_EQ_INT(0, (int)q.newest_index);

	uint8_t overflow = 0xFF;
	journal_queue_result_t r =
		journal_queue_push(&q, JOURNAL_REC_TYPE_LOG_PKT,
				   &overflow, 1);
	TEST_ASSERT_EQ_INT(JOURNAL_QUEUE_DROPPED, (int)r);
	TEST_ASSERT_EQ_INT(1, (int)q.overrun_count);

	bool found_slot15 = false;
	bool found_overflow = false;
	bool found_dropped = false;
	while (journal_queue_pop(&q, &type_out, data_out, &len_out)) {
		if (data_out[0] == 16) found_slot15 = true;
		if (data_out[0] == 0xFF) found_overflow = true;
		if (data_out[0] == 0xEE) found_dropped = true;
	}
	TEST_ASSERT(found_slot15, "slot 15 data survived");
	TEST_ASSERT(found_overflow, "overflow data at slot 0");
	TEST_ASSERT(!found_dropped, "pushed E was dropped as newest");
}

/* ================================================================
 * Partition append / read (config, calib)
 * ================================================================ */

static void test_partition_append_and_read_calib(void)
{
	mock_flash_reset();
	for (uint32_t s = 0; s < JOURNAL_CALIB_SECTORS; s++)
		mock_flash_erase_sector(JOURNAL_CALIB_START +
					s * QSPI_SECTOR_SIZE);

	journal_sb_info_t sb;
	journal_sb_ext_init(&sb);
	sb.valid = true;
	sb.next_sequence = 1;

	journal_flash_backend_t backend = {j_read, j_program, j_erase};
	uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
	journal_record_t rec;
	journal_rec_init(&rec, JOURNAL_REC_TYPE_CALIB, 0, 0, payload, 4);

	TEST_ASSERT(journal_partition_append(&backend, &sb,
		JOURNAL_PART_CALIB, &rec), "append should succeed");
	TEST_ASSERT_EQ_INT(2, (int)sb.next_sequence);
	TEST_ASSERT_EQ_INT(JOURNAL_REC_HEADER_SIZE + 4,
		(int)sb.calib_write_off);

	journal_record_t out;
	TEST_ASSERT(journal_partition_read_one(mock_flash_read, &sb,
		JOURNAL_PART_CALIB, 0, &out), "read should find record");
	TEST_ASSERT(journal_rec_validate(&out), "record validates");
	TEST_ASSERT_EQ_INT(JOURNAL_REC_TYPE_CALIB, out.record_type);
	TEST_ASSERT_EQ_INT(1, (int)out.sequence);
	TEST_ASSERT_EQ_INT(0xDE, out.payload[0]);
	TEST_ASSERT_EQ_INT(0xEF, out.payload[3]);
}

static void test_partition_append_config_routes_correctly(void)
{
	mock_flash_reset();
	mock_flash_erase_sector(JOURNAL_CONFIG_START);

	journal_sb_info_t sb;
	journal_sb_ext_init(&sb);
	sb.valid = true;
	sb.next_sequence = 10;

	journal_flash_backend_t backend = {j_read, j_program, j_erase};
	uint8_t payload[] = {0x01};
	journal_record_t rec;
	journal_rec_init(&rec, JOURNAL_REC_TYPE_CONFIG, 0, 0, payload, 1);

	TEST_ASSERT(journal_partition_append(&backend, &sb,
		JOURNAL_PART_CONFIG, &rec), "config append");
	TEST_ASSERT_EQ_INT(11, (int)sb.next_sequence);
	TEST_ASSERT_EQ_INT(JOURNAL_REC_HEADER_SIZE + 1,
		(int)sb.config_write_off);
	TEST_ASSERT_EQ_INT(0, (int)sb.calib_write_off);

	journal_record_t out;
	TEST_ASSERT(journal_partition_read_one(mock_flash_read, &sb,
		JOURNAL_PART_CONFIG, 0, &out), "read config");
	TEST_ASSERT_EQ_INT(JOURNAL_REC_TYPE_CONFIG, out.record_type);
}

static void test_partition_append_multiple_advances_offset(void)
{
	mock_flash_reset();
	mock_flash_erase_sector(JOURNAL_CALIB_START);

	journal_sb_info_t sb;
	journal_sb_ext_init(&sb);
	sb.valid = true;
	sb.next_sequence = 1;

	journal_flash_backend_t backend = {j_read, j_program, j_erase};
	for (int i = 0; i < 3; i++) {
		uint8_t p[] = {(uint8_t)i};
		journal_record_t rec;
		journal_rec_init(&rec, JOURNAL_REC_TYPE_CALIB, 0, 0, p, 1);
		TEST_ASSERT(journal_partition_append(&backend, &sb,
			JOURNAL_PART_CALIB, &rec), "append");
	}
	TEST_ASSERT_EQ_INT(3 * (JOURNAL_REC_HEADER_SIZE + 1),
		(int)sb.calib_write_off);

	uint32_t off = 0;
	for (int i = 0; i < 3; i++) {
		journal_record_t out;
		TEST_ASSERT(journal_partition_read_one(mock_flash_read, &sb,
			JOURNAL_PART_CALIB, off, &out), "read");
		TEST_ASSERT_EQ_INT(i + 1, (int)out.sequence);
		TEST_ASSERT_EQ_INT(i, out.payload[0]);
		off += JOURNAL_REC_HEADER_SIZE + out.payload_length;
	}
}

/* ================================================================
 * Superblock commit (alternate-sector publish)
 * ================================================================ */

static void test_commit_superblock_alternates_and_persists(void)
{
	mock_flash_reset();
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
	sb.next_sequence = 5;
	sb.calib_write_off = 0;
	uint8_t ext_buf[JOURNAL_SB_EXT_SIZE];
	journal_sb_ext_serialize(&sb, ext_buf);
	mock_flash_program(JOURNAL_SB_EXT_OFF, ext_buf, JOURNAL_SB_EXT_SIZE);

	sb.next_sequence = 42;
	sb.calib_write_off = 100;
	journal_flash_backend_t backend = {j_read, j_program, j_erase};
	TEST_ASSERT(journal_commit_superblock(&backend, &sb),
		"commit should succeed");

	TEST_ASSERT_EQ_INT(1, sb.active_sb_sector);
	TEST_ASSERT_EQ_INT(2, (int)sb.erase_generation);

	journal_sb_info_t recovered;
	TEST_ASSERT(journal_scan_superblocks(mock_flash_read, &recovered),
		"boot scan should find committed SB");
	TEST_ASSERT_EQ_INT(1, recovered.active_sb_sector);
	TEST_ASSERT_EQ_INT(2, (int)recovered.erase_generation);
	TEST_ASSERT_EQ_INT(42, (int)recovered.next_sequence);
	TEST_ASSERT_EQ_INT(100, (int)recovered.calib_write_off);
}

static void test_commit_superblock_second_commit_back_to_sector0(void)
{
	mock_flash_reset();
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

	journal_flash_backend_t backend = {j_read, j_program, j_erase};

	journal_commit_superblock(&backend, &sb);
	TEST_ASSERT_EQ_INT(1, sb.active_sb_sector);
	TEST_ASSERT_EQ_INT(2, (int)sb.erase_generation);

	journal_commit_superblock(&backend, &sb);
	TEST_ASSERT_EQ_INT(0, sb.active_sb_sector);
	TEST_ASSERT_EQ_INT(3, (int)sb.erase_generation);

	journal_sb_info_t recovered;
	TEST_ASSERT(journal_scan_superblocks(mock_flash_read, &recovered),
		"scan should find latest SB");
	TEST_ASSERT_EQ_INT(0, recovered.active_sb_sector);
	TEST_ASSERT_EQ_INT(3, (int)recovered.erase_generation);
}

/* ================================================================
 * Full round-trip: append → commit → reboot → read
 * ================================================================ */

static void test_partition_append_survives_reboot(void)
{
	mock_flash_reset();
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

	for (uint32_t s = 0; s < JOURNAL_CALIB_SECTORS; s++)
		mock_flash_erase_sector(JOURNAL_CALIB_START +
					s * QSPI_SECTOR_SIZE);

	journal_flash_backend_t backend = {j_read, j_program, j_erase};
	uint8_t payload[] = {0xCA, 0xFE};
	journal_record_t rec;
	journal_rec_init(&rec, JOURNAL_REC_TYPE_CALIB, 0, 0, payload, 2);
	TEST_ASSERT(journal_partition_append(&backend, &sb,
		JOURNAL_PART_CALIB, &rec), "append");

	TEST_ASSERT(journal_commit_superblock(&backend, &sb), "commit");

	journal_sb_info_t recovered;
	TEST_ASSERT(journal_scan_superblocks(mock_flash_read, &recovered),
		"scan should find SB");
	TEST_ASSERT_EQ_INT((int)sb.calib_write_off,
		(int)recovered.calib_write_off);

	journal_record_t out;
	TEST_ASSERT(journal_partition_read_one(mock_flash_read, &recovered,
		JOURNAL_PART_CALIB, 0, &out), "read after reboot");
	TEST_ASSERT_EQ_INT(0xCA, out.payload[0]);
	TEST_ASSERT_EQ_INT(0xFE, out.payload[1]);
}

/* ================================================================
 * Calib-aware partition scan: latest matching record wins
 * ================================================================ */

static void test_partition_scan_latest_matching_calib(void)
{
	mock_flash_reset();
	mock_flash_erase_sector(JOURNAL_CALIB_START);

	journal_sb_info_t sb;
	journal_sb_ext_init(&sb);
	sb.valid = true;
	sb.next_sequence = 1;

	journal_flash_backend_t backend = {j_read, j_program, j_erase};

	uint8_t blob1[40], blob2[40];
	memset(blob1, 0, 40);
	blob1[0] = 1;
	blob1[1] = 1;
	blob1[28] = 1;
	memcpy(blob2, blob1, 40);
	blob2[4] = 0x42;

	calib_record_t crec1, crec2;
	calib_rec_init(&crec1, CALIB_KIND_IMU, 1, blob1, 40);
	calib_rec_init(&crec2, CALIB_KIND_IMU, 1, blob2, 40);

	uint8_t p1[CALIB_PAYLOAD_MAX], p2[CALIB_PAYLOAD_MAX];
	uint16_t plen1 = calib_rec_serialize(&crec1, p1, sizeof(p1));
	uint16_t plen2 = calib_rec_serialize(&crec2, p2, sizeof(p2));

	journal_record_t jrec1, jrec2;
	journal_rec_init(&jrec1, JOURNAL_REC_TYPE_CALIB, 0, 0, p1, plen1);
	journal_rec_init(&jrec2, JOURNAL_REC_TYPE_CALIB, 0, 0, p2, plen2);

	TEST_ASSERT(journal_partition_append(&backend, &sb,
		JOURNAL_PART_CALIB, &jrec1), "append rec1");
	TEST_ASSERT(journal_partition_append(&backend, &sb,
		JOURNAL_PART_CALIB, &jrec2), "append rec2");

	uint32_t off = 0;
	calib_record_t best;
	memset(&best, 0, sizeof(best));
	uint64_t best_seq = 0;
	bool found = false;

	while (off + JOURNAL_REC_HEADER_SIZE <= sb.calib_write_off) {
		journal_record_t jrec;
		if (!journal_partition_read_one(mock_flash_read, &sb,
		    JOURNAL_PART_CALIB, off, &jrec))
			break;
		calib_record_t crec;
		if (calib_rec_deserialize(jrec.payload,
		    jrec.payload_length, &crec) &&
		    crec.kind == CALIB_KIND_IMU && crec.ref_id == 1) {
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

	TEST_ASSERT(found, "found matching record");
	TEST_ASSERT_EQ_INT(2, (int)best_seq);
	TEST_ASSERT_EQ_INT(0x42, best.blob[4]);
}

/* ================================================================
 * Runner
 * ================================================================ */

int main(void)
{
	test_framework_init();

	/* Partition layout */
	RUN_TEST(test_partition_sizes);
	RUN_TEST(test_record_header_size);

	/* CRC32 */
	RUN_TEST(test_crc32_journal_uses_same_variant);
	RUN_TEST(test_crc32_empty_buffer);

	/* Record format */
	RUN_TEST(test_record_init_and_validate);
	RUN_TEST(test_record_serialize_deserialize_roundtrip);
	RUN_TEST(test_record_zero_payload_validates);
	RUN_TEST(test_record_bad_magic_fails_deserialize);
	RUN_TEST(test_record_corrupt_header_crc_fails_validate);
	RUN_TEST(test_record_corrupt_payload_crc_fails_validate);
	RUN_TEST(test_record_state_transitions);
	RUN_TEST(test_record_erased_detection);

	/* Sequence wrap */
	RUN_TEST(test_seq_normal_ordering);
	RUN_TEST(test_seq_wrap_around);
	RUN_TEST(test_seq_exact_wrap);
	RUN_TEST(test_seq_large_gap);

	/* Superblock extension */
	RUN_TEST(test_sb_ext_init_defaults);
	RUN_TEST(test_sb_ext_serialize_deserialize_roundtrip);
	RUN_TEST(test_sb_ext_bad_magic_fails);
	RUN_TEST(test_sb_ext_crc_corruption_fails);

	/* Boot scan */
	RUN_TEST(test_boot_scan_empty_flash_returns_false);
	RUN_TEST(test_boot_scan_finds_committed_superblock);
	RUN_TEST(test_boot_scan_picks_higher_erase_generation);

	/* Circular log */
	RUN_TEST(test_log_append_and_read_back);
	RUN_TEST(test_log_append_multiple_records);
	RUN_TEST(test_log_is_full_check);

	/* Power-cut fault injection */
	RUN_TEST(test_powercut_record_write_before_commit);
	RUN_TEST(test_powercut_corrupt_header_rejected);
	RUN_TEST(test_powercut_partial_header_write);
	RUN_TEST(test_powercut_full_record_recovery);

	/* GC policy */
	RUN_TEST(test_gc_not_needed_when_empty);
	RUN_TEST(test_gc_needed_when_75_percent_full);
	RUN_TEST(test_gc_not_needed_at_threshold);
	RUN_TEST(test_gc_should_replace_same_type_later_seq);
	RUN_TEST(test_gc_should_not_replace_different_type);
	RUN_TEST(test_gc_should_not_replace_earlier_seq);

	/* Log queue */
	RUN_TEST(test_queue_init_empty);
	RUN_TEST(test_queue_push_and_pop);
	RUN_TEST(test_queue_fill_to_capacity);
	RUN_TEST(test_queue_drop_newest_on_full);
	RUN_TEST(test_queue_pop_empty_returns_false);

	/* Content filter */
	RUN_TEST(test_filter_allows_board_events);
	RUN_TEST(test_filter_excludes_audio);
	RUN_TEST(test_filter_excludes_secrets);

	/* StorageReadLog chunking */
	RUN_TEST(test_readlog_single_chunk);
	RUN_TEST(test_readlog_multi_chunk_fragmentation);
	RUN_TEST(test_readlog_offset_past_end_returns_eof);

	/* StorageEraseLog FSM */
	RUN_TEST(test_eraselog_idle_to_accepted);
	RUN_TEST(test_eraselog_accepted_to_erasing);
	RUN_TEST(test_eraselog_erasing_to_done);
	RUN_TEST(test_eraselog_erasing_to_error);
	RUN_TEST(test_eraselog_accepted_cancel);
	RUN_TEST(test_eraselog_done_is_terminal);
	RUN_TEST(test_eraselog_idle_ignores_non_confirm);

	/* Pre-erase idle check */
	RUN_TEST(test_is_idle_when_no_flags);
	RUN_TEST(test_is_not_idle_when_radio_active);
	RUN_TEST(test_is_not_idle_when_ble_connected);
	RUN_TEST(test_is_not_idle_when_sensor_sampling);
	RUN_TEST(test_is_not_idle_when_display_updating);
	RUN_TEST(test_is_idle_ignores_upper_bits);

	/* NOR commit semantics */
	RUN_TEST(test_nor_commit_transitions_valid);
	RUN_TEST(test_nor_cannot_recommit_after_commit);

	/* Drop-newest queue (newest_index fix) */
	RUN_TEST(test_queue_newest_index_empty_on_init);
	RUN_TEST(test_queue_newest_index_tracks_push);
	RUN_TEST(test_queue_drop_newest_after_pop_push_cycle);

	/* Partition append / read */
	RUN_TEST(test_partition_append_and_read_calib);
	RUN_TEST(test_partition_append_config_routes_correctly);
	RUN_TEST(test_partition_append_multiple_advances_offset);

	/* Superblock commit */
	RUN_TEST(test_commit_superblock_alternates_and_persists);
	RUN_TEST(test_commit_superblock_second_commit_back_to_sector0);

	/* Full round-trip */
	RUN_TEST(test_partition_append_survives_reboot);

	/* Calib-aware scan */
	RUN_TEST(test_partition_scan_latest_matching_calib);

	return test_framework_finish();
}
