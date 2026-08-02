#include "test_framework.h"

/* whad-lib types must be available before capabilities.h. */
#include "types.h"
#include "discovery.h"
#include "domains/phy.h"

/* runtime.h must be included before capabilities.h (it declares
 * runtime_mode_t used by getRuntimeCapabilities). */
#include "runtime.h"

#define BOARD_CLUE
#include "capabilities.h"
#include "boardModuleC.h"
#include "../../src/boardModuleEval.c"

static void test_cmd_is_64bit(void)
{
	uint64_t lo  = CMD(0);
	uint64_t mid = CMD(31);
	uint64_t hi  = CMD(55);

	TEST_ASSERT_EQ_INT(1ULL, (long long)lo);
	TEST_ASSERT_EQ_INT(0x80000000ULL, (long long)mid);
	TEST_ASSERT_EQ_INT(0x0080000000000000ULL, (long long)hi);
}

static void test_cmd_union_is_64bit(void)
{
	uint64_t combined = CMD(0) | CMD(31) | CMD(63);
	TEST_ASSERT_EQ_INT((long long)((1ULL << 63) | 0x80000001ULL),
		(long long)combined);
}

static void test_capabilities_raw_whad_has_board(void)
{
	int foundBoard = 0;
	for (const whad_domain_desc_t *p = CAPABILITIES_RAW_WHAD;
	     p->domain != DOMAIN_NONE; ++p) {
		if (p->domain == DOMAIN_BOARD) {
			foundBoard = 1;
			TEST_ASSERT_EQ_INT(
				(long long)(CAP_READ | CAP_WRITE),
				(long long)p->cap);
			TEST_ASSERT(p->supported_commands != 0,
				"Board supported_commands zero");
			break;
		}
	}
	TEST_ASSERT_EQ_INT(1, foundBoard);
}

static void test_capabilities_ble_hid_board_only(void)
{
	int count = 0;
	for (const whad_domain_desc_t *p = CAPABILITIES_BLE_HID;
	     p->domain != DOMAIN_NONE; ++p) {
		++count;
		TEST_ASSERT_EQ_INT((long long)DOMAIN_BOARD,
			(long long)p->domain);
	}
	TEST_ASSERT_EQ_INT(1, count);
}

static void test_capabilities_raw_whad_has_six_domains(void)
{
	int count = 0;
	for (const whad_domain_desc_t *p = CAPABILITIES_RAW_WHAD;
	     p->domain != DOMAIN_NONE; ++p) {
		++count;
	}
	TEST_ASSERT_EQ_INT(6, count);
}

static void test_capabilities_non_clue_has_five_domains(void)
{
	int count = 0;
	for (const whad_domain_desc_t *p = CAPABILITIES;
	     p->domain != DOMAIN_NONE; ++p) {
		++count;
		TEST_ASSERT(p->domain != DOMAIN_BOARD,
			"non-CLUE must not include BOARD");
	}
	TEST_ASSERT_EQ_INT(5, count);
}

static void test_capabilities_non_clue_no_board(void)
{
	int foundBoard = 0;
	for (const whad_domain_desc_t *p = CAPABILITIES;
	     p->domain != DOMAIN_NONE; ++p) {
		if (p->domain == DOMAIN_BOARD) {
			foundBoard = 1;
			break;
		}
	}
	TEST_ASSERT_EQ_INT(0, foundBoard);
}

static void test_rt_to_proto(void)
{
	TEST_ASSERT_EQ_INT((long long)BOARD_RT_RAW_WHAD,
		(long long)boardmodule_rt_to_proto(RUNTIME_RAW_WHAD));
	TEST_ASSERT_EQ_INT((long long)BOARD_RT_BLE_HID,
		(long long)boardmodule_rt_to_proto(RUNTIME_BLE_HID));
}

