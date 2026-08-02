/*
 * test_qspi.cpp - QSPI storage evaluation host tests.
 *
 * Covers: CRC32 known vectors, command trace classification (pre/post
 * adoption), JEDEC RDID validation, nonce lifecycle (all transitions),
 * adoption FSM (all paths, failure always → UNADOPTED), superblock
 * format (init/serialize/deserialize/validate/CRC/golden vector),
 * and power-cut recovery via mock_flash NOR semantics.
 *
 * The power-cut recovery tests simulate interruption at EVERY step of
 * the adoption transaction and verify the device is never left in a
 * partially-formatted ADOPTED state.
 */
#include "test_framework.h"
#include "../../src/storage/qspi_eval.h"
#include "mocks/mock_flash.h"
#include <string.h>

/* qspi_eval.c is pure C — include it directly for test access. */
extern "C" {
#include "../../src/storage/qspi_eval.c"
}

/* ================================================================
 * CRC32 known vectors (ISO-HDLC / zlib)
 * ================================================================ */

static void test_crc32_empty_buffer(void)
{
	TEST_ASSERT_EQ_INT((int)0x00000000u, (int)qspi_crc32((const uint8_t *)"", 0));
}

static void test_crc32_known_vectors(void)
{
	TEST_ASSERT_EQ_INT((int)0xCBF43926u,
		(int)qspi_crc32((const uint8_t *)"123456789", 9));
	TEST_ASSERT_EQ_INT((int)0xC497531Cu,
		(int)qspi_crc32((const uint8_t *)"butterfly", 9));
}

/* ================================================================
 * Pre-adoption command trace classification
 * ================================================================ */

static void test_trace_pre_empty_is_pass(void)
{
	TEST_ASSERT_EQ_INT(QSPI_TRACE_PASS,
		qspi_eval_trace_pre_adopt(NULL, 0));
}

static void test_trace_pre_rdid_only_passes(void)
{
	qspi_cmd_trace_t t[] = {
		{QSPI_OP_JEDEC_ID, false},
		{QSPI_OP_JEDEC_ID, false},
	};
	TEST_ASSERT_EQ_INT(QSPI_TRACE_PASS,
		qspi_eval_trace_pre_adopt(t, 2));
}

static void test_trace_pre_fast_read_fails(void)
{
	qspi_cmd_trace_t t[] = {{QSPI_OP_FAST_READ, false}};
	TEST_ASSERT_EQ_INT(QSPI_TRACE_FAIL,
		qspi_eval_trace_pre_adopt(t, 1));
}

static void test_trace_pre_wren_fails(void)
{
	qspi_cmd_trace_t t[] = {{QSPI_OP_WRITE_ENABLE, true}};
	TEST_ASSERT_EQ_INT(QSPI_TRACE_FAIL,
		qspi_eval_trace_pre_adopt(t, 1));
}

static void test_trace_pre_page_program_fails(void)
{
	qspi_cmd_trace_t t[] = {{QSPI_OP_PAGE_PROGRAM, true}};
	TEST_ASSERT_EQ_INT(QSPI_TRACE_FAIL,
		qspi_eval_trace_pre_adopt(t, 1));
}

static void test_trace_pre_sector_erase_fails(void)
{
	qspi_cmd_trace_t t[] = {{QSPI_OP_SECTOR_ERASE_4K, true}};
	TEST_ASSERT_EQ_INT(QSPI_TRACE_FAIL,
		qspi_eval_trace_pre_adopt(t, 1));
}

static void test_trace_pre_chip_erase_fails(void)
{
	qspi_cmd_trace_t t[] = {{QSPI_OP_CHIP_ERASE, true}};
	TEST_ASSERT_EQ_INT(QSPI_TRACE_FAIL,
		qspi_eval_trace_pre_adopt(t, 1));
}

static void test_trace_pre_write_status_fails(void)
{
	qspi_cmd_trace_t t1[] = {{QSPI_OP_WRITE_STATUS1, true}};
	qspi_cmd_trace_t t2[] = {{QSPI_OP_WRITE_STATUS2, true}};
	TEST_ASSERT_EQ_INT(QSPI_TRACE_FAIL,
		qspi_eval_trace_pre_adopt(t1, 1));
	TEST_ASSERT_EQ_INT(QSPI_TRACE_FAIL,
		qspi_eval_trace_pre_adopt(t2, 1));
}

static void test_trace_pre_rdid_then_wren_fails(void)
{
	qspi_cmd_trace_t t[] = {
		{QSPI_OP_JEDEC_ID, false},
		{QSPI_OP_WRITE_ENABLE, true},
	};
	TEST_ASSERT_EQ_INT(QSPI_TRACE_FAIL,
		qspi_eval_trace_pre_adopt(t, 2));
}

/* ================================================================
 * Post-adoption command trace classification
 * ================================================================ */

static void test_trace_post_empty_is_pass(void)
{
	TEST_ASSERT_EQ_INT(QSPI_TRACE_PASS,
		qspi_eval_trace_post_adopt(NULL, 0));
}

