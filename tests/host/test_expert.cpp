/*
 * test_expert.cpp - GPIO/SAADC expert API evaluation host tests.
 *
 * Covers: pin alias table (8 entries, non-trivial A-to-AIN mapping),
 * GPIO config validation (all enum ranges), SAADC mV conversion
 * (known values + saturation clamp), 4x oversampling, lease lifecycle
 * (issue, validate, release, idempotent, stale, cross-session,
 * wrong-owner, table-full, non-analog rejection).
 *
 * No hardware required. All logic is in expert_eval.c.
 */
#include "test_framework.h"

#include "../../src/expert/expert_eval.h"

#include "whad/protocol/board/board.pb.h"

#include <string.h>

/* ================================================================
 * Alias table — all 8 entries, every lookup direction
 * ================================================================ */

static void test_alias_table_size(void)
{
	TEST_ASSERT_EQ_INT(8, EXPERT_ALIAS_COUNT);
}

static void test_alias_all_d_to_ain(void)
{
	uint8_t ain = 0xFF;
	TEST_ASSERT(expert_eval_d_to_ain(0, &ain), "D0 maps to AIN2");
	TEST_ASSERT_EQ_INT(2, ain);

	ain = 0xFF;
	TEST_ASSERT(expert_eval_d_to_ain(4, &ain), "D4 maps to AIN0");
	TEST_ASSERT_EQ_INT(0, ain);

	ain = 0xFF;
	TEST_ASSERT(expert_eval_d_to_ain(12, &ain), "D12 maps to AIN7");
	TEST_ASSERT_EQ_INT(7, ain);
}

static void test_alias_non_trivial_a_to_ain(void)
{
	/* A0 -> AIN7, NOT AIN0. Never infer AIN from A-number. */
	const expert_alias_t *a0 = expert_eval_alias_by_a(0);
	TEST_ASSERT(a0 != NULL, "A0 alias exists");
	TEST_ASSERT_EQ_INT(7, a0->ain);

	const expert_alias_t *a1 = expert_eval_alias_by_a(1);
	TEST_ASSERT(a1 != NULL, "A1 alias exists");
	TEST_ASSERT_EQ_INT(5, a1->ain);

	const expert_alias_t *a6 = expert_eval_alias_by_a(6);
	TEST_ASSERT(a6 != NULL, "A6 alias exists");
	TEST_ASSERT_EQ_INT(0, a6->ain);

	const expert_alias_t *a7 = expert_eval_alias_by_a(7);
	TEST_ASSERT(a7 != NULL, "A7 alias exists");
	TEST_ASSERT_EQ_INT(6, a7->ain);
}

static void test_alias_d3_a5_is_ain4(void)
{
	/* D3/A5 P0.28 -> AIN4. NOT inferred from A5 -> AIN5. */
	const expert_alias_t *alias = expert_eval_alias_by_d(3);
	TEST_ASSERT(alias != NULL, "D3 alias exists");
	TEST_ASSERT_EQ_INT(5, alias->a_pin);
	TEST_ASSERT_EQ_INT(28, alias->resource_id);
	TEST_ASSERT_EQ_INT(4, alias->ain);
}

static void test_alias_exhaustive_table(void)
{
	struct { uint8_t d, a, rid, ain; } expected[8] = {
		{ 0, 2,  4, 2},
		{ 1, 3,  5, 3},
		{ 2, 4,  3, 1},
		{ 3, 5, 28, 4},
		{ 4, 6,  2, 0},
		{10, 7, 30, 6},
		{12, 0, 31, 7},
		{16, 1, 29, 5},
	};

	for (int i = 0; i < 8; i++) {
		TEST_ASSERT_EQ_INT(expected[i].d,   EXPERT_ALIASES[i].d_pin);
		TEST_ASSERT_EQ_INT(expected[i].a,   EXPERT_ALIASES[i].a_pin);
		TEST_ASSERT_EQ_INT(expected[i].rid, EXPERT_ALIASES[i].resource_id);
		TEST_ASSERT_EQ_INT(expected[i].ain, EXPERT_ALIASES[i].ain);
	}
}

