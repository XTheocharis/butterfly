/*
 * test_core_eval.cpp - Host tests for core_eval layer.
 *
 * Textually includes core_eval.c (pure-C, no SDK deps) per the
 * project's eval/C++ split convention. Mirrors the test_buzzer.cpp and
 * test_pdm.cpp patterns.
 *
 * Covers: protocol descriptor table lookups (BLE/DOT15D4/ESB/ANT/MOSART/
 * GENERIC/IDLE), per-protocol channel validation (BLE 0-39, dot15d4
 * 11-26, ESB 0-100 + 0xFF wildcard, unbounded PHY/ANT/MOSART), dot15d4
 * payload bounds, runtime mode validation, has_raw_radio predicate,
 * service init plan for RAW_WHAD / BLE_HID / invalid, domain routing
 * policy (board, discovery, generic, radio, unknown), version
 * compatibility.
 */
#include "test_framework.h"

#include "core_eval.h"

#include <string.h>
#include <stdint.h>

#include "../../src/core_eval.c"

/* ---- Protocol descriptor table lookups ------------------------------- */

static void lookup_ble_returns_blue_slot0_ch0_39(void)
{
	const core_protocol_descriptor_t *d = core_lookup_protocol(CORE_PROTOCOL_BLE);
	TEST_ASSERT(d != NULL, "BLE descriptor found");
	TEST_ASSERT_EQ_INT((int)CORE_COLOR_BLUE, (int)d->led_color);
	TEST_ASSERT_EQ_INT(0,  d->controller_slot);
	TEST_ASSERT_EQ_INT(0,  d->channel_min);
	TEST_ASSERT_EQ_INT(39, d->channel_max);
	TEST_ASSERT_EQ_INT(-1, d->channel_magic);
}

static void lookup_dot15d4_returns_green_slot1_ch11_26(void)
{
	const core_protocol_descriptor_t *d =
		core_lookup_protocol(CORE_PROTOCOL_DOT15D4);
	TEST_ASSERT(d != NULL, "dot15d4 descriptor found");
	TEST_ASSERT_EQ_INT((int)CORE_COLOR_GREEN, (int)d->led_color);
	TEST_ASSERT_EQ_INT(1,  d->controller_slot);
	TEST_ASSERT_EQ_INT(11, d->channel_min);
	TEST_ASSERT_EQ_INT(26, d->channel_max);
}

static void lookup_esb_has_0xff_magic_wildcard(void)
{
	const core_protocol_descriptor_t *d = core_lookup_protocol(CORE_PROTOCOL_ESB);
	TEST_ASSERT(d != NULL, "ESB descriptor found");
	TEST_ASSERT_EQ_INT(0xFF, d->channel_magic);
	TEST_ASSERT_EQ_INT(0,   d->channel_min);
	TEST_ASSERT_EQ_INT(100, d->channel_max);
}

static void lookup_idle_returns_descriptor_with_slot_minus_one(void)
{
	const core_protocol_descriptor_t *d = core_lookup_protocol(CORE_PROTOCOL_IDLE);
	TEST_ASSERT(d != NULL, "IDLE descriptor found");
	TEST_ASSERT_EQ_INT(-1, d->controller_slot);
}

static void lookup_unknown_returns_null(void)
{
	core_protocol_t bogus = (core_protocol_t)0xAB;
	TEST_ASSERT(core_lookup_protocol(bogus) == NULL, "0xAB unknown");
}

static void protocol_table_count_excludes_idle_sentinel(void)
{
	/* 6 populated entries: BLE, DOT15D4, ESB, ANT, MOSART, GENERIC. */
	TEST_ASSERT_EQ_INT(6u, core_protocol_table_count());
}

/* ---- Channel validation per protocol --------------------------------- */

static void is_valid_channel_ble_range(void)
{
	TEST_ASSERT(core_is_valid_channel(CORE_PROTOCOL_BLE, 0),
		"BLE ch 0 valid");
	TEST_ASSERT(core_is_valid_channel(CORE_PROTOCOL_BLE, 39),
		"BLE ch 39 valid");
	TEST_ASSERT(!core_is_valid_channel(CORE_PROTOCOL_BLE, 40),
		"BLE ch 40 invalid");
	TEST_ASSERT(!core_is_valid_channel(CORE_PROTOCOL_BLE, -1),
		"BLE ch -1 invalid");
}