static void test_trace_post_fast_read_passes(void)
{
	qspi_cmd_trace_t t[] = {{QSPI_OP_FAST_READ, false}};
	TEST_ASSERT_EQ_INT(QSPI_TRACE_PASS,
		qspi_eval_trace_post_adopt(t, 1));
}

static void test_trace_post_page_program_passes(void)
{
	qspi_cmd_trace_t t[] = {
		{QSPI_OP_WRITE_ENABLE, true},
		{QSPI_OP_PAGE_PROGRAM, true},
	};
	TEST_ASSERT_EQ_INT(QSPI_TRACE_PASS,
		qspi_eval_trace_post_adopt(t, 2));
}

static void test_trace_post_erase_fails(void)
{
	qspi_cmd_trace_t t[] = {{QSPI_OP_SECTOR_ERASE_4K, true}};
	TEST_ASSERT_EQ_INT(QSPI_TRACE_FAIL,
		qspi_eval_trace_post_adopt(t, 1));
}

static void test_trace_post_chip_erase_fails(void)
{
	qspi_cmd_trace_t t[] = {{QSPI_OP_CHIP_ERASE, true}};
	TEST_ASSERT_EQ_INT(QSPI_TRACE_FAIL,
		qspi_eval_trace_post_adopt(t, 1));
}

static void test_trace_post_write_status_fails(void)
{
	qspi_cmd_trace_t t[] = {{QSPI_OP_WRITE_STATUS1, true}};
	TEST_ASSERT_EQ_INT(QSPI_TRACE_FAIL,
		qspi_eval_trace_post_adopt(t, 1));
}

/* ================================================================
 * JEDEC ID validation
 * ================================================================ */

static void test_jedec_match_correct_id(void)
{
	uint8_t id[3] = {QSPI_JEDEC_MANUFACTURER, QSPI_JEDEC_TYPE, QSPI_JEDEC_CAPACITY};
	TEST_ASSERT(qspi_eval_jedec_match(id), "C8 40 15 should match");
}

static void test_jedec_mismatch_wrong_manufacturer(void)
{
	uint8_t id[3] = {0xEF, QSPI_JEDEC_TYPE, QSPI_JEDEC_CAPACITY};
	TEST_ASSERT(!qspi_eval_jedec_match(id), "EF 40 15 should NOT match");
}

static void test_jedec_mismatch_wrong_capacity(void)
{
	uint8_t id[3] = {QSPI_JEDEC_MANUFACTURER, QSPI_JEDEC_TYPE, 0x16};
	TEST_ASSERT(!qspi_eval_jedec_match(id), "C8 40 16 should NOT match");
}

/* ================================================================
 * Nonce lifecycle FSM
 * ================================================================ */

static void test_nonce_init_is_empty(void)
{
	qspi_nonce_tracker_t t;
	qspi_eval_nonce_init(&t);
	TEST_ASSERT_EQ_INT(QSPI_NONCE_EMPTY, t.state);
	TEST_ASSERT(!qspi_eval_nonce_is_valid(&t, 0), "empty nonce not valid");
}

static void test_nonce_issue_makes_valid(void)
{
	qspi_nonce_tracker_t t;
	qspi_eval_nonce_init(&t);
	uint8_t nonce[16] = {1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16};
	qspi_eval_nonce_issue(&t, nonce, 1000);
	TEST_ASSERT_EQ_INT(QSPI_NONCE_VALID, t.state);
	TEST_ASSERT(qspi_eval_nonce_is_valid(&t, 1001), "nonce valid right after issue");
}

static void test_nonce_correct_match_consumes(void)
{
	qspi_nonce_tracker_t t;
	qspi_eval_nonce_init(&t);
	uint8_t nonce[16] = {0xAA};
	qspi_eval_nonce_issue(&t, nonce, 1000);
	TEST_ASSERT_EQ_INT(QSPI_NONCE_MATCH_OK,
		qspi_eval_nonce_check(&t, nonce, 16, 2000));
	TEST_ASSERT_EQ_INT(QSPI_NONCE_CONSUMED, t.state);
}

static void test_nonce_double_use_rejected(void)
{
	qspi_nonce_tracker_t t;
	qspi_eval_nonce_init(&t);
	uint8_t nonce[16] = {0xBB};
	qspi_eval_nonce_issue(&t, nonce, 1000);
	qspi_eval_nonce_check(&t, nonce, 16, 2000);
	TEST_ASSERT_EQ_INT(QSPI_NONCE_ALREADY_USED,
		qspi_eval_nonce_check(&t, nonce, 16, 3000));
}

static void test_nonce_wrong_bytes_no_match(void)
{
	qspi_nonce_tracker_t t;
	qspi_eval_nonce_init(&t);
	uint8_t nonce[16] = {0xCC};
	uint8_t wrong[16] = {0xDD};
	qspi_eval_nonce_issue(&t, nonce, 1000);
	TEST_ASSERT_EQ_INT(QSPI_NONCE_NO_MATCH,
		qspi_eval_nonce_check(&t, wrong, 16, 2000));
	TEST_ASSERT_EQ_INT(QSPI_NONCE_VALID, t.state);
}