static void test_alias_lookup_by_resource(void)
{
	const expert_alias_t *r2 = expert_eval_alias_by_resource(2);
	TEST_ASSERT(r2 != NULL, "P0.02 is analog");
	TEST_ASSERT_EQ_INT(4, r2->d_pin);
	TEST_ASSERT_EQ_INT(0, r2->ain);

	const expert_alias_t *r31 = expert_eval_alias_by_resource(31);
	TEST_ASSERT(r31 != NULL, "P0.31 is analog");
	TEST_ASSERT_EQ_INT(12, r31->d_pin);
	TEST_ASSERT_EQ_INT(7, r31->ain);
}

static void test_alias_unknown_d_pin(void)
{
	uint8_t ain = 0xFF;
	TEST_ASSERT(!expert_eval_d_to_ain(5, &ain), "D5 does not exist");
	TEST_ASSERT(!expert_eval_d_to_ain(255, &ain), "D255 does not exist");
	TEST_ASSERT(expert_eval_alias_by_d(99) == NULL, "D99 NULL");
}

static void test_is_analog_pin(void)
{
	TEST_ASSERT(expert_eval_is_analog_pin(2), "P0.02 analog");
	TEST_ASSERT(expert_eval_is_analog_pin(3), "P0.03 analog");
	TEST_ASSERT(expert_eval_is_analog_pin(4), "P0.04 analog");
	TEST_ASSERT(expert_eval_is_analog_pin(5), "P0.05 analog");
	TEST_ASSERT(expert_eval_is_analog_pin(28), "P0.28 analog");
	TEST_ASSERT(expert_eval_is_analog_pin(29), "P0.29 analog");
	TEST_ASSERT(expert_eval_is_analog_pin(30), "P0.30 analog");
	TEST_ASSERT(expert_eval_is_analog_pin(31), "P0.31 analog");

	TEST_ASSERT(!expert_eval_is_analog_pin(0), "P0.00 not analog");
	TEST_ASSERT(!expert_eval_is_analog_pin(1), "P0.01 not analog");
	TEST_ASSERT(!expert_eval_is_analog_pin(6), "P0.06 not analog");
	TEST_ASSERT(!expert_eval_is_analog_pin(50), "out of range");
}

/* ================================================================
 * GPIO config validation
 * ================================================================ */

static void test_gpio_config_valid_defaults(void)
{
	expert_gpio_config_t cfg;
	cfg.dir = EXPERT_GPIO_DIR_INPUT;
	cfg.pull = EXPERT_GPIO_PULL_NONE;
	cfg.drive = EXPERT_GPIO_DRIVE_S0S1;
	cfg.sense = EXPERT_GPIO_SENSE_NONE;
	TEST_ASSERT(expert_eval_gpio_config_valid(&cfg), "defaults valid");
}

static void test_gpio_config_valid_output_h0h1(void)
{
	expert_gpio_config_t cfg;
	cfg.dir = EXPERT_GPIO_DIR_OUTPUT;
	cfg.pull = EXPERT_GPIO_PULL_PULLUP;
	cfg.drive = EXPERT_GPIO_DRIVE_H0H1;
	cfg.sense = EXPERT_GPIO_SENSE_NONE;
	TEST_ASSERT(expert_eval_gpio_config_valid(&cfg), "H0H1 valid");
}

static void test_gpio_config_valid_all_pulls(void)
{
	for (int p = 0; p < EXPERT_GPIO_PULL_COUNT; p++) {
		expert_gpio_config_t cfg;
		cfg.dir = EXPERT_GPIO_DIR_INPUT;
		cfg.pull = (expert_gpio_pull_t)p;
		cfg.drive = EXPERT_GPIO_DRIVE_S0S1;
		cfg.sense = EXPERT_GPIO_SENSE_NONE;
		TEST_ASSERT(expert_eval_gpio_config_valid(&cfg), "pull valid");
	}
}