static void test_proto_to_rt(void)
{
	bool valid = false;
	runtime_mode_t rt;

	rt = boardmodule_proto_to_rt(BOARD_RT_RAW_WHAD, &valid);
	TEST_ASSERT_EQ_INT(1, valid);
	TEST_ASSERT_EQ_INT((long long)RUNTIME_RAW_WHAD, (long long)rt);

	rt = boardmodule_proto_to_rt(BOARD_RT_BLE_HID, &valid);
	TEST_ASSERT_EQ_INT(1, valid);
	TEST_ASSERT_EQ_INT((long long)RUNTIME_BLE_HID, (long long)rt);

	rt = boardmodule_proto_to_rt(BOARD_RT_UNKNOWN, &valid);
	TEST_ASSERT_EQ_INT(0, valid);

	rt = boardmodule_proto_to_rt(99, &valid);
	TEST_ASSERT_EQ_INT(0, valid);
}

static void test_eval_set_runtime_mode_success(void)
{
	long long code = boardmodule_eval_set_runtime_mode(
		BOARD_RT_RAW_WHAD, false);
	TEST_ASSERT_EQ_INT((long long)board_BoardResultCode_SUCCESS, code);

	code = boardmodule_eval_set_runtime_mode(
		BOARD_RT_BLE_HID, false);
	TEST_ASSERT_EQ_INT((long long)board_BoardResultCode_SUCCESS, code);
}

static void test_eval_set_runtime_mode_not_adopted(void)
{
	long long code = boardmodule_eval_set_runtime_mode(
		BOARD_RT_RAW_WHAD, true);
	TEST_ASSERT_EQ_INT(
		(long long)board_BoardResultCode_NOT_ADOPTED, code);
}

static void test_eval_set_runtime_mode_wrong_mode(void)
{
	long long code = boardmodule_eval_set_runtime_mode(
		BOARD_RT_UNKNOWN, false);
	TEST_ASSERT_EQ_INT(
		(long long)board_BoardResultCode_WRONG_MODE, code);
}

static void test_eval_set_runtime_config_pairing_bonds_success(void)
{
	long long code = boardmodule_eval_set_runtime_config(
		BOARD_CFG_OP_OPEN_PAIRING, false);
	TEST_ASSERT_EQ_INT(
		(long long)board_BoardResultCode_SUCCESS, code);

	code = boardmodule_eval_set_runtime_config(
		BOARD_CFG_OP_CLEAR_BONDS, false);
	TEST_ASSERT_EQ_INT(
		(long long)board_BoardResultCode_SUCCESS, code);
}

static void test_eval_set_runtime_config_update_success(void)
{
	long long code = boardmodule_eval_set_runtime_config(
		BOARD_CFG_OP_UPDATE, false);
	TEST_ASSERT_EQ_INT(
		(long long)board_BoardResultCode_SUCCESS, code);
}

static void test_eval_set_runtime_config_update_not_adopted(void)
{
	long long code = boardmodule_eval_set_runtime_config(
		BOARD_CFG_OP_UPDATE, true);
	TEST_ASSERT_EQ_INT(
		(long long)board_BoardResultCode_NOT_ADOPTED, code);
}

static void test_eval_set_runtime_config_invalid(void)
{
	long long code = boardmodule_eval_set_runtime_config(99, false);
	TEST_ASSERT_EQ_INT(
		(long long)board_BoardResultCode_INVALID_ARGUMENT, code);
}