static void test_nonce_expired_after_60s(void)
{
	qspi_nonce_tracker_t t;
	qspi_eval_nonce_init(&t);
	uint8_t nonce[16] = {0xEE};
	qspi_eval_nonce_issue(&t, nonce, 1000);
	uint64_t expiry = 1000 + QSPI_NONCE_TTL_US + 1;
	TEST_ASSERT_EQ_INT(QSPI_NONCE_RESULT_EXPIRED,
		qspi_eval_nonce_check(&t, nonce, 16, expiry));
	TEST_ASSERT_EQ_INT(QSPI_NONCE_EXPIRED, t.state);
}

static void test_nonce_just_before_60s_ok(void)
{
	qspi_nonce_tracker_t t;
	qspi_eval_nonce_init(&t);
	uint8_t nonce[16] = {0x11};
	qspi_eval_nonce_issue(&t, nonce, 5000);
	uint64_t just_before = 5000 + QSPI_NONCE_TTL_US;
	TEST_ASSERT_EQ_INT(QSPI_NONCE_MATCH_OK,
		qspi_eval_nonce_check(&t, nonce, 16, just_before));
}

static void test_nonce_new_request_invalidates_old(void)
{
	qspi_nonce_tracker_t t;
	qspi_eval_nonce_init(&t);
	uint8_t n1[16] = {1};
	uint8_t n2[16] = {2};
	qspi_eval_nonce_issue(&t, n1, 1000);
	qspi_eval_nonce_issue(&t, n2, 2000);
	TEST_ASSERT_EQ_INT(QSPI_NONCE_NO_MATCH,
		qspi_eval_nonce_check(&t, n1, 16, 3000));
	TEST_ASSERT_EQ_INT(QSPI_NONCE_MATCH_OK,
		qspi_eval_nonce_check(&t, n2, 16, 3000));
}

static void test_nonce_force_invalidate(void)
{
	qspi_nonce_tracker_t t;
	qspi_eval_nonce_init(&t);
	uint8_t nonce[16] = {0x22};
	qspi_eval_nonce_issue(&t, nonce, 1000);
	qspi_eval_nonce_invalidate(&t);
	TEST_ASSERT_EQ_INT(QSPI_NONCE_INVALIDATED, t.state);
	TEST_ASSERT_EQ_INT(QSPI_NONCE_NO_MATCH,
		qspi_eval_nonce_check(&t, nonce, 16, 2000));
}

static void test_nonce_check_when_empty(void)
{
	qspi_nonce_tracker_t t;
	qspi_eval_nonce_init(&t);
	uint8_t nonce[16] = {0};
	TEST_ASSERT_EQ_INT(QSPI_NONCE_NONE_ISSUED,
		qspi_eval_nonce_check(&t, nonce, 16, 1000));
}

static void test_nonce_wrong_length_no_match(void)
{
	qspi_nonce_tracker_t t;
	qspi_eval_nonce_init(&t);
	uint8_t nonce[16] = {0x33};
	qspi_eval_nonce_issue(&t, nonce, 1000);
	TEST_ASSERT_EQ_INT(QSPI_NONCE_NO_MATCH,
		qspi_eval_nonce_check(&t, nonce, 15, 2000));
}

/* ================================================================
 * Adoption state machine
 * ================================================================ */

static void test_adopt_happy_path_full_sequence(void)
{
	qspi_state_t s = QSPI_ST_UNADOPTED;
	s = qspi_eval_adopt_transition(s, QSPI_EVT_PROBE_OK);
	TEST_ASSERT_EQ_INT(QSPI_ST_PROBING, s);
	s = qspi_eval_adopt_transition(s, QSPI_EVT_ID_MATCH);
	TEST_ASSERT_EQ_INT(QSPI_ST_VALIDATED, s);
	s = qspi_eval_adopt_transition(s, QSPI_EVT_NONCE_ISSUED);
	TEST_ASSERT_EQ_INT(QSPI_ST_NONCE_ISSUED, s);
	s = qspi_eval_adopt_transition(s, QSPI_EVT_CONFIRMED);
	TEST_ASSERT_EQ_INT(QSPI_ST_CONFIRMED, s);
	s = qspi_eval_adopt_transition(s, QSPI_EVT_ERASE_SB_OK);
	TEST_ASSERT_EQ_INT(QSPI_ST_ERASING_SUPERBLOCK, s);
	s = qspi_eval_adopt_transition(s, QSPI_EVT_ERASE_REM_OK);
	TEST_ASSERT_EQ_INT(QSPI_ST_ERASING_REMAINING, s);
	s = qspi_eval_adopt_transition(s, QSPI_EVT_VERIFY_OK);
	TEST_ASSERT_EQ_INT(QSPI_ST_VERIFYING, s);
	s = qspi_eval_adopt_transition(s, QSPI_EVT_WRITE_SB_OK);
	TEST_ASSERT_EQ_INT(QSPI_ST_WRITING_SUPERBLOCK, s);
	s = qspi_eval_adopt_transition(s, QSPI_EVT_COMMIT_OK);
	TEST_ASSERT_EQ_INT(QSPI_ST_COMMITTING, s);
	s = qspi_eval_adopt_transition(s, QSPI_EVT_COMMIT_OK);
	TEST_ASSERT_EQ_INT(QSPI_ST_ADOPTED, s);
	TEST_ASSERT(qspi_eval_state_is_adopted(s), "final state should be adopted");
}