static void test_gpio_config_valid_all_drives(void)
{
	for (int d = 0; d < EXPERT_GPIO_DRIVE_COUNT; d++) {
		expert_gpio_config_t cfg;
		cfg.dir = EXPERT_GPIO_DIR_OUTPUT;
		cfg.pull = EXPERT_GPIO_PULL_NONE;
		cfg.drive = (expert_gpio_drive_t)d;
		cfg.sense = EXPERT_GPIO_SENSE_NONE;
		TEST_ASSERT(expert_eval_gpio_config_valid(&cfg), "drive valid");
	}
}

static void test_gpio_config_valid_all_senses(void)
{
	for (int s = 0; s < EXPERT_GPIO_SENSE_COUNT; s++) {
		expert_gpio_config_t cfg;
		cfg.dir = EXPERT_GPIO_DIR_INPUT;
		cfg.pull = EXPERT_GPIO_PULL_NONE;
		cfg.drive = EXPERT_GPIO_DRIVE_S0S1;
		cfg.sense = (expert_gpio_sense_t)s;
		TEST_ASSERT(expert_eval_gpio_config_valid(&cfg), "sense valid");
	}
}

static void test_gpio_config_invalid_dir(void)
{
	expert_gpio_config_t cfg;
	memset(&cfg, 0xFF, sizeof cfg);
	cfg.pull = EXPERT_GPIO_PULL_NONE;
	cfg.drive = EXPERT_GPIO_DRIVE_S0S1;
	cfg.sense = EXPERT_GPIO_SENSE_NONE;
	/* dir is set to invalid via memset */
	TEST_ASSERT(!expert_eval_gpio_config_valid(&cfg), "bad dir rejected");
}

static void test_gpio_config_invalid_pull(void)
{
	expert_gpio_config_t cfg;
	memset(&cfg, 0xFF, sizeof cfg);
	cfg.dir = EXPERT_GPIO_DIR_INPUT;
	cfg.drive = EXPERT_GPIO_DRIVE_S0S1;
	cfg.sense = EXPERT_GPIO_SENSE_NONE;
	TEST_ASSERT(!expert_eval_gpio_config_valid(&cfg), "bad pull rejected");
}

static void test_gpio_config_invalid_drive(void)
{
	expert_gpio_config_t cfg;
	memset(&cfg, 0xFF, sizeof cfg);
	cfg.dir = EXPERT_GPIO_DIR_OUTPUT;
	cfg.pull = EXPERT_GPIO_PULL_NONE;
	cfg.sense = EXPERT_GPIO_SENSE_NONE;
	TEST_ASSERT(!expert_eval_gpio_config_valid(&cfg), "bad drive rejected");
}

static void test_gpio_config_null(void)
{
	TEST_ASSERT(!expert_eval_gpio_config_valid(NULL), "NULL rejected");
}

/* ================================================================
 * SAADC raw -> millivolt conversion
 * ================================================================ */

static void test_adc_raw_zero(void)
{
	uint16_t mv = 0xFFFF;
	TEST_ASSERT_EQ_INT(EXPERT_ADC_OK, expert_eval_adc_raw_to_mv(0, &mv));
	TEST_ASSERT_EQ_INT(0, mv);
}

static void test_adc_raw_midrange(void)
{
	uint16_t mv = 0xFFFF;
	TEST_ASSERT_EQ_INT(EXPERT_ADC_OK, expert_eval_adc_raw_to_mv(2048, &mv));
	TEST_ASSERT_EQ_INT(1800, mv);
}

static void test_adc_raw_quarter(void)
{
	uint16_t mv = 0xFFFF;
	TEST_ASSERT_EQ_INT(EXPERT_ADC_OK, expert_eval_adc_raw_to_mv(1024, &mv));
	TEST_ASSERT_EQ_INT(900, mv);
}

static void test_adc_raw_at_vdd_boundary(void)
{
	/* raw=3754 -> 3754*3600/4095 = 3300 mV (at VDD, not saturated) */
	uint16_t mv = 0xFFFF;
	TEST_ASSERT_EQ_INT(EXPERT_ADC_OK, expert_eval_adc_raw_to_mv(3754, &mv));
	TEST_ASSERT_EQ_INT(3300, mv);
}

