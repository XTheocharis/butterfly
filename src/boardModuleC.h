/*
 * boardModuleC.h - Pure-C Board domain evaluation API.
 *
 * No SDK deps — safe to include from host tests. The BoardModule
 * C++ class (boardModule.h) builds on top of these declarations.
 */
#ifndef BOARD_MODULE_C_H
#define BOARD_MODULE_C_H

#include <stdint.h>
#include <stdbool.h>

/* Pull in board command/result enums and the runtime_mode_t. */
#include "runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Board message evaluation (pure logic, host-testable) ------------ */

/* board_RuntimeMode protobuf enum (board_RuntimeMode_*). */
#define BOARD_RT_UNKNOWN  board_RuntimeMode_RUNTIME_UNKNOWN
#define BOARD_RT_RAW_WHAD board_RuntimeMode_RUNTIME_RAW_WHAD
#define BOARD_RT_BLE_HID  board_RuntimeMode_RUNTIME_BLE_HID

/* SetRuntimeConfigRequest.operation oneof case values. */
#define BOARD_CFG_OP_UPDATE        1
#define BOARD_CFG_OP_OPEN_PAIRING  2
#define BOARD_CFG_OP_CLEAR_BONDS   3

/* Map runtime_mode_t to board_RuntimeMode protobuf enum. */
uint32_t boardmodule_rt_to_proto(runtime_mode_t mode);

/* Map board_RuntimeMode protobuf enum to runtime_mode_t.
 * Returns false in *out_valid if the proto mode is UNKNOWN. */
runtime_mode_t boardmodule_proto_to_rt(uint32_t proto, bool *out_valid);

/* Evaluate SetRuntimeMode request.
 *
 * Returns the BoardResultCode to send back:
 *   - WRONG_MODE if target runtime is RUNTIME_UNKNOWN
 *   - NOT_ADOPTED if persist==true (store unavailable until Todo 30)
 *   - SUCCESS otherwise (volatile mode change via GPREGRET2 one-shot)
 */
uint32_t boardmodule_eval_set_runtime_mode(
    uint32_t protoRuntime, bool persist);

/* Evaluate SetRuntimeConfig request.
 *
 * bleRuntimeActive selects between BLE-aware and BLE-absent behaviour:
 *   - OPEN_PAIRING/CLEAR_BONDS require an active BleRuntime → SUCCESS,
 *     otherwise NOT_IMPLEMENTED.
 *   - The UPDATE sub-op is BLE-agnostic.
 *
 * Returns BoardResultCode:
 *   - update sub-op with has_persisted_runtime==true → NOT_ADOPTED
 *   - open_pairing with bleRuntimeActive==true → SUCCESS (else NOT_IMPLEMENTED)
 *   - clear_bonds  with bleRuntimeActive==true → SUCCESS (else NOT_IMPLEMENTED)
 *   - update sub-op with volatile-only fields → SUCCESS
 */
uint32_t boardmodule_eval_set_runtime_config(
    uint32_t whichOperation, bool hasPersistedRuntime,
    bool bleRuntimeActive);

/* Runtime state for GetRuntimeConfig eval — host-testable (no SDK deps).
 * Uses uint32_t for enum fields to decouple from nanopb types. */
typedef struct {
	uint32_t active_runtime;       /* board_RuntimeMode enum value */
	uint32_t persisted_runtime;    /* board_RuntimeMode enum value */
	bool     persistence_available;
	bool     ble_advertising;
	bool     ble_pairable;
	bool     ble_connected;
	uint32_t bond_count;
	uint32_t event_log_filter;
	uint32_t raw_packet_log_filter;
} board_runtime_state_t;

/* Evaluate GetRuntimeConfig — validate/clamp state into response.
 *
 * - active_runtime >2 → clamped to BOARD_RT_UNKNOWN
 * - bond_count >255 → clamped to 255
 * - all other fields copied directly from *state
 */
void boardmodule_eval_get_runtime_config(
    const board_runtime_state_t *state,
    board_RuntimeConfigResponse *out);

/* Check if a Board command bit is in the advertised bitmask. */
bool boardmodule_is_command_advertised(uint32_t commandBit);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_MODULE_C_H */