static void test_adopt_probe_fail_returns_unadopted(void)
{
	qspi_state_t s = QSPI_ST_PROBING;
	s = qspi_eval_adopt_transition(s, QSPI_EVT_ID_MISMATCH);
	TEST_ASSERT_EQ_INT(QSPI_ST_UNADOPTED, s);
}

static void test_adopt_confirm_fail_returns_unadopted(void)
{
	qspi_state_t s = QSPI_ST_NONCE_ISSUED;
	s = qspi_eval_adopt_transition(s, QSPI_EVT_CONFIRM_FAIL);
	TEST_ASSERT_EQ_INT(QSPI_ST_UNADOPTED, s);
}

static void test_adopt_erase_sb_fail_returns_unadopted(void)
{
	qspi_state_t s = QSPI_ST_CONFIRMED;
	s = qspi_eval_adopt_transition(s, QSPI_EVT_ERASE_SB_FAIL);
	TEST_ASSERT_EQ_INT(QSPI_ST_UNADOPTED, s);
}

static void test_adopt_erase_rem_fail_returns_unadopted(void)
{
	qspi_state_t s = QSPI_ST_ERASING_SUPERBLOCK;
	s = qspi_eval_adopt_transition(s, QSPI_EVT_ERASE_REM_FAIL);
	TEST_ASSERT_EQ_INT(QSPI_ST_UNADOPTED, s);
}

static void test_adopt_verify_fail_returns_unadopted(void)
{
	qspi_state_t s = QSPI_ST_ERASING_REMAINING;
	s = qspi_eval_adopt_transition(s, QSPI_EVT_VERIFY_FAIL);
	TEST_ASSERT_EQ_INT(QSPI_ST_UNADOPTED, s);
}

static void test_adopt_write_sb_fail_returns_unadopted(void)
{
	qspi_state_t s = QSPI_ST_VERIFYING;
	s = qspi_eval_adopt_transition(s, QSPI_EVT_WRITE_SB_FAIL);
	TEST_ASSERT_EQ_INT(QSPI_ST_UNADOPTED, s);
}

static void test_adopt_reset_from_validated(void)
{
	qspi_state_t s = QSPI_ST_VALIDATED;
	s = qspi_eval_adopt_transition(s, QSPI_EVT_RESET);
	TEST_ASSERT_EQ_INT(QSPI_ST_UNADOPTED, s);
}

static void test_adopt_reset_from_nonce_issued(void)
{
	qspi_state_t s = QSPI_ST_NONCE_ISSUED;
	s = qspi_eval_adopt_transition(s, QSPI_EVT_RESET);
	TEST_ASSERT_EQ_INT(QSPI_ST_UNADOPTED, s);
}

static void test_adopt_adopted_is_sticky(void)
{
	qspi_state_t s = QSPI_ST_ADOPTED;
	for (int e = 0; e <= (int)QSPI_EVT_RESET; e++) {
		s = qspi_eval_adopt_transition(QSPI_ST_ADOPTED,
			(qspi_event_t)e);
		TEST_ASSERT_EQ_INT(QSPI_ST_ADOPTED, s);
	}
}

static void test_adopt_unadopted_ignores_wrong_events(void)
{
	qspi_state_t s = QSPI_ST_UNADOPTED;
	s = qspi_eval_adopt_transition(s, QSPI_EVT_ID_MATCH);
	TEST_ASSERT_EQ_INT(QSPI_ST_UNADOPTED, s);
}

/* ================================================================
 * Superblock format
 * ================================================================ */

static void test_sb_init_correct_values(void)
{
	qspi_superblock_t sb;
	qspi_eval_sb_init(&sb);
	TEST_ASSERT_EQ_INT('B', sb.magic[0]);
	TEST_ASSERT_EQ_INT('T', sb.magic[1]);
	TEST_ASSERT_EQ_INT('F', sb.magic[2]);
	TEST_ASSERT_EQ_INT('Q', sb.magic[3]);
	TEST_ASSERT_EQ_INT(QSPI_SB_VERSION, sb.version);
	TEST_ASSERT_EQ_INT(QSPI_JEDEC_MANUFACTURER, sb.jedec_id[0]);
	TEST_ASSERT_EQ_INT(QSPI_FLASH_SIZE_BYTES, (int)sb.capacity_bytes);
	TEST_ASSERT_EQ_INT(QSPI_SECTOR_SIZE, sb.sector_size);
	TEST_ASSERT_EQ_INT(QSPI_SECTOR_COUNT, (int)sb.sector_count);
	TEST_ASSERT_EQ_INT(QSPI_SB_COMMIT_ERASED, sb.commit_marker);
}