static void test_adc_raw_above_vdd_saturated(void)
{
	/* raw=3755 -> mV > 3300 -> SATURATED, clamped to 3300 */
	uint16_t mv = 0xFFFF;
	TEST_ASSERT_EQ_INT(EXPERT_ADC_SATURATED,
		expert_eval_adc_raw_to_mv(3755, &mv));
	TEST_ASSERT_EQ_INT(3300, mv);
}

static void test_adc_raw_max_saturated(void)
{
	uint16_t mv = 0xFFFF;
	TEST_ASSERT_EQ_INT(EXPERT_ADC_SATURATED,
		expert_eval_adc_raw_to_mv(4095, &mv));
	TEST_ASSERT_EQ_INT(3300, mv);
}

static void test_adc_raw_formula_constants(void)
{
	TEST_ASSERT_EQ_INT(600, EXPERT_ADC_REF_MV);
	TEST_ASSERT_EQ_INT(3600, EXPERT_ADC_FS_MV);
	TEST_ASSERT_EQ_INT(4095, EXPERT_ADC_MAX_RAW);
	TEST_ASSERT_EQ_INT(3300, EXPERT_VDD_NOMINAL_MV);
}

/* ================================================================
 * Oversampling 4x
 * ================================================================ */

static void test_adc_oversample_uniform(void)
{
	uint16_t samples[4] = {1000, 1000, 1000, 1000};
	TEST_ASSERT_EQ_INT(1000, expert_eval_adc_oversample_4x(samples));
}

static void test_adc_oversample_mixed(void)
{
	uint16_t samples[4] = {100, 200, 300, 400};
	TEST_ASSERT_EQ_INT(250, expert_eval_adc_oversample_4x(samples));
}

static void test_adc_oversample_truncation(void)
{
	uint16_t samples[4] = {100, 200, 300, 401};
	TEST_ASSERT_EQ_INT(250, expert_eval_adc_oversample_4x(samples));
}

static void test_adc_oversample_max_values(void)
{
	uint16_t samples[4] = {4095, 4095, 4095, 4095};
	TEST_ASSERT_EQ_INT(4095, expert_eval_adc_oversample_4x(samples));
}

static void test_adc_oversample_zeros(void)
{
	uint16_t samples[4] = {0, 0, 0, 0};
	TEST_ASSERT_EQ_INT(0, expert_eval_adc_oversample_4x(samples));
}

static void test_adc_calibrate_result(void)
{
	TEST_ASSERT_EQ_INT(EXPERT_ADC_CAL_DONE, expert_eval_adc_calibrate_result());
}

/* ================================================================
 * Lease lifecycle
 * ================================================================ */

#define TEST_OWNER_A  8  /* PINREG_OWNER_GPIO_USER */
#define TEST_OWNER_B  6  /* PINREG_OWNER_NEOPIXEL  */

static void test_lease_issue_returns_token(void)
{
	expert_eval_init();
	expert_token_t tok = EXPERT_TOKEN_INVALID;
	TEST_ASSERT_EQ_INT(EXPERT_OK,
		expert_eval_lease_issue(4, TEST_OWNER_A, false, &tok));
	TEST_ASSERT(tok != EXPERT_TOKEN_INVALID, "got non-zero token");
}

static void test_lease_validate_correct_owner(void)
{
	expert_eval_init();
	expert_token_t tok = EXPERT_TOKEN_INVALID;
	expert_eval_lease_issue(4, TEST_OWNER_A, false, &tok);
	TEST_ASSERT_EQ_INT(EXPERT_OK,
		expert_eval_lease_validate(tok, TEST_OWNER_A));
}

static void test_lease_reuse_multiple_operations(void)
{
	expert_eval_init();
	expert_token_t tok = EXPERT_TOKEN_INVALID;
	expert_eval_lease_issue(4, TEST_OWNER_A, false, &tok);
	for (int i = 0; i < 5; i++) {
		TEST_ASSERT_EQ_INT(EXPERT_OK,
			expert_eval_lease_validate(tok, TEST_OWNER_A));
	}
}