static void is_valid_channel_dot15d4_range(void)
{
	TEST_ASSERT(core_is_valid_channel(CORE_PROTOCOL_DOT15D4, 11),
		"dot15d4 ch 11 valid");
	TEST_ASSERT(core_is_valid_channel(CORE_PROTOCOL_DOT15D4, 26),
		"dot15d4 ch 26 valid");
	TEST_ASSERT(!core_is_valid_channel(CORE_PROTOCOL_DOT15D4, 10),
		"dot15d4 ch 10 invalid");
	TEST_ASSERT(!core_is_valid_channel(CORE_PROTOCOL_DOT15D4, 27),
		"dot15d4 ch 27 invalid");
}

static void is_valid_channel_esb_range_with_0xff_wildcard(void)
{
	TEST_ASSERT(core_is_valid_channel(CORE_PROTOCOL_ESB, 0),
		"ESB ch 0 valid");
	TEST_ASSERT(core_is_valid_channel(CORE_PROTOCOL_ESB, 100),
		"ESB ch 100 valid");
	TEST_ASSERT(core_is_valid_channel(CORE_PROTOCOL_ESB, 0xFF),
		"ESB ch 0xFF wildcard valid");
	TEST_ASSERT(!core_is_valid_channel(CORE_PROTOCOL_ESB, 101),
		"ESB ch 101 invalid");
	TEST_ASSERT(!core_is_valid_channel(CORE_PROTOCOL_ESB, -1),
		"ESB ch -1 invalid");
}

static void is_valid_channel_generic_unbounded_accepts_non_negative(void)
{
	TEST_ASSERT(core_is_valid_channel(CORE_PROTOCOL_GENERIC, 0),
		"GENERIC ch 0 valid");
	TEST_ASSERT(core_is_valid_channel(CORE_PROTOCOL_GENERIC, 9999),
		"GENERIC ch 9999 valid (unbounded)");
	TEST_ASSERT(!core_is_valid_channel(CORE_PROTOCOL_GENERIC, -1),
		"GENERIC ch -1 invalid");
}

/* ---- Dot15d4 payload bounds ----------------------------------------- */

static void dot15d4_send_pdu_fits_one_byte_header(void)
{
	TEST_ASSERT(core_dot15d4_send_pdu_fits(/*pdu=*/10, /*slot=*/11),
		"10 + 1-byte hdr fits in 11");
	TEST_ASSERT(!core_dot15d4_send_pdu_fits(/*pdu=*/10, /*slot=*/10),
		"10 + 1-byte hdr does NOT fit in 10");
}

static void dot15d4_send_raw_pdu_fits_three_byte_header(void)
{
	TEST_ASSERT(core_dot15d4_send_raw_pdu_fits(/*pdu=*/10, /*slot=*/13),
		"10 + 3-byte hdr fits in 13");
	TEST_ASSERT(!core_dot15d4_send_raw_pdu_fits(/*pdu=*/10, /*slot=*/12),
		"10 + 3-byte hdr does NOT fit in 12");
}

/* ---- Runtime predicates --------------------------------------------- */

static void runtime_is_valid_for_both_modes(void)
{
	TEST_ASSERT(core_runtime_is_valid(CORE_RUNTIME_RAW_WHAD),
		"RAW_WHAD valid");
	TEST_ASSERT(core_runtime_is_valid(CORE_RUNTIME_BLE_HID),
		"BLE_HID valid");
	core_runtime_mode_t bogus = (core_runtime_mode_t)99;
	TEST_ASSERT(!core_runtime_is_valid(bogus),
		"bogus mode invalid");
}

static void runtime_has_raw_radio_requires_raw_mode_and_radio_present(void)
{
	TEST_ASSERT(core_runtime_has_raw_radio(CORE_RUNTIME_RAW_WHAD,
		/*radio_present=*/true), "raw + present");
	TEST_ASSERT(!core_runtime_has_raw_radio(CORE_RUNTIME_RAW_WHAD,
		/*radio_present=*/false), "raw + absent");
	TEST_ASSERT(!core_runtime_has_raw_radio(CORE_RUNTIME_BLE_HID,
		/*radio_present=*/true), "BLE + present (radio not constructed)");
}

/* ---- Service initialization plan ------------------------------------ */

