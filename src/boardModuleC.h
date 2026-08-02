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
 * Returns BoardResultCode:
 *   - update sub-op with has_persisted_runtime==true → NOT_ADOPTED
 *   - open_pairing_window sub-op → NOT_IMPLEMENTED (Todo 23)
 *   - clear_bonds sub-op → NOT_IMPLEMENTED (Todo 23)
 *   - update sub-op with volatile-only fields → SUCCESS
 */
uint32_t boardmodule_eval_set_runtime_config(
    uint32_t whichOperation, bool hasPersistedRuntime);

/* Check if a Board command bit is in the advertised bitmask. */
bool boardmodule_is_command_advertised(uint32_t commandBit);

#ifdef __cplusplus
}
#endif

#endif /* BOARD_MODULE_C_H */