static void test_is_command_advertised(void)
{
	TEST_ASSERT_EQ_INT(1, boardmodule_is_command_advertised(
		CMD(WHAD_BOARD_CMD_GET_BOARD_INFO)));
	TEST_ASSERT_EQ_INT(1, boardmodule_is_command_advertised(
		CMD(WHAD_BOARD_CMD_GET_RUNTIME_CONFIG)));
	TEST_ASSERT_EQ_INT(1, boardmodule_is_command_advertised(
		CMD(WHAD_BOARD_CMD_SET_RUNTIME_MODE)));
	TEST_ASSERT_EQ_INT(1, boardmodule_is_command_advertised(
		CMD(WHAD_BOARD_CMD_SET_RUNTIME_CONFIG)));

	TEST_ASSERT_EQ_INT(1, boardmodule_is_command_advertised(
		CMD(WHAD_BOARD_CMD_LIST_SENSORS)));
	TEST_ASSERT_EQ_INT(1, boardmodule_is_command_advertised(
		CMD(WHAD_BOARD_CMD_READ_SENSOR)));
	TEST_ASSERT_EQ_INT(1, boardmodule_is_command_advertised(
		CMD(WHAD_BOARD_CMD_I2C_TRANSFER)));
	TEST_ASSERT_EQ_INT(0, boardmodule_is_command_advertised(
		CMD(WHAD_BOARD_CMD_STORAGE_INFO)));
}

static void test_get_runtime_caps_raw_whad(void)
{
	const whad_domain_desc_t *caps =
		getRuntimeCapabilities(RUNTIME_RAW_WHAD);
	TEST_ASSERT(caps != NULL, "raw-WHAD caps must not be NULL");

	int foundBoard = 0;
	for (const whad_domain_desc_t *p = caps;
	     p->domain != DOMAIN_NONE; ++p) {
		if (p->domain == DOMAIN_BOARD) {
			foundBoard = 1;
			break;
		}
	}
	TEST_ASSERT_EQ_INT(1, foundBoard);
}

static void test_get_runtime_caps_ble_hid(void)
{
	const whad_domain_desc_t *caps =
		getRuntimeCapabilities(RUNTIME_BLE_HID);
	TEST_ASSERT(caps != NULL, "BLE-HID caps must not be NULL");

	int foundBoard = 0;
	for (const whad_domain_desc_t *p = caps;
	     p->domain != DOMAIN_NONE; ++p) {
		TEST_ASSERT(p->domain != DOMAIN_BTLE,
			"BLE-HID must not advertise BLE domain");
		if (p->domain == DOMAIN_BOARD) {
			foundBoard = 1;
		}
	}
	TEST_ASSERT_EQ_INT(1, foundBoard);
}

static void test_get_runtime_caps_raw_has_radio_domains(void)
{
	const whad_domain_desc_t *caps =
		getRuntimeCapabilities(RUNTIME_RAW_WHAD);

	int foundBle = 0, foundD15 = 0, foundEsb = 0,
	    foundUni = 0, foundPhy = 0;
	for (const whad_domain_desc_t *p = caps;
	     p->domain != DOMAIN_NONE; ++p) {
		if (p->domain == DOMAIN_BTLE)               foundBle = 1;
		if (p->domain == DOMAIN_DOT15D4)            foundD15 = 1;
		if (p->domain == DOMAIN_ESB)                foundEsb = 1;
		if (p->domain == DOMAIN_LOGITECH_UNIFYING)  foundUni = 1;
		if (p->domain == DOMAIN_PHY)                foundPhy = 1;
	}
	TEST_ASSERT_EQ_INT(1, foundBle);
	TEST_ASSERT_EQ_INT(1, foundD15);
	TEST_ASSERT_EQ_INT(1, foundEsb);
	TEST_ASSERT_EQ_INT(1, foundUni);
	TEST_ASSERT_EQ_INT(1, foundPhy);
}

static void test_get_runtime_caps_ble_has_no_radio_domains(void)
{
	const whad_domain_desc_t *caps =
		getRuntimeCapabilities(RUNTIME_BLE_HID);

	for (const whad_domain_desc_t *p = caps;
	     p->domain != DOMAIN_NONE; ++p) {
		TEST_ASSERT(p->domain != DOMAIN_BTLE,
			"BLE-HID must not include BLE radio domain");
		TEST_ASSERT(p->domain != DOMAIN_DOT15D4,
			"BLE-HID must not include Dot15d4");
		TEST_ASSERT(p->domain != DOMAIN_ESB,
			"BLE-HID must not include ESB");
		TEST_ASSERT(p->domain != DOMAIN_LOGITECH_UNIFYING,
			"BLE-HID must not include Unifying");
		TEST_ASSERT(p->domain != DOMAIN_PHY,
			"BLE-HID must not include PHY");
	}
}