static void test_sb_serialize_deserialize_roundtrip(void)
{
	qspi_superblock_t sb;
	qspi_eval_sb_init(&sb);
	sb.crc32 = qspi_eval_sb_compute_crc(&sb);

	uint8_t buf[QSPI_SB_HEADER_SIZE];
	qspi_eval_sb_serialize(&sb, buf);

	qspi_superblock_t sb2;
	TEST_ASSERT(qspi_eval_sb_deserialize(buf, &sb2), "roundtrip deserialize should succeed");
	TEST_ASSERT_EQ_INT(sb.version, sb2.version);
	TEST_ASSERT_EQ_INT((int)sb.capacity_bytes, (int)sb2.capacity_bytes);
	TEST_ASSERT_EQ_INT(sb.sector_size, sb2.sector_size);
	TEST_ASSERT_EQ_INT((int)sb.sector_count, (int)sb2.sector_count);
	TEST_ASSERT_EQ_INT((int)sb.crc32, (int)sb2.crc32);
	TEST_ASSERT_EQ_INT(sb.commit_marker, sb2.commit_marker);
}

static void test_sb_validate_correct(void)
{
	qspi_superblock_t sb;
	qspi_eval_sb_init(&sb);
	sb.crc32 = qspi_eval_sb_compute_crc(&sb);
	TEST_ASSERT(qspi_eval_sb_validate(&sb), "valid superblock should validate");
}

static void test_sb_validate_wrong_crc_fails(void)
{
	qspi_superblock_t sb;
	qspi_eval_sb_init(&sb);
	sb.crc32 = 0xDEADBEEF;
	TEST_ASSERT(!qspi_eval_sb_validate(&sb), "wrong CRC should fail");
}

static void test_sb_validate_wrong_magic_fails(void)
{
	qspi_superblock_t sb;
	qspi_eval_sb_init(&sb);
	sb.magic[0] = 'X';
	sb.crc32 = qspi_eval_sb_compute_crc(&sb);
	TEST_ASSERT(!qspi_eval_sb_validate(&sb), "wrong magic should fail");
}

static void test_sb_validate_wrong_jedec_fails(void)
{
	qspi_superblock_t sb;
	qspi_eval_sb_init(&sb);
	sb.jedec_id[0] = 0xEF;
	sb.crc32 = qspi_eval_sb_compute_crc(&sb);
	TEST_ASSERT(!qspi_eval_sb_validate(&sb), "wrong JEDEC should fail");
}

static void test_sb_deserialize_bad_magic_fails(void)
{
	uint8_t buf[QSPI_SB_HEADER_SIZE];
	memset(buf, 0xFF, sizeof(buf));
	qspi_superblock_t sb;
	TEST_ASSERT(!qspi_eval_sb_deserialize(buf, &sb),
		"erased flash should not deserialize");
}

static void test_sb_not_committed_by_default(void)
{
	qspi_superblock_t sb;
	qspi_eval_sb_init(&sb);
	TEST_ASSERT(!qspi_eval_sb_is_committed(&sb),
		"fresh superblock should NOT be committed");
}

static void test_sb_committed_after_set(void)
{
	qspi_superblock_t sb;
	qspi_eval_sb_init(&sb);
	sb.commit_marker = QSPI_SB_COMMIT_VALUE;
	TEST_ASSERT(qspi_eval_sb_is_committed(&sb),
		"superblock with commit marker should be committed");
}

static void test_sb_buf_commit_check(void)
{
	uint8_t buf[QSPI_SB_HEADER_SIZE];
	memset(buf, 0xFF, sizeof(buf));
	TEST_ASSERT(!qspi_eval_sb_buf_is_committed(buf),
		"0xFF commit byte should not be committed");
	buf[QSPI_SB_COMMIT_OFFSET] = QSPI_SB_COMMIT_VALUE;
	TEST_ASSERT(qspi_eval_sb_buf_is_committed(buf),
		"0xC0 commit byte should be committed");
}

/* ================================================================
 * Golden vector: frozen initial superblock bytes
 * ================================================================ */

static void test_golden_vector_initial_superblock(void)
{
	qspi_superblock_t sb;
	qspi_eval_sb_init(&sb);
	sb.crc32 = qspi_eval_sb_compute_crc(&sb);

	uint8_t golden[QSPI_SB_HEADER_SIZE];
	qspi_eval_sb_serialize(&sb, golden);

	/* Magic */
	TEST_ASSERT_EQ_INT('B', golden[0]);
	TEST_ASSERT_EQ_INT('T', golden[1]);
	TEST_ASSERT_EQ_INT('F', golden[2]);
	TEST_ASSERT_EQ_INT('Q', golden[3]);
	/* Version 1 LE */
	TEST_ASSERT_EQ_INT(1, golden[4]);
	TEST_ASSERT_EQ_INT(0, golden[5]);
	/* JEDEC C8 40 15 */
	TEST_ASSERT_EQ_INT(0xC8, golden[6]);
	TEST_ASSERT_EQ_INT(0x40, golden[7]);
	TEST_ASSERT_EQ_INT(0x15, golden[8]);
	/* Flags = 0 */
	TEST_ASSERT_EQ_INT(0, golden[9]);
	/* Capacity 0x200000 LE */
	TEST_ASSERT_EQ_INT(0x00, golden[10]);
	TEST_ASSERT_EQ_INT(0x00, golden[11]);
	TEST_ASSERT_EQ_INT(0x20, golden[12]);
	TEST_ASSERT_EQ_INT(0x00, golden[13]);
	/* Sector size 0x1000 LE (u16) */
	TEST_ASSERT_EQ_INT(0x00, golden[14]);
	TEST_ASSERT_EQ_INT(0x10, golden[15]);
	/* Sector count 0x200 LE (u32) */
	TEST_ASSERT_EQ_INT(0x00, golden[16]);
	TEST_ASSERT_EQ_INT(0x02, golden[17]);
	TEST_ASSERT_EQ_INT(0x00, golden[18]);
	TEST_ASSERT_EQ_INT(0x00, golden[19]);
	/* Reserved = 0xFF */
	TEST_ASSERT_EQ_INT(0xFF, golden[20]);
	TEST_ASSERT_EQ_INT(0xFF, golden[23]);
	/* Commit = 0xFF (uncommitted) */
	TEST_ASSERT_EQ_INT(QSPI_SB_COMMIT_ERASED, golden[QSPI_SB_COMMIT_OFFSET]);
	/* CRC matches re-computation */
	qspi_superblock_t rt;
	qspi_eval_sb_deserialize(golden, &rt);
	TEST_ASSERT_EQ_INT((int)sb.crc32, (int)rt.crc32);
	TEST_ASSERT(qspi_eval_sb_validate(&rt), "golden roundtrip validates");
}

