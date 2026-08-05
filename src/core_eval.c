/*
 * core_eval.c - Pure-logic core orchestrator evaluation.
 *
 * Implementation companion to core_eval.h. No SDK deps, no whad deps.
 * Compiled from both core.cpp (firmware TU includes this .c via the
 * eval pattern) and the host test suite.
 *
 * The frozen tables here are the single source of truth mirrored FROM
 * core.cpp's switch statements. Any firmware change to the protocol
 * table, channel ranges, runtime plan, or routing policy MUST be
 * reflected here and locked by a test.
 */
#include <stddef.h>
#include "core_eval.h"

/* ---- Protocol descriptor table -------------------------------------- *
 * Mirrors Core::selectController (core.cpp:1859-1907). Order matches
 * the firmware switch for readability; lookup is linear (small table).
 */
static const core_protocol_descriptor_t kProtocolTable[] = {
	{
		CORE_PROTOCOL_BLE,
		"BLE",
		CORE_COLOR_BLUE,
		0,        /* controller slot */
		0, 39,    /* BLE channels 0-39 */
		-1        /* no magic */
	},
	{
		CORE_PROTOCOL_DOT15D4,
		"802.15.4",
		CORE_COLOR_GREEN,
		1,        /* controller slot */
		11, 26,   /* dot15d4 channels 11-26 */
		-1
	},
	{
		CORE_PROTOCOL_ESB,
		"ESB",
		CORE_COLOR_PURPLE,
		2,        /* controller slot */
		0, 100,   /* ESB channels 0-100 */
		0xFF      /* 0xFF = sniff-all wildcard */
	},
	{
		CORE_PROTOCOL_ANT,
		"ANT",
		CORE_COLOR_RED,
		3,        /* controller slot */
		-1, -1,   /* ANT inherits ESB range at runtime, no fixed bound here */
		-1
	},
	{
		CORE_PROTOCOL_MOSART,
		"MOSART",
		CORE_COLOR_YELLOW,
		4,        /* controller slot */
		-1, -1,   /* MOSART inherits ESB range at runtime */
		-1
	},
	{
		CORE_PROTOCOL_GENERIC,
		"PHY",
		CORE_COLOR_CYAN,
		5,        /* controller slot */
		-1, -1,   /* PHY uses frequency, not channel */
		-1
	},
};

/* Sentinel for the IDLE / "no controller" branch of selectController. */
static const core_protocol_descriptor_t kIdleDescriptor = {
	CORE_PROTOCOL_IDLE,
	"IDLE",
	CORE_COLOR_RED,     /* color is unused when controller_slot == -1 */
	-1,                 /* no controller */
	-1, -1,
	-1
};

const core_protocol_descriptor_t *core_lookup_protocol(core_protocol_t protocol)
{
	uint32_t i;
	uint32_t n = sizeof(kProtocolTable) / sizeof(kProtocolTable[0]);
	for (i = 0; i < n; i++) {
		if (kProtocolTable[i].protocol == protocol) {
			return &kProtocolTable[i];
		}
	}
	if (protocol == CORE_PROTOCOL_IDLE) {
		return &kIdleDescriptor;
	}
	return NULL;
}

uint32_t core_protocol_table_count(void)
{
	return sizeof(kProtocolTable) / sizeof(kProtocolTable[0]);
}

/* ---- Channel validation --------------------------------------------- */
bool core_is_valid_channel(core_protocol_t protocol, int channel)
{
	const core_protocol_descriptor_t *d = core_lookup_protocol(protocol);

	if (d == NULL) {
		return false;
	}
	if (channel < 0) {
		return false;
	}
	/* Magic wildcard always passes (ESB 0xFF sniff-all). */
	if (d->channel_magic >= 0 && channel == d->channel_magic) {
		return true;
	}
	/* Unbounded protocols (PHY / ANT / MOSART): accept any non-negative
	 * channel — the firmware does not pre-validate these at this layer. */
	if (d->channel_min < 0 || d->channel_max < 0) {
		return true;
	}
	return (channel >= d->channel_min && channel <= d->channel_max);
}

/* ---- Dot15d4 payload bound ------------------------------------------ */
bool core_dot15d4_send_pdu_fits(uint32_t pdu_size, uint32_t slot_size)
{
	/* Mirrors core.cpp:321 `if ((1 + size) > sizeof(packet))`. */
	return (pdu_size + CORE_DOT15D4_SEND_PDU_HEADER) <= slot_size;
}

bool core_dot15d4_send_raw_pdu_fits(uint32_t pdu_size, uint32_t slot_size)
{
	/* Mirrors core.cpp:353 `if ((3 + size) > sizeof(packet))`. */
	return (pdu_size + CORE_DOT15D4_SEND_RAW_HEADER) <= slot_size;
}