/* ---- Integration: exhaustive command-path coverage ------------------- */

/* Table of all 28 BoardCommand values. Each entry pairs the command
 * with whether BoardModule advertises it (implemented handler) or
 * returns NOT_IMPLEMENTED (default case in processMessage switch). */
static const struct {
	uint32_t cmd_bit;
	int       advertised;
} ALL_BOARD_COMMANDS[] = {
	{ CMD(WHAD_BOARD_CMD_GET_BOARD_INFO),     1 },
	{ CMD(WHAD_BOARD_CMD_LIST_SENSORS),       1 },
	{ CMD(WHAD_BOARD_CMD_READ_SENSOR),        1 },
	{ CMD(WHAD_BOARD_CMD_CONFIGURE_STREAM),   1 },
	{ CMD(WHAD_BOARD_CMD_STOP_STREAM),        1 },
	{ CMD(WHAD_BOARD_CMD_CALIBRATE),          1 },
	{ CMD(WHAD_BOARD_CMD_GET_CALIBRATION),    1 },
	{ CMD(WHAD_BOARD_CMD_SET_OUTPUT),         1 },
	{ CMD(WHAD_BOARD_CMD_GET_INPUT_STATE),    1 },
	{ CMD(WHAD_BOARD_CMD_CONFIGURE_INPUT),    1 },
	{ CMD(WHAD_BOARD_CMD_I2C_TRANSFER),       1 },
	{ CMD(WHAD_BOARD_CMD_GPIO_CONFIGURE),     0 },
	{ CMD(WHAD_BOARD_CMD_GPIO_READ),          0 },
	{ CMD(WHAD_BOARD_CMD_GPIO_WRITE),         0 },
	{ CMD(WHAD_BOARD_CMD_ADC_READ),           0 },
	{ CMD(WHAD_BOARD_CMD_SPI_TRANSFER),       1 },
	{ CMD(WHAD_BOARD_CMD_STORAGE_INFO),       0 },
	{ CMD(WHAD_BOARD_CMD_STORAGE_ADOPT),      0 },
	{ CMD(WHAD_BOARD_CMD_STORAGE_READ_LOG),   0 },
	{ CMD(WHAD_BOARD_CMD_STORAGE_ERASE_LOG),  0 },
	{ CMD(WHAD_BOARD_CMD_GET_RUNTIME_CONFIG), 1 },
	{ CMD(WHAD_BOARD_CMD_SET_RUNTIME_CONFIG), 1 },
	{ CMD(WHAD_BOARD_CMD_SET_RUNTIME_MODE),   1 },
	{ CMD(WHAD_BOARD_CMD_REMOTE_PROFILE_GET), 1 },
	{ CMD(WHAD_BOARD_CMD_REMOTE_PROFILE_SET), 1 },
	{ CMD(WHAD_BOARD_CMD_AUDIO_CONFIGURE),    1 },
	{ CMD(WHAD_BOARD_CMD_RELEASE_PIN),        1 },
	{ CMD(WHAD_BOARD_CMD_RAW_PCM_DIAGNOSTICS),1 },
};

#define NUM_BOARD_COMMANDS \
	(sizeof(ALL_BOARD_COMMANDS) / sizeof(ALL_BOARD_COMMANDS[0]))

static void test_all_board_commands_classified(void)
{
	TEST_ASSERT_EQ_INT(28, (int)NUM_BOARD_COMMANDS);

	int advertised_count = 0;
	for (size_t i = 0; i < NUM_BOARD_COMMANDS; ++i) {
		int got = boardmodule_is_command_advertised(
			ALL_BOARD_COMMANDS[i].cmd_bit);
		TEST_ASSERT_EQ_INT(
			ALL_BOARD_COMMANDS[i].advertised, got);
		if (ALL_BOARD_COMMANDS[i].advertised) {
			++advertised_count;
		}
	}
	TEST_ASSERT_EQ_INT(20, advertised_count);
}