static void test_lease_release_valid(void)
{
	expert_eval_init();
	expert_token_t tok = EXPERT_TOKEN_INVALID;
	expert_eval_lease_issue(4, TEST_OWNER_A, false, &tok);
	TEST_ASSERT_EQ_INT(EXPERT_OK, expert_eval_lease_release(tok, TEST_OWNER_A));
	TEST_ASSERT_EQ_INT(EXPERT_ERR_INVALID_TOKEN,
		expert_eval_lease_validate(tok, TEST_OWNER_A));
}

static void test_lease_stale_token_rejected(void)
{
	expert_eval_init();
	expert_token_t tok = EXPERT_TOKEN_INVALID;
	expert_eval_lease_issue(4, TEST_OWNER_A, false, &tok);
	expert_eval_lease_release(tok, TEST_OWNER_A);
	TEST_ASSERT_EQ_INT(EXPERT_ERR_INVALID_TOKEN,
		expert_eval_lease_validate(tok, TEST_OWNER_A));
}

static void test_lease_idempotent_release(void)
{
	expert_eval_init();
	expert_token_t tok = EXPERT_TOKEN_INVALID;
	expert_eval_lease_issue(4, TEST_OWNER_A, false, &tok);
	TEST_ASSERT_EQ_INT(EXPERT_OK, expert_eval_lease_release(tok, TEST_OWNER_A));
	TEST_ASSERT_EQ_INT(EXPERT_OK, expert_eval_lease_release(tok, TEST_OWNER_A));
}

static void test_lease_idempotent_does_not_extend(void)
{
	expert_eval_init();
	expert_token_t t1 = EXPERT_TOKEN_INVALID, t2 = EXPERT_TOKEN_INVALID;
	expert_eval_lease_issue(4, TEST_OWNER_A, false, &t1);
	expert_eval_lease_issue(5, TEST_OWNER_A, false, &t2);

	TEST_ASSERT_EQ_INT(EXPERT_OK, expert_eval_lease_release(t1, TEST_OWNER_A));
	TEST_ASSERT_EQ_INT(EXPERT_OK, expert_eval_lease_release(t1, TEST_OWNER_A));
	TEST_ASSERT_EQ_INT(EXPERT_OK, expert_eval_lease_release(t2, TEST_OWNER_A));
	TEST_ASSERT_EQ_INT(EXPERT_OK, expert_eval_lease_release(t2, TEST_OWNER_A));
	/* t1 after t2: NOT last-released, so rejected. */
	TEST_ASSERT_EQ_INT(EXPERT_ERR_INVALID_TOKEN,
		expert_eval_lease_release(t1, TEST_OWNER_A));
}

static void test_lease_wrong_owner_rejected(void)
{
	expert_eval_init();
	expert_token_t tok = EXPERT_TOKEN_INVALID;
	expert_eval_lease_issue(4, TEST_OWNER_A, false, &tok);
	TEST_ASSERT_EQ_INT(EXPERT_ERR_WRONG_OWNER,
		expert_eval_lease_validate(tok, TEST_OWNER_B));
}

static void test_lease_release_wrong_owner_rejected(void)
{
	/* Only the original owner may release. A second owner that knows
	 * the token cannot force-release the first owner's lease. */
	expert_eval_init();
	expert_token_t tok = EXPERT_TOKEN_INVALID;
	expert_eval_lease_issue(4, TEST_OWNER_A, false, &tok);
	TEST_ASSERT_EQ_INT(EXPERT_ERR_WRONG_OWNER,
		expert_eval_lease_release(tok, TEST_OWNER_B));
	/* Lease still active for the legitimate owner. */
	TEST_ASSERT_EQ_INT(EXPERT_OK,
		expert_eval_lease_validate(tok, TEST_OWNER_A));
	TEST_ASSERT_EQ_INT(EXPERT_OK,
		expert_eval_lease_release(tok, TEST_OWNER_A));
}