/* ================================================================
 * Power-cut recovery via mock_flash NOR semantics
 * ================================================================
 *
 * Simulates the adoption transaction step by step, interrupting at
 * each point. The critical invariant: device must NEVER be left
 * in a partially-formatted ADOPTED state. Only a verified commit
 * marker makes the device ADOPTED.
 */

static void test_powercut_after_erase_superblock(void)
{
	mock_flash_reset();
	/* Simulate: erase superblock sector 0, then power-cut. */
	TEST_ASSERT_EQ_INT(0, mock_flash_erase_sector(0));
	/* No superblock written → commit byte at 0xFF → not committed */
	uint8_t buf[QSPI_SB_HEADER_SIZE];
	mock_flash_read(0, buf, sizeof(buf));
	TEST_ASSERT(!qspi_eval_sb_buf_is_committed(buf),
		"power-cut after erase should leave device unadopted");
	TEST_ASSERT_EQ_INT(QSPI_ST_UNADOPTED,
		qspi_eval_adopt_transition(QSPI_ST_ERASING_SUPERBLOCK, QSPI_EVT_RESET));
}

static void test_powercut_after_header_written_before_commit(void)
{
	mock_flash_reset();
	mock_flash_erase_sector(0);

	qspi_superblock_t sb;
	qspi_eval_sb_init(&sb);
	sb.crc32 = qspi_eval_sb_compute_crc(&sb);

	uint8_t buf[QSPI_SB_HEADER_SIZE];
	qspi_eval_sb_serialize(&sb, buf);

	/* Write header bytes 0-27 (NOT the commit byte at 28). */
	TEST_ASSERT_EQ_INT(0,
		mock_flash_program(0, buf, QSPI_SB_COMMIT_OFFSET));

	/* Power-cut: commit byte still 0xFF */
	uint8_t readback[QSPI_SB_HEADER_SIZE];
	mock_flash_read(0, readback, sizeof(readback));
	TEST_ASSERT(!qspi_eval_sb_buf_is_committed(readback),
		"power-cut before commit should NOT be adopted");
}

static void test_powercut_after_commit_is_adopted(void)
{
	mock_flash_reset();
	mock_flash_erase_sector(0);

	qspi_superblock_t sb;
	qspi_eval_sb_init(&sb);
	sb.crc32 = qspi_eval_sb_compute_crc(&sb);

	uint8_t buf[QSPI_SB_HEADER_SIZE];
	qspi_eval_sb_serialize(&sb, buf);

	/* Write header bytes 0-27. */
	mock_flash_program(0, buf, QSPI_SB_COMMIT_OFFSET);
	/* Write commit byte LAST. */
	uint8_t commit = QSPI_SB_COMMIT_VALUE;
	mock_flash_program(QSPI_SB_COMMIT_OFFSET, &commit, 1);

	uint8_t readback[QSPI_SB_HEADER_SIZE];
	mock_flash_read(0, readback, sizeof(readback));
	TEST_ASSERT(qspi_eval_sb_buf_is_committed(readback),
		"after commit write, device should be adopted");

	qspi_superblock_t rt;
	TEST_ASSERT(qspi_eval_sb_deserialize(readback, &rt), "deserialize committed superblock");
	TEST_ASSERT(qspi_eval_sb_validate(&rt), "committed superblock validates");
	TEST_ASSERT(qspi_eval_sb_is_committed(&rt), "committed marker set");
}

static void test_powercut_readoption_never_partial_adopted(void)
{
	/* Exhaustive: interrupt at EVERY state from CONFIRMED onwards. */
	qspi_state_t states[] = {
		QSPI_ST_CONFIRMED,
		QSPI_ST_ERASING_SUPERBLOCK,
		QSPI_ST_ERASING_REMAINING,
		QSPI_ST_VERIFYING,
		QSPI_ST_WRITING_SUPERBLOCK,
		QSPI_ST_COMMITTING,
	};
	for (size_t i = 0; i < sizeof(states)/sizeof(states[0]); i++) {
		qspi_state_t after_reset =
			qspi_eval_adopt_transition(states[i], QSPI_EVT_RESET);
		TEST_ASSERT_EQ_INT(QSPI_ST_UNADOPTED, after_reset);
	}
}

