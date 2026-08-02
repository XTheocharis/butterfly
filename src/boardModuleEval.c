/*
 * boardModuleEval.c - Pure-logic Board domain evaluation functions.
 *
 * No SDK deps — only board.pb.h and runtime.h. Included from
 * boardModule.cpp (firmware) and test_board_module.cpp (host test).
 * Functions:
 *   boardmodule_rt_to_proto / boardmodule_proto_to_rt
 *   boardmodule_eval_set_runtime_mode
 *   boardmodule_eval_set_runtime_config
 *   boardmodule_is_command_advertised
 *   getRuntimeCapabilities (when BOARD_CLUE is defined)
 */
#include "boardModuleC.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t boardmodule_rt_to_proto(runtime_mode_t mode)
{
	switch (mode) {
	case RUNTIME_RAW_WHAD: return BOARD_RT_RAW_WHAD;
	case RUNTIME_BLE_HID:  return BOARD_RT_BLE_HID;
	default:               return BOARD_RT_UNKNOWN;
	}
}

runtime_mode_t boardmodule_proto_to_rt(uint32_t proto, bool *out_valid)
{
	switch (proto) {
	case BOARD_RT_RAW_WHAD:
		*out_valid = true;
		return RUNTIME_RAW_WHAD;
	case BOARD_RT_BLE_HID:
		*out_valid = true;
		return RUNTIME_BLE_HID;
	default:
		*out_valid = false;
		return RUNTIME_RAW_WHAD;
	}
}

uint32_t boardmodule_eval_set_runtime_mode(
	uint32_t protoRuntime, bool persist)
{
	if (protoRuntime == BOARD_RT_UNKNOWN) {
		return board_BoardResultCode_WRONG_MODE;
	}
	if (persist) {
		return board_BoardResultCode_NOT_ADOPTED;
	}
	return board_BoardResultCode_SUCCESS;
}

uint32_t boardmodule_eval_set_runtime_config(
	uint32_t whichOperation, bool hasPersistedRuntime)
{
	switch (whichOperation) {
	case BOARD_CFG_OP_OPEN_PAIRING:
		return board_BoardResultCode_SUCCESS;
	case BOARD_CFG_OP_CLEAR_BONDS:
		return board_BoardResultCode_SUCCESS;
	case BOARD_CFG_OP_UPDATE:
		if (hasPersistedRuntime) {
			return board_BoardResultCode_NOT_ADOPTED;
		}
		return board_BoardResultCode_SUCCESS;
	default:
		return board_BoardResultCode_INVALID_ARGUMENT;
	}
}

bool boardmodule_is_command_advertised(uint32_t commandBit)
{
#ifdef BOARD_CLUE
	return (commandBit & BOARD_ADVERTISED_COMMANDS) != 0U;
#else
	(void)commandBit;
	return false;
#endif
}

#ifdef BOARD_CLUE

#include "capabilities.h"

const whad_domain_desc_t *getRuntimeCapabilities(runtime_mode_t mode)
{
	return (mode == RUNTIME_BLE_HID) ? CAPABILITIES_BLE_HID
	                                 : CAPABILITIES_RAW_WHAD;
}

#endif /* BOARD_CLUE */

#ifdef __cplusplus
}
#endif