static void plan_for_raw_whad_with_board(void)
{
	core_service_plan_t p =
		core_plan_for_runtime(CORE_RUNTIME_RAW_WHAD, /*board=*/true);
	TEST_ASSERT(p.has_radio,             "radio constructed");
	TEST_ASSERT(p.has_raw_controllers,   "raw controllers constructed");
	TEST_ASSERT(p.has_sercomm_at_ctor,   "SerialComm at ctor (raw)");
	TEST_ASSERT(p.has_usb_cdc,           "CDC runs after init");
	TEST_ASSERT(p.has_board_module,      "BoardModule on CLUE");
	TEST_ASSERT(!p.has_ble_runtime,      "no BleRuntime in raw mode");
}

static void plan_for_ble_hid_with_board(void)
{
	core_service_plan_t p =
		core_plan_for_runtime(CORE_RUNTIME_BLE_HID, /*board=*/true);
	TEST_ASSERT(!p.has_radio,            "radio NOT constructed");
	TEST_ASSERT(!p.has_raw_controllers,  "no raw controllers");
	TEST_ASSERT(!p.has_sercomm_at_ctor,  "SerialComm deferred to init");
	TEST_ASSERT(p.has_usb_cdc,           "CDC runs after init");
	TEST_ASSERT(p.has_board_module,      "BoardModule on CLUE");
	TEST_ASSERT(p.has_ble_runtime,       "BleRuntime constructed on CLUE");
}

static void plan_for_raw_whad_without_board(void)
{
	core_service_plan_t p =
		core_plan_for_runtime(CORE_RUNTIME_RAW_WHAD, /*board=*/false);
	TEST_ASSERT(p.has_radio,        "radio still constructed");
	TEST_ASSERT(!p.has_board_module, "no BoardModule on non-CLUE");
	TEST_ASSERT(!p.has_ble_runtime, "no BleRuntime");
}

static void plan_for_invalid_mode_is_all_false_except_board(void)
{
	core_runtime_mode_t bogus = (core_runtime_mode_t)99;
	core_service_plan_t p = core_plan_for_runtime(bogus, /*board=*/true);
	TEST_ASSERT(!p.has_radio,           "no radio");
	TEST_ASSERT(!p.has_raw_controllers, "no controllers");
	TEST_ASSERT(!p.has_ble_runtime,     "no BLE runtime");
	TEST_ASSERT(!p.has_usb_cdc,         "no CDC");
	TEST_ASSERT(p.has_board_module,     "BoardModule still constructed on CLUE");
}

/* ---- Domain routing policy ------------------------------------------ */

static void route_board_routes_to_board_module_on_clue(void)
{
	TEST_ASSERT_EQ_INT((int)CORE_ROUTE_BOARD_MODULE,
		(int)core_route_domain(CORE_DOMAIN_BOARD,
			CORE_RUNTIME_RAW_WHAD, /*radio=*/true, /*board=*/true));
	TEST_ASSERT_EQ_INT((int)CORE_ROUTE_BOARD_MODULE,
		(int)core_route_domain(CORE_DOMAIN_BOARD,
			CORE_RUNTIME_BLE_HID, /*radio=*/false, /*board=*/true));
	TEST_ASSERT_EQ_INT((int)CORE_ROUTE_REJECT,
		(int)core_route_domain(CORE_DOMAIN_BOARD,
			CORE_RUNTIME_RAW_WHAD, /*radio=*/true, /*board=*/false));
}

static void route_radio_domains_require_raw_mode_and_radio_present(void)
{
	TEST_ASSERT_EQ_INT((int)CORE_ROUTE_RAW_RADIO,
		(int)core_route_domain(CORE_DOMAIN_BLE,
			CORE_RUNTIME_RAW_WHAD, /*radio=*/true, /*board=*/true));
	TEST_ASSERT_EQ_INT((int)CORE_ROUTE_REJECT,
		(int)core_route_domain(CORE_DOMAIN_BLE,
			CORE_RUNTIME_BLE_HID, /*radio=*/false, /*board=*/true));
	TEST_ASSERT_EQ_INT((int)CORE_ROUTE_REJECT,
		(int)core_route_domain(CORE_DOMAIN_PHY,
			CORE_RUNTIME_RAW_WHAD, /*radio=*/false, /*board=*/true));
	TEST_ASSERT_EQ_INT((int)CORE_ROUTE_RAW_RADIO,
		(int)core_route_domain(CORE_DOMAIN_DOT15D4,
			CORE_RUNTIME_RAW_WHAD, /*radio=*/true, /*board=*/false));
}