/* ---- Runtime predicates --------------------------------------------- */
bool core_runtime_is_valid(core_runtime_mode_t mode)
{
	return (mode == CORE_RUNTIME_RAW_WHAD || mode == CORE_RUNTIME_BLE_HID);
}

bool core_runtime_has_raw_radio(core_runtime_mode_t mode, bool radio_present)
{
	return (mode == CORE_RUNTIME_RAW_WHAD && radio_present);
}

/* ---- Service initialization plan ------------------------------------ */
core_service_plan_t core_plan_for_runtime(core_runtime_mode_t mode,
                                          bool board_available)
{
	core_service_plan_t plan;
	plan.has_radio             = false;
	plan.has_raw_controllers   = false;
	plan.has_ble_runtime       = false;
	plan.has_board_module      = false;
	plan.has_sercomm_at_ctor   = false;
	plan.has_usb_cdc           = false;

	/* BoardModule is constructed unconditionally on CLUE builds
	 * (core.cpp:1721) — independent of runtime mode. */
	plan.has_board_module = board_available;

	switch (mode) {
	case CORE_RUNTIME_RAW_WHAD:
		/* core.cpp:1686-1694, 1705-1706, 1822-1832. */
		plan.has_radio           = true;
		plan.has_raw_controllers = true;
		plan.has_sercomm_at_ctor = true;
		plan.has_usb_cdc         = true;
		/* BleRuntime stays NULL (core.cpp:1728-1730). */
		break;

	case CORE_RUNTIME_BLE_HID:
		/* core.cpp:1690-1694, 1707-1709, 1833-1847.
		 * Radio + raw controllers never constructed.
		 * SerialComm deferred from ctor to init() (after SD bring-up),
		 * so has_sercomm_at_ctor is false. After init() completes,
		 * USB CDC is running. BleRuntime only when board_available
		 * (CLUE) since main.cpp gates the BLE boot path on CLUE. */
		plan.has_radio           = false;
		plan.has_raw_controllers = false;
		plan.has_sercomm_at_ctor = false;
		plan.has_usb_cdc         = true;
		plan.has_ble_runtime     = board_available;
		break;

	default:
		/* Invalid mode — plan stays all-false (only board_available
		 * set above). Matches the firmware's defensive behavior. */
		break;
	}

	return plan;
}

/* ---- Domain routing policy ------------------------------------------ */
core_domain_route_t core_route_domain(core_domain_t domain,
                                      core_runtime_mode_t mode,
                                      bool radio_present,
                                      bool board_available)
{
	/* Board domain always routes to BoardModule on CLUE builds,
	 * independent of runtime mode (core.cpp:33-40). */
	if (domain == CORE_DOMAIN_BOARD) {
		return board_available ? CORE_ROUTE_BOARD_MODULE : CORE_ROUTE_REJECT;
	}

	/* Discovery and Generic are handled inline in both modes
	 * (core.cpp:23-29: GenericMsg / DiscoveryMsg cases precede the
	 * hasRawRadio gate). */
	if (domain == CORE_DOMAIN_DISCOVERY) {
		return CORE_ROUTE_DISCOVERY;
	}
	if (domain == CORE_DOMAIN_GENERIC) {
		return CORE_ROUTE_GENERIC;
	}

	/* Radio domains require hasRawRadio (core.cpp:46-50). */
	if (domain == CORE_DOMAIN_BLE ||
	    domain == CORE_DOMAIN_DOT15D4 ||
	    domain == CORE_DOMAIN_ESB ||
	    domain == CORE_DOMAIN_UNIFYING ||
	    domain == CORE_DOMAIN_PHY) {
		if (core_runtime_has_raw_radio(mode, radio_present)) {
			return CORE_ROUTE_RAW_RADIO;
		}
		return CORE_ROUTE_REJECT;
	}

	/* Unknown domain — firmware hits the default branch of the
	 * domain switch (core.cpp:74-76) which is a silent no-op; we
	 * surface that as REJECT so callers can produce UnsupportedDomain. */
	(void)radio_present;
	return CORE_ROUTE_REJECT;
}

bool core_domain_routes_to_raw_radio(core_domain_t domain,
                                     core_runtime_mode_t mode,
                                     bool radio_present)
{
	return core_route_domain(domain, mode, radio_present,
	                         /*board_available=*/false)
	       == CORE_ROUTE_RAW_RADIO;
}

/* ---- Version compatibility ------------------------------------------ */
bool core_version_is_compatible(uint32_t query_version, uint32_t min_version)
{
	return query_version >= min_version;
}
