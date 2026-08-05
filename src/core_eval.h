/*
 * core_eval.h - Pure-logic core orchestrator evaluation (host-testable).
 *
 * Mirrors the decision logic of core.{cpp,h} without any SDK or whad
 * dependency. Extracts the testable policy that lives inside Core:
 *
 *   - Protocol descriptor table (slot index, LED color, name, channel range)
 *     matching the switch in Core::selectController (core.cpp:1853-1918).
 *   - Per-protocol channel validation used by the BLE / 802.15.4 / ESB /
 *     Unifying message handlers before they touch the radio.
 *   - Dot15d4 Send/SendRawPdu payload bound check mirroring the
 *     `((N + size) > sizeof(packet))` guards at core.cpp:321,353.
 *   - Runtime mode predicate mirroring Core::hasRawRadio and the
 *     mode-gated construction branches in Core::Core / Core::init.
 *   - Service initialization plan: which subsystems exist for each
 *     runtime mode (data table behind the constructor switch).
 *   - Domain routing policy mirroring Core::processInputMessage
 *     (board domain always routed on CLUE, radio domains rejected
 *     when hasRawRadio() is false, generic/discovery always handled).
 *   - WHAD protocol version compatibility check mirroring the
 *     `query.getVersion() >= WHAD_MIN_VERSION` guard at core.cpp:124.
 *
 * Pure C (cc -std=c11). No SDK headers. Included from core.cpp
 * (firmware) and test_core.cpp (host test).
 *
 * The enums below mirror firmware values (radio_defs.h::Protocol,
 * runtime.h::runtime_mode_t, led.h::LedColor) — kept parallel on
 * purpose so the eval layer remains standalone. Any firmware change
 * to those enums MUST be reflected here (a test asserts the mapping).
 */
#ifndef CORE_EVAL_H
#define CORE_EVAL_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Protocol mirror (radio_defs.h::Protocol) ----------------------- */

typedef enum {
	CORE_PROTOCOL_BLE     = 0x00,
	CORE_PROTOCOL_DOT15D4 = 0x01,
	CORE_PROTOCOL_ESB     = 0x02,
	CORE_PROTOCOL_ANT     = 0x03,
	CORE_PROTOCOL_MOSART  = 0x04,
	CORE_PROTOCOL_GENERIC = 0xFF,
	/* Sentinel for "no controller selected" — matches the `else` branch
	 * of selectController which sets currentController = NULL. Distinct
	 * from CORE_PROTOCOL_GENERIC (0xFF) which is a real PHY controller. */
	CORE_PROTOCOL_IDLE    = 0xFE,
} core_protocol_t;

/* ---- Runtime mode mirror (runtime.h::runtime_mode_t) ---------------- */

typedef enum {
	CORE_RUNTIME_RAW_WHAD = 0,
	CORE_RUNTIME_BLE_HID  = 1,
} core_runtime_mode_t;

#define CORE_RUNTIME_MODE_COUNT 2u

/* ---- LED color mirror (led.h::LedColor) ----------------------------- */
/* Order MUST match the LedColor enum so a test can assert equality. */
typedef enum {
	CORE_COLOR_RED    = 0,
	CORE_COLOR_GREEN  = 1,
	CORE_COLOR_BLUE   = 2,
	CORE_COLOR_YELLOW = 3,
	CORE_COLOR_PURPLE = 4,
	CORE_COLOR_CYAN   = 5,
} core_led_color_t;

/* ---- Protocol descriptor -------------------------------------------- */
/*
 * Frozen table backing Core::selectController. `controller_slot` is the
 * index into Core's controller array (0=ble, 1=dot15d4, 2=esb, 3=ant,
 * 4=mosart, 5=generic). GENERIC is at slot 5 by convention even though
 * its enum value is 0xFF. IDLE is "no controller" and has slot -1.
 *
 * `channel_min` / `channel_max` reflect the per-protocol validity ranges
 * enforced by the message handlers in core.cpp BEFORE touching the radio.
 * A range of [-1,-1] means the protocol is not channel-bounded at this
 * layer (PHY uses frequency, ANT/MOSART inherit ESB's range at runtime).
 *
 * `channel_magic` is an out-of-range value that the handler accepts as a
 * special wildcard (ESB sniff-all uses 0xFF).
 */
typedef struct {
	core_protocol_t  protocol;
	const char      *name;            /* splash label, may be NULL */
	core_led_color_t led_color;
	int8_t           controller_slot; /* -1 = no controller (IDLE) */
	int16_t          channel_min;     /* -1 = no fixed lower bound */
	int16_t          channel_max;     /* -1 = no fixed upper bound */
	int16_t          channel_magic;   /* -1 = none */
} core_protocol_descriptor_t;

/* Returns pointer to the descriptor for `protocol`, or NULL if it does
 * not map to a slot in the table (CORE_PROTOCOL_IDLE returns a valid
 * descriptor with controller_slot == -1). */
const core_protocol_descriptor_t *core_lookup_protocol(core_protocol_t protocol);

/* Number of populated entries (excludes IDLE sentinel). */
uint32_t core_protocol_table_count(void);

/* ---- Channel validation --------------------------------------------- */
/*
 * Returns true if `channel` is acceptable for `protocol` according to
 * the firmware handler's range check (BLE 0-39, dot15d4 11-26,
 * ESB/Unifying 0-100 with 0xFF wildcard, others unbounded).
 *
 * For protocols with channel_min==-1 (unbounded), returns true for any
 * non-negative channel and false for negative values.
 */