static void route_discovery_and_generic_always_inline(void)
{
	TEST_ASSERT_EQ_INT((int)CORE_ROUTE_DISCOVERY,
		(int)core_route_domain(CORE_DOMAIN_DISCOVERY,
			CORE_RUNTIME_BLE_HID, /*radio=*/false, /*board=*/true));
	TEST_ASSERT_EQ_INT((int)CORE_ROUTE_GENERIC,
		(int)core_route_domain(CORE_DOMAIN_GENERIC,
			CORE_RUNTIME_RAW_WHAD, /*radio=*/false, /*board=*/false));
}

static void route_unknown_domain_rejected(void)
{
	TEST_ASSERT_EQ_INT((int)CORE_ROUTE_REJECT,
		(int)core_route_domain(CORE_DOMAIN_UNKNOWN,
			CORE_RUNTIME_RAW_WHAD, /*radio=*/true, /*board=*/true));
}

static void routes_to_raw_radio_predicate_matches_route_decision(void)
{
	TEST_ASSERT(core_domain_routes_to_raw_radio(CORE_DOMAIN_ESB,
		CORE_RUNTIME_RAW_WHAD, /*radio=*/true),
		"ESB routes to raw radio in raw mode");
	TEST_ASSERT(!core_domain_routes_to_raw_radio(CORE_DOMAIN_ESB,
		CORE_RUNTIME_BLE_HID, /*radio=*/false),
		"ESB does NOT route to raw radio in BLE mode");
}

/* ---- Version compatibility ------------------------------------------ */

static void version_compatible_when_query_at_least_min(void)
{
	TEST_ASSERT(core_version_is_compatible(/*query=*/10, /*min=*/5),
		"newer compatible");
	TEST_ASSERT(core_version_is_compatible(/*query=*/5, /*min=*/5),
		"equal compatible");
	TEST_ASSERT(!core_version_is_compatible(/*query=*/4, /*min=*/5),
		"older incompatible");
}

/* ---- MAIN ------------------------------------------------------------ */

int main(void)
{
	test_framework_init();

	/* Protocol descriptors */
	RUN_TEST(lookup_ble_returns_blue_slot0_ch0_39);
	RUN_TEST(lookup_dot15d4_returns_green_slot1_ch11_26);
	RUN_TEST(lookup_esb_has_0xff_magic_wildcard);
	RUN_TEST(lookup_idle_returns_descriptor_with_slot_minus_one);
	RUN_TEST(lookup_unknown_returns_null);
	RUN_TEST(protocol_table_count_excludes_idle_sentinel);

	/* Channel validation */
	RUN_TEST(is_valid_channel_ble_range);
	RUN_TEST(is_valid_channel_dot15d4_range);
	RUN_TEST(is_valid_channel_esb_range_with_0xff_wildcard);
	RUN_TEST(is_valid_channel_generic_unbounded_accepts_non_negative);

	/* Dot15d4 payload bounds */
	RUN_TEST(dot15d4_send_pdu_fits_one_byte_header);
	RUN_TEST(dot15d4_send_raw_pdu_fits_three_byte_header);

	/* Runtime predicates */
	RUN_TEST(runtime_is_valid_for_both_modes);
	RUN_TEST(runtime_has_raw_radio_requires_raw_mode_and_radio_present);

	/* Service plan */
	RUN_TEST(plan_for_raw_whad_with_board);
	RUN_TEST(plan_for_ble_hid_with_board);
	RUN_TEST(plan_for_raw_whad_without_board);
	RUN_TEST(plan_for_invalid_mode_is_all_false_except_board);

	/* Routing */
	RUN_TEST(route_board_routes_to_board_module_on_clue);
	RUN_TEST(route_radio_domains_require_raw_mode_and_radio_present);
	RUN_TEST(route_discovery_and_generic_always_inline);
	RUN_TEST(route_unknown_domain_rejected);
	RUN_TEST(routes_to_raw_radio_predicate_matches_route_decision);

	/* Version */
	RUN_TEST(version_compatible_when_query_at_least_min);

	return test_framework_finish();
}