static void test_lease_cross_session_rejected(void)
{
	expert_eval_init();
	expert_token_t tok = EXPERT_TOKEN_INVALID;
	expert_eval_lease_issue(4, TEST_OWNER_A, false, &tok);
	TEST_ASSERT_EQ_INT(EXPERT_OK,
		expert_eval_lease_validate(tok, TEST_OWNER_A));

	expert_eval_init();
	TEST_ASSERT_EQ_INT(EXPERT_ERR_INVALID_TOKEN,
		expert_eval_lease_validate(tok, TEST_OWNER_A));
	TEST_ASSERT_EQ_INT(EXPERT_ERR_INVALID_TOKEN,
		expert_eval_lease_release(tok, TEST_OWNER_A));
}

static void test_lease_invalid_param(void)
{
	expert_eval_init();
	expert_token_t tok = EXPERT_TOKEN_INVALID;
	TEST_ASSERT_EQ_INT(EXPERT_ERR_INVALID_PARAM,
		expert_eval_lease_issue(4, TEST_OWNER_A, false, NULL));
	TEST_ASSERT_EQ_INT(EXPERT_ERR_INVALID_PARAM,
		expert_eval_lease_issue(4, 0, false, &tok));
}

static void test_lease_non_analog_rejected_for_adc(void)
{
	expert_eval_init();
	expert_token_t tok = EXPERT_TOKEN_INVALID;
	TEST_ASSERT_EQ_INT(EXPERT_ERR_NOT_ANALOG,
		expert_eval_lease_issue(6, TEST_OWNER_A, true, &tok));
	TEST_ASSERT_EQ_INT(EXPERT_TOKEN_INVALID, tok);

	TEST_ASSERT_EQ_INT(EXPERT_OK,
		expert_eval_lease_issue(6, TEST_OWNER_A, false, &tok));
	TEST_ASSERT(tok != EXPERT_TOKEN_INVALID, "GPIO accepts non-analog");
}

static void test_lease_table_full(void)
{
	expert_eval_init();
	expert_token_t toks[EXPERT_MAX_LEASES];
	static const uint8_t rids[] = {2, 3, 4, 5, 28, 29, 30, 31, 6, 7, 8, 9};

	for (int i = 0; i < (int)EXPERT_MAX_LEASES; i++) {
		toks[i] = EXPERT_TOKEN_INVALID;
		TEST_ASSERT_EQ_INT(EXPERT_OK,
			expert_eval_lease_issue(rids[i], TEST_OWNER_A, false, &toks[i]));
		TEST_ASSERT(toks[i] != EXPERT_TOKEN_INVALID, "slot allocated");
	}

	expert_token_t overflow = EXPERT_TOKEN_INVALID;
	TEST_ASSERT_EQ_INT(EXPERT_ERR_TABLE_FULL,
		expert_eval_lease_issue(10, TEST_OWNER_A, false, &overflow));
	TEST_ASSERT_EQ_INT(EXPERT_TOKEN_INVALID, overflow);

	TEST_ASSERT_EQ_INT(EXPERT_OK, expert_eval_lease_release(toks[0], TEST_OWNER_A));
	TEST_ASSERT_EQ_INT(EXPERT_OK,
		expert_eval_lease_issue(10, TEST_OWNER_A, false, &overflow));
	TEST_ASSERT(overflow != EXPERT_TOKEN_INVALID, "freed slot reused");
}

static void test_lease_query(void)
{
	expert_eval_init();
	expert_token_t tok = EXPERT_TOKEN_INVALID;
	expert_eval_lease_issue(4, TEST_OWNER_A, false, &tok);

	uint8_t resource = 0xFF, owner = 0xFF;
	TEST_ASSERT(expert_eval_lease_query(tok, &resource, &owner), "query active");
	TEST_ASSERT_EQ_INT(4, resource);
	TEST_ASSERT_EQ_INT(TEST_OWNER_A, owner);

	TEST_ASSERT(!expert_eval_lease_query(EXPERT_TOKEN_INVALID,
	                                     &resource, &owner), "invalid token");

	expert_eval_lease_release(tok, TEST_OWNER_A);
	TEST_ASSERT(!expert_eval_lease_query(tok, &resource, &owner), "released");
}