bool core_is_valid_channel(core_protocol_t protocol, int channel);

/* ---- Dot15d4 payload bound ------------------------------------------ */
/*
 * core.cpp:320-323 builds `packet[0] = size; memcpy(packet+1, pdu, size)`
 * and rejects when (1 + size) > slot_size. Same shape for raw PDU with
 * a 3-byte header (size byte + 2-byte FCS) at core.cpp:353-356.
 */
#define CORE_DOT15D4_SEND_PDU_HEADER   1u
#define CORE_DOT15D4_SEND_RAW_HEADER   3u

bool core_dot15d4_send_pdu_fits(uint32_t pdu_size, uint32_t slot_size);
bool core_dot15d4_send_raw_pdu_fits(uint32_t pdu_size, uint32_t slot_size);

/* ---- Runtime predicates --------------------------------------------- */

/* True if `mode` is one of the two valid runtime modes. */
bool core_runtime_is_valid(core_runtime_mode_t mode);

/* Mirrors Core::hasRawRadio (core.cpp:1787-1789): raw radio is exposed
 * only in RAW_WHAD mode AND the radio peripheral was actually
 * constructed (firmware passes `radio != NULL` as `radio_present`). */
bool core_runtime_has_raw_radio(core_runtime_mode_t mode, bool radio_present);

/* ---- Service initialization plan ------------------------------------ */
/*
 * Data behind the mode-gated branches of Core::Core (core.cpp:1686-1731)
 * and Core::init (core.cpp:1824-1848). Tells host tests which subsystems
 * exist for a given (mode, board_available) without instantiating them.
 *
 * Mirrors the firmware's hard invariants:
 *   - radio + raw controllers exist only in RAW_WHAD.
 *   - BleRuntime exists only in BLE_HID.
 *   - BoardModule exists iff board_available (CLUE build).
 *   - SerialComm is constructed at the ctor in RAW_WHAD; deferred to
 *     init() in BLE_HID (sd_sercomm_at_ctor == false, but has_sercomm
 *     is true after init() completes).
 *   - USB CDC runs in both modes once init() returns.
 */
typedef struct {
	bool has_radio;
	bool has_raw_controllers;   /* BLE/DOT15D4/ESB/ANT/MOSART/GENERIC */
	bool has_ble_runtime;       /* BleRuntime constructed (CLUE only)   */
	bool has_board_module;      /* BoardModule constructed (CLUE only)  */
	bool has_sercomm_at_ctor;   /* SerialComm built in ctor (RAW only)  */
	bool has_usb_cdc;           /* CDC stack running after init()       */
} core_service_plan_t;

core_service_plan_t core_plan_for_runtime(core_runtime_mode_t mode,
                                          bool board_available);

/* ---- Domain routing policy ------------------------------------------ */
/*
 * Mirrors Core::processInputMessage (core.cpp:18-84). Given the message
 * domain, current runtime mode, and whether the board domain is
 * available on this build, returns the routing decision the firmware
 * makes BEFORE constructing any whad C++ message wrapper.
 */

typedef enum {
	CORE_DOMAIN_GENERIC    = 0,
	CORE_DOMAIN_DISCOVERY  = 1,
	CORE_DOMAIN_BLE        = 2,
	CORE_DOMAIN_DOT15D4    = 3,
	CORE_DOMAIN_ESB        = 4,
	CORE_DOMAIN_UNIFYING   = 5,
	CORE_DOMAIN_PHY        = 6,
	CORE_DOMAIN_BOARD      = 7,
	CORE_DOMAIN_UNKNOWN    = 8,
} core_domain_t;

typedef enum {
	CORE_ROUTE_REJECT          = 0, /* respond UnsupportedDomain / Error */
	CORE_ROUTE_BOARD_MODULE    = 1, /* hand to BoardModule (CLUE only)   */
	CORE_ROUTE_RAW_RADIO       = 2, /* hand to a radio controller        */
	CORE_ROUTE_DISCOVERY       = 3, /* handle inline (info/reset/query)  */
	CORE_ROUTE_GENERIC         = 4, /* handle inline (verbose/etc)       */
} core_domain_route_t;

/* Top-level routing for a message domain. Encodes the firmware rule:
 *   - Board domain (if available) routes to BoardModule in BOTH modes.
 *   - Discovery and Generic route inline in BOTH modes.
 *   - Radio domains (BLE/DOT15D4/ESB/UNIFYING/PHY) route to a raw
 *     controller only when hasRawRadio() is true; otherwise REJECT.
 *   - Unknown domains are REJECTed. */
core_domain_route_t core_route_domain(core_domain_t domain,
                                      core_runtime_mode_t mode,
                                      bool radio_present,
                                      bool board_available);

/* True if the given raw-radio domain is allowed given the current
 * runtime. Convenience predicate — equivalent to
 * (core_route_domain(...) == CORE_ROUTE_RAW_RADIO). */
bool core_domain_routes_to_raw_radio(core_domain_t domain,
                                     core_runtime_mode_t mode,
                                     bool radio_present);

/* ---- Version compatibility ------------------------------------------ */
/*
 * Mirrors the DeviceInfoQuery check at core.cpp:124-148. Returns true
 * if the host's reported WHAD protocol version is at least the
 * firmware's minimum. Equal versions are compatible.
 */
bool core_version_is_compatible(uint32_t query_version, uint32_t min_version);

#ifdef __cplusplus
}
#endif

#endif /* CORE_EVAL_H */