static void test_powercut_readopt_after_partial_write(void)
{
	/* Scenario: device was previously adopted, user re-adopts.
	 * Step 1: write a committed superblock (old). */
	mock_flash_reset();
	mock_flash_erase_sector(0);
	qspi_superblock_t sb;
	qspi_eval_sb_init(&sb);
	sb.crc32 = qspi_eval_sb_compute_crc(&sb);
	uint8_t buf[QSPI_SB_HEADER_SIZE];
	qspi_eval_sb_serialize(&sb, buf);
	mock_flash_program(0, buf, QSPI_SB_COMMIT_OFFSET);
	uint8_t commit = QSPI_SB_COMMIT_VALUE;
	mock_flash_program(QSPI_SB_COMMIT_OFFSET, &commit, 1);

	/* Verify old state is committed. */
	uint8_t check[QSPI_SB_HEADER_SIZE];
	mock_flash_read(0, check, sizeof(check));
	TEST_ASSERT(qspi_eval_sb_buf_is_committed(check),
		"old committed superblock should be detected");

	/* Step 2: start new adoption — erase sector 0 (invalidate old marker). */
	mock_flash_erase_sector(0);
	mock_flash_read(0, check, sizeof(check));
	TEST_ASSERT(!qspi_eval_sb_buf_is_committed(check),
		"after erase, old commit marker must be gone");

	/* Step 3: power-cut before writing new superblock → UNADOPTED. */
	TEST_ASSERT(!qspi_eval_sb_buf_is_committed(check),
		"uncommitted device should not be adopted");
}

static void test_nor_program_only_clears_bits(void)
{
	mock_flash_reset();
	mock_flash_erase_sector(0);

	/* Program 0xC0 at commit offset should succeed (0xFF → 0xC0). */
	uint8_t val = 0xC0;
	TEST_ASSERT_EQ_INT(0, mock_flash_program(QSPI_SB_COMMIT_OFFSET, &val, 1));

	/* Attempting to program 0xFF over 0xC0 should FAIL (can't set bits). */
	uint8_t ff = 0xFF;
	TEST_ASSERT_EQ_INT(-1, mock_flash_program(QSPI_SB_COMMIT_OFFSET, &ff, 1));
}

static void test_full_adoption_transaction_on_mock(void)
{
	/* Complete adoption transaction on mock_flash. */
	mock_flash_reset();

	/* (1) Erase+verify both superblock sectors. */
	for (uint32_t s = 0; s < QSPI_SUPERBLOCK_SECTORS; s++) {
		TEST_ASSERT_EQ_INT(0, mock_flash_erase_sector(s * QSPI_SECTOR_SIZE));
	}

	/* (2) Erase+verify remaining sectors. */
	for (uint32_t s = QSPI_SUPERBLOCK_SECTORS; s < QSPI_SECTOR_COUNT; s++) {
		TEST_ASSERT_EQ_INT(0, mock_flash_erase_sector(s * QSPI_SECTOR_SIZE));
	}

	/* (3) Verify whole 2MiB device is erased. */
	uint8_t verify_buf[256];
	for (uint32_t off = 0; off < QSPI_FLASH_SIZE_BYTES; off += sizeof(verify_buf)) {
		mock_flash_read(off, verify_buf, sizeof(verify_buf));
		for (size_t i = 0; i < sizeof(verify_buf); i++) {
			TEST_ASSERT_EQ_INT(0xFF, verify_buf[i]);
		}
	}

	/* (4) Write+verify initial superblock + commit marker LAST. */
	qspi_superblock_t sb;
	qspi_eval_sb_init(&sb);
	sb.crc32 = qspi_eval_sb_compute_crc(&sb);
	uint8_t buf[QSPI_SB_HEADER_SIZE];
	qspi_eval_sb_serialize(&sb, buf);

	mock_flash_program(0, buf, QSPI_SB_COMMIT_OFFSET);
	uint8_t commit = QSPI_SB_COMMIT_VALUE;
	mock_flash_program(QSPI_SB_COMMIT_OFFSET, &commit, 1);

	/* Verify. */
	uint8_t readback[QSPI_SB_HEADER_SIZE];
	mock_flash_read(0, readback, sizeof(readback));
	TEST_ASSERT(qspi_eval_sb_buf_is_committed(readback), "full adoption should commit");
	qspi_superblock_t rt;
	TEST_ASSERT(qspi_eval_sb_deserialize(readback, &rt), "deserialize after full adoption");
	TEST_ASSERT(qspi_eval_sb_validate(&rt), "full adoption superblock validates");
}

/* ================================================================
 * Hardware constants sanity
 * ================================================================ */

static void test_jedec_opcode_is_0x9f_not_fast_read(void)
{
	TEST_ASSERT_EQ_INT(0x9F, QSPI_OP_JEDEC_ID);
	TEST_ASSERT(QSPI_OP_JEDEC_ID != QSPI_OP_FAST_READ, "RDID 0x9F must not equal fast-read 0x0B");
}