static void test_lease_release_all(void)
{
	expert_eval_init();
	expert_token_t t1 = EXPERT_TOKEN_INVALID, t2 = EXPERT_TOKEN_INVALID;
	expert_eval_lease_issue(4, TEST_OWNER_A, false, &t1);
	expert_eval_lease_issue(5, TEST_OWNER_A, false, &t2);

	expert_eval_lease_release_all();

	TEST_ASSERT(!expert_eval_lease_query(t1, NULL, NULL), "t1 gone");
	TEST_ASSERT(!expert_eval_lease_query(t2, NULL, NULL), "t2 gone");
	TEST_ASSERT_EQ_INT(EXPERT_ERR_INVALID_TOKEN,
		expert_eval_lease_release(t1, TEST_OWNER_A));
	TEST_ASSERT_EQ_INT(EXPERT_ERR_INVALID_TOKEN,
		expert_eval_lease_release(t2, TEST_OWNER_A));
}

static void test_lease_multiple_owners_distinguished(void)
{
	expert_eval_init();
	expert_token_t t1 = EXPERT_TOKEN_INVALID, t2 = EXPERT_TOKEN_INVALID;
	TEST_ASSERT_EQ_INT(EXPERT_OK,
		expert_eval_lease_issue(4, TEST_OWNER_A, false, &t1));
	TEST_ASSERT_EQ_INT(EXPERT_OK,
		expert_eval_lease_issue(4, TEST_OWNER_B, false, &t2));

	TEST_ASSERT_EQ_INT(EXPERT_OK,
		expert_eval_lease_validate(t1, TEST_OWNER_A));
	TEST_ASSERT_EQ_INT(EXPERT_ERR_WRONG_OWNER,
		expert_eval_lease_validate(t1, TEST_OWNER_B));
	TEST_ASSERT_EQ_INT(EXPERT_OK,
		expert_eval_lease_validate(t2, TEST_OWNER_B));
	TEST_ASSERT_EQ_INT(EXPERT_ERR_WRONG_OWNER,
		expert_eval_lease_validate(t2, TEST_OWNER_A));
}

static void test_session_increments(void)
{
	expert_eval_init();
	uint16_t s1 = expert_eval_get_session();
	expert_eval_init();
	uint16_t s2 = expert_eval_get_session();
	TEST_ASSERT_EQ_INT((int)(s1 + 1), (int)s2);
}

/* ================================================================
 * Result-code mapping (expert_result_t → board_BoardResultCode)
 * Shared by GpioConfigure/Read/Write, AdcRead, ReleasePin handlers.
 * ================================================================ */

static void test_to_board_result_ok(void)
{
	TEST_ASSERT_EQ_INT((long long)board_BoardResultCode_SUCCESS,
		(long long)expert_eval_to_board_result(EXPERT_OK));
}

static void test_to_board_result_invalid_param(void)
{
	TEST_ASSERT_EQ_INT((long long)board_BoardResultCode_INVALID_ARGUMENT,
		(long long)expert_eval_to_board_result(EXPERT_ERR_INVALID_PARAM));
}

static void test_to_board_result_not_analog(void)
{
	TEST_ASSERT_EQ_INT((long long)board_BoardResultCode_INVALID_ARGUMENT,
		(long long)expert_eval_to_board_result(EXPERT_ERR_NOT_ANALOG));
}

static void test_to_board_result_table_full(void)
{
	TEST_ASSERT_EQ_INT((long long)board_BoardResultCode_BUSY,
		(long long)expert_eval_to_board_result(EXPERT_ERR_TABLE_FULL));
}

static void test_to_board_result_invalid_token(void)
{
	TEST_ASSERT_EQ_INT((long long)board_BoardResultCode_INVALID_ARGUMENT,
		(long long)expert_eval_to_board_result(EXPERT_ERR_INVALID_TOKEN));
}

static void test_to_board_result_wrong_owner(void)
{
	TEST_ASSERT_EQ_INT((long long)board_BoardResultCode_INVALID_ARGUMENT,
		(long long)expert_eval_to_board_result(EXPERT_ERR_WRONG_OWNER));
}