static void test_unimplemented_commands_not_advertised(void)
{
	/* Resource-transfer commands NOT implemented until later waves. */
	TEST_ASSERT_EQ_INT(0, boardmodule_is_command_advertised(
		CMD(WHAD_BOARD_CMD_GPIO_CONFIGURE)));
	TEST_ASSERT_EQ_INT(0, boardmodule_is_command_advertised(
		CMD(WHAD_BOARD_CMD_ADC_READ)));

	/* Storage commands NOT implemented until Todo 29. */
	TEST_ASSERT_EQ_INT(0, boardmodule_is_command_advertised(
		CMD(WHAD_BOARD_CMD_STORAGE_INFO)));
	TEST_ASSERT_EQ_INT(0, boardmodule_is_command_advertised(
		CMD(WHAD_BOARD_CMD_STORAGE_ADOPT)));
}

static void test_implemented_handlers_advertised(void)
{
	/* Base runtime handlers (always implemented): */
	TEST_ASSERT_EQ_INT(1, boardmodule_is_command_advertised(
		CMD(WHAD_BOARD_CMD_GET_BOARD_INFO)));
	TEST_ASSERT_EQ_INT(1, boardmodule_is_command_advertised(
		CMD(WHAD_BOARD_CMD_GET_RUNTIME_CONFIG)));
	TEST_ASSERT_EQ_INT(1, boardmodule_is_command_advertised(
		CMD(WHAD_BOARD_CMD_SET_RUNTIME_CONFIG)));
	TEST_ASSERT_EQ_INT(1, boardmodule_is_command_advertised(
		CMD(WHAD_BOARD_CMD_SET_RUNTIME_MODE)));
}

static void test_eval_set_runtime_mode_all_paths(void)
{
	/* All three runtime modes × persist flag: exhaustive matrix. */
	struct {
		uint32_t runtime;
		bool     persist;
		uint32_t expected;
	} cases[] = {
		{ BOARD_RT_RAW_WHAD, false, board_BoardResultCode_SUCCESS },
		{ BOARD_RT_RAW_WHAD, true,  board_BoardResultCode_NOT_ADOPTED },
		{ BOARD_RT_BLE_HID,  false, board_BoardResultCode_SUCCESS },
		{ BOARD_RT_BLE_HID,  true,  board_BoardResultCode_NOT_ADOPTED },
		{ BOARD_RT_UNKNOWN,  false, board_BoardResultCode_WRONG_MODE },
		{ BOARD_RT_UNKNOWN,  true,  board_BoardResultCode_WRONG_MODE },
	};

	for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
		uint32_t got = boardmodule_eval_set_runtime_mode(
			cases[i].runtime, cases[i].persist);
		TEST_ASSERT_EQ_INT((long long)cases[i].expected,
			(long long)got);
	}
}

static void test_eval_set_runtime_config_all_paths(void)
{
	/* All operation types × hasPersistedRuntime: exhaustive. */
	struct {
		uint32_t op;
		bool     has_persisted;
		uint32_t expected;
	} cases[] = {
		{ BOARD_CFG_OP_UPDATE,       false, board_BoardResultCode_SUCCESS },
		{ BOARD_CFG_OP_UPDATE,       true,  board_BoardResultCode_NOT_ADOPTED },
		{ BOARD_CFG_OP_OPEN_PAIRING, false, board_BoardResultCode_SUCCESS },
		{ BOARD_CFG_OP_OPEN_PAIRING, true,  board_BoardResultCode_SUCCESS },
		{ BOARD_CFG_OP_CLEAR_BONDS,  false, board_BoardResultCode_SUCCESS },
		{ BOARD_CFG_OP_CLEAR_BONDS,  true,  board_BoardResultCode_SUCCESS },
		{ 99,                        false, board_BoardResultCode_INVALID_ARGUMENT },
		{ 99,                        true,  board_BoardResultCode_INVALID_ARGUMENT },
	};

	for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); ++i) {
		uint32_t got = boardmodule_eval_set_runtime_config(
			cases[i].op, cases[i].has_persisted);
		TEST_ASSERT_EQ_INT((long long)cases[i].expected,
			(long long)got);
	}
}