static void test_flash_size_constants(void)
{
	TEST_ASSERT_EQ_INT(2u * 1024u * 1024u, QSPI_FLASH_SIZE_BYTES);
	TEST_ASSERT_EQ_INT(4096, QSPI_SECTOR_SIZE);
	TEST_ASSERT_EQ_INT(512, QSPI_SECTOR_COUNT);
	TEST_ASSERT_EQ_INT(32, QSPI_SB_HEADER_SIZE);
	TEST_ASSERT_EQ_INT(2, QSPI_SUPERBLOCK_SECTORS);
}

/* ================================================================
 * Runner
 * ================================================================ */

int main(void)
{
	test_framework_init();

	/* CRC32 */
	RUN_TEST(test_crc32_empty_buffer);
	RUN_TEST(test_crc32_known_vectors);

	/* Pre-adoption trace */
	RUN_TEST(test_trace_pre_empty_is_pass);
	RUN_TEST(test_trace_pre_rdid_only_passes);
	RUN_TEST(test_trace_pre_fast_read_fails);
	RUN_TEST(test_trace_pre_wren_fails);
	RUN_TEST(test_trace_pre_page_program_fails);
	RUN_TEST(test_trace_pre_sector_erase_fails);
	RUN_TEST(test_trace_pre_chip_erase_fails);
	RUN_TEST(test_trace_pre_write_status_fails);
	RUN_TEST(test_trace_pre_rdid_then_wren_fails);

	/* Post-adoption trace */
	RUN_TEST(test_trace_post_empty_is_pass);
	RUN_TEST(test_trace_post_fast_read_passes);
	RUN_TEST(test_trace_post_page_program_passes);
	RUN_TEST(test_trace_post_erase_fails);
	RUN_TEST(test_trace_post_chip_erase_fails);
	RUN_TEST(test_trace_post_write_status_fails);

	/* JEDEC */
	RUN_TEST(test_jedec_match_correct_id);
	RUN_TEST(test_jedec_mismatch_wrong_manufacturer);
	RUN_TEST(test_jedec_mismatch_wrong_capacity);

	/* Nonce lifecycle */
	RUN_TEST(test_nonce_init_is_empty);
	RUN_TEST(test_nonce_issue_makes_valid);
	RUN_TEST(test_nonce_correct_match_consumes);
	RUN_TEST(test_nonce_double_use_rejected);
	RUN_TEST(test_nonce_wrong_bytes_no_match);
	RUN_TEST(test_nonce_expired_after_60s);
	RUN_TEST(test_nonce_just_before_60s_ok);
	RUN_TEST(test_nonce_new_request_invalidates_old);
	RUN_TEST(test_nonce_force_invalidate);
	RUN_TEST(test_nonce_check_when_empty);
	RUN_TEST(test_nonce_wrong_length_no_match);

	/* Adoption FSM */
	RUN_TEST(test_adopt_happy_path_full_sequence);
	RUN_TEST(test_adopt_probe_fail_returns_unadopted);
	RUN_TEST(test_adopt_confirm_fail_returns_unadopted);
	RUN_TEST(test_adopt_erase_sb_fail_returns_unadopted);
	RUN_TEST(test_adopt_erase_rem_fail_returns_unadopted);
	RUN_TEST(test_adopt_verify_fail_returns_unadopted);
	RUN_TEST(test_adopt_write_sb_fail_returns_unadopted);
	RUN_TEST(test_adopt_reset_from_validated);
	RUN_TEST(test_adopt_reset_from_nonce_issued);
	RUN_TEST(test_adopt_adopted_is_sticky);
	RUN_TEST(test_adopt_unadopted_ignores_wrong_events);

	/* Superblock format */
	RUN_TEST(test_sb_init_correct_values);
	RUN_TEST(test_sb_serialize_deserialize_roundtrip);
	RUN_TEST(test_sb_validate_correct);
	RUN_TEST(test_sb_validate_wrong_crc_fails);
	RUN_TEST(test_sb_validate_wrong_magic_fails);
	RUN_TEST(test_sb_validate_wrong_jedec_fails);
	RUN_TEST(test_sb_deserialize_bad_magic_fails);
	RUN_TEST(test_sb_not_committed_by_default);
	RUN_TEST(test_sb_committed_after_set);
	RUN_TEST(test_sb_buf_commit_check);

	/* Golden vector */
	RUN_TEST(test_golden_vector_initial_superblock);

	/* Power-cut recovery */
	RUN_TEST(test_powercut_after_erase_superblock);
	RUN_TEST(test_powercut_after_header_written_before_commit);
	RUN_TEST(test_powercut_after_commit_is_adopted);
	RUN_TEST(test_powercut_readoption_never_partial_adopted);
	RUN_TEST(test_powercut_readopt_after_partial_write);
	RUN_TEST(test_nor_program_only_clears_bits);
	RUN_TEST(test_full_adoption_transaction_on_mock);

	/* Constants */
	RUN_TEST(test_jedec_opcode_is_0x9f_not_fast_read);
	RUN_TEST(test_flash_size_constants);

	return test_framework_finish();
}