static void test_to_board_result_wrong_session(void)
{
	TEST_ASSERT_EQ_INT((long long)board_BoardResultCode_INVALID_ARGUMENT,
		(long long)expert_eval_to_board_result(EXPERT_ERR_WRONG_SESSION));
}

/* ---- Compile-time guard ---- */
static_assert(EXPERT_ALIAS_COUNT == 8,
	"CLUE has exactly 8 analog-capable edge connector pins");

int main(void)
{
	test_framework_init();

	/* Alias table */
	RUN_TEST(test_alias_table_size);
	RUN_TEST(test_alias_all_d_to_ain);
	RUN_TEST(test_alias_non_trivial_a_to_ain);
	RUN_TEST(test_alias_d3_a5_is_ain4);
	RUN_TEST(test_alias_exhaustive_table);
	RUN_TEST(test_alias_lookup_by_resource);
	RUN_TEST(test_alias_unknown_d_pin);
	RUN_TEST(test_is_analog_pin);

	/* GPIO config validation */
	RUN_TEST(test_gpio_config_valid_defaults);
	RUN_TEST(test_gpio_config_valid_output_h0h1);
	RUN_TEST(test_gpio_config_valid_all_pulls);
	RUN_TEST(test_gpio_config_valid_all_drives);
	RUN_TEST(test_gpio_config_valid_all_senses);
	RUN_TEST(test_gpio_config_invalid_dir);
	RUN_TEST(test_gpio_config_invalid_pull);
	RUN_TEST(test_gpio_config_invalid_drive);
	RUN_TEST(test_gpio_config_null);

	/* ADC mV conversion */
	RUN_TEST(test_adc_raw_zero);
	RUN_TEST(test_adc_raw_midrange);
	RUN_TEST(test_adc_raw_quarter);
	RUN_TEST(test_adc_raw_at_vdd_boundary);
	RUN_TEST(test_adc_raw_above_vdd_saturated);
	RUN_TEST(test_adc_raw_max_saturated);
	RUN_TEST(test_adc_raw_formula_constants);

	/* Oversampling */
	RUN_TEST(test_adc_oversample_uniform);
	RUN_TEST(test_adc_oversample_mixed);
	RUN_TEST(test_adc_oversample_truncation);
	RUN_TEST(test_adc_oversample_max_values);
	RUN_TEST(test_adc_oversample_zeros);
	RUN_TEST(test_adc_calibrate_result);

	/* Lease lifecycle */
	RUN_TEST(test_lease_issue_returns_token);
	RUN_TEST(test_lease_validate_correct_owner);
	RUN_TEST(test_lease_reuse_multiple_operations);
	RUN_TEST(test_lease_release_valid);
	RUN_TEST(test_lease_stale_token_rejected);
	RUN_TEST(test_lease_idempotent_release);
	RUN_TEST(test_lease_idempotent_does_not_extend);
	RUN_TEST(test_lease_wrong_owner_rejected);
	RUN_TEST(test_lease_release_wrong_owner_rejected);
	RUN_TEST(test_lease_cross_session_rejected);
	RUN_TEST(test_lease_invalid_param);
	RUN_TEST(test_lease_non_analog_rejected_for_adc);
	RUN_TEST(test_lease_table_full);
	RUN_TEST(test_lease_query);
	RUN_TEST(test_lease_release_all);
	RUN_TEST(test_lease_multiple_owners_distinguished);
	RUN_TEST(test_session_increments);

	/* Result-code mapping (shared by GPIO/ADC expert handlers) */
	RUN_TEST(test_to_board_result_ok);
	RUN_TEST(test_to_board_result_invalid_param);
	RUN_TEST(test_to_board_result_not_analog);
	RUN_TEST(test_to_board_result_table_full);
	RUN_TEST(test_to_board_result_invalid_token);
	RUN_TEST(test_to_board_result_wrong_owner);
	RUN_TEST(test_to_board_result_wrong_session);

	return test_framework_finish();
}