static void test_runtime_mapping_roundtrip(void)
{
	/* runtime_mode_t → proto → runtime_mode_t must be identity
	 * for the two valid modes. */
	runtime_mode_t modes[] = { RUNTIME_RAW_WHAD, RUNTIME_BLE_HID };
	for (size_t i = 0; i < sizeof(modes)/sizeof(modes[0]); ++i) {
		uint32_t proto = boardmodule_rt_to_proto(modes[i]);
		bool valid = false;
		runtime_mode_t back = boardmodule_proto_to_rt(proto, &valid);
		TEST_ASSERT_EQ_INT(1, valid);
		TEST_ASSERT_EQ_INT((long long)modes[i], (long long)back);
	}

	/* UNKNOWN proto must produce invalid. */
	bool valid = true;
	(void)boardmodule_proto_to_rt(BOARD_RT_UNKNOWN, &valid);
	TEST_ASSERT_EQ_INT(0, valid);

	/* Out-of-range proto must produce invalid. */
	valid = true;
	(void)boardmodule_proto_to_rt(99, &valid);
	TEST_ASSERT_EQ_INT(0, valid);
}

static void test_non_clue_domain_board_absent(void)
{
	int count = 0;
	for (const whad_domain_desc_t *p = CAPABILITIES;
	     p->domain != DOMAIN_NONE; ++p) {
		TEST_ASSERT(p->domain != DOMAIN_BOARD,
			"non-CLUE must not include DOMAIN_BOARD");
		++count;
	}
	TEST_ASSERT_EQ_INT(5, count);
}

int main(void)
{
	test_framework_init();

	RUN_TEST(test_cmd_is_64bit);
	RUN_TEST(test_cmd_union_is_64bit);
	RUN_TEST(test_capabilities_raw_whad_has_board);
	RUN_TEST(test_capabilities_ble_hid_board_only);
	RUN_TEST(test_capabilities_raw_whad_has_six_domains);
	RUN_TEST(test_capabilities_non_clue_has_five_domains);
	RUN_TEST(test_capabilities_non_clue_no_board);
	RUN_TEST(test_rt_to_proto);
	RUN_TEST(test_proto_to_rt);
	RUN_TEST(test_eval_set_runtime_mode_success);
	RUN_TEST(test_eval_set_runtime_mode_not_adopted);
	RUN_TEST(test_eval_set_runtime_mode_wrong_mode);
	RUN_TEST(test_eval_set_runtime_config_pairing_bonds_success);
	RUN_TEST(test_eval_set_runtime_config_update_success);
	RUN_TEST(test_eval_set_runtime_config_update_not_adopted);
	RUN_TEST(test_eval_set_runtime_config_invalid);
	RUN_TEST(test_is_command_advertised);
	RUN_TEST(test_get_runtime_caps_raw_whad);
	RUN_TEST(test_get_runtime_caps_ble_hid);
	RUN_TEST(test_get_runtime_caps_raw_has_radio_domains);
	RUN_TEST(test_get_runtime_caps_ble_has_no_radio_domains);

	RUN_TEST(test_all_board_commands_classified);
	RUN_TEST(test_unimplemented_commands_not_advertised);
	RUN_TEST(test_implemented_handlers_advertised);
	RUN_TEST(test_eval_set_runtime_mode_all_paths);
	RUN_TEST(test_eval_set_runtime_config_all_paths);
	RUN_TEST(test_runtime_mapping_roundtrip);
	RUN_TEST(test_non_clue_domain_board_absent);

	return test_framework_finish();
}
