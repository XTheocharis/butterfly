#ifndef CAPABILITIES_H
#define CAPABILITIES_H

/* supported_commands is uint64_t — use 1ULL to avoid signed-int UB
 * for command values >= 31 (Todo 14 fix).
 *
 * domanis/board.h and discovery.h are included by the consuming source
 * files (core.cpp) via <whad.h> — do NOT re-include them here to
 * avoid circular deps. */
#define CMD(X) (1ULL << (X))


const whad_domain_desc_t CAPABILITIES[] = {
  {
    DOMAIN_BTLE,
    (whad_capability_t)(CAP_SNIFF | CAP_INJECT | CAP_HIJACK | CAP_SIMULATE_ROLE),
    (
      CMD(ble_BleCommand_SniffAdv) |
      CMD(ble_BleCommand_SniffConnReq) |
      CMD(ble_BleCommand_SniffActiveConn) |
      CMD(ble_BleCommand_Start) |
      CMD(ble_BleCommand_Stop) |
      CMD(ble_BleCommand_SendRawPDU) |
      CMD(ble_BleCommand_SendPDU) |
      CMD(ble_BleCommand_HijackMaster) |
      CMD(ble_BleCommand_HijackSlave) |
      CMD(ble_BleCommand_HijackBoth) |
      CMD(ble_BleCommand_CentralMode) |
      CMD(ble_BleCommand_PeripheralMode) |
      CMD(ble_BleCommand_SniffAccessAddress) |
      CMD(ble_BleCommand_ReactiveJam) |
      CMD(ble_BleCommand_ConnectTo) |
      CMD(ble_BleCommand_Disconnect) |
      CMD(ble_BleCommand_PrepareSequence) |
      CMD(ble_BleCommand_TriggerSequence) |
      CMD(ble_BleCommand_DeleteSequence) |
      CMD(ble_BleCommand_ScanMode) |
      CMD(ble_BleCommand_SetBdAddress) |
      CMD(ble_BleCommand_SetEncryption)

      )
  },
  {
    DOMAIN_DOT15D4,
    (whad_capability_t)(CAP_SNIFF | CAP_INJECT | CAP_JAM | CAP_SIMULATE_ROLE),
    (
      CMD(dot15d4_Dot15d4Command_Sniff) |
      CMD(dot15d4_Dot15d4Command_Jam) |
      CMD(dot15d4_Dot15d4Command_EnergyDetection) |
      CMD(dot15d4_Dot15d4Command_Send) |
      CMD(dot15d4_Dot15d4Command_Start) |
      CMD(dot15d4_Dot15d4Command_Stop) |
      CMD(dot15d4_Dot15d4Command_SetNodeAddress) |
      CMD(dot15d4_Dot15d4Command_EndDeviceMode) |
      CMD(dot15d4_Dot15d4Command_CoordinatorMode) |
      CMD(dot15d4_Dot15d4Command_RouterMode) |
      CMD(dot15d4_Dot15d4Command_ManInTheMiddle)
      )
  },
  {
    DOMAIN_ESB,
    (whad_capability_t)(discovery_Capability_Sniff | discovery_Capability_Inject | discovery_Capability_Jam | discovery_Capability_SimulateRole),
    (
      CMD(esb_ESBCommand_Sniff) |
      CMD(esb_ESBCommand_Send) |
      CMD(esb_ESBCommand_Start) |
      CMD(esb_ESBCommand_Stop) |
      CMD(esb_ESBCommand_SetNodeAddress) |
      CMD(esb_ESBCommand_PrimaryReceiverMode) |
      CMD(esb_ESBCommand_PrimaryTransmitterMode)

      )
  },
  {
    DOMAIN_LOGITECH_UNIFYING,
    (whad_capability_t)(discovery_Capability_Sniff | discovery_Capability_Inject | discovery_Capability_Jam | discovery_Capability_SimulateRole),
    (
      CMD(unifying_UnifyingCommand_Sniff) |
      CMD(unifying_UnifyingCommand_Send) |
      CMD(unifying_UnifyingCommand_Start) |
      CMD(unifying_UnifyingCommand_Stop) |
      CMD(unifying_UnifyingCommand_SetNodeAddress) |
      CMD(unifying_UnifyingCommand_LogitechDongleMode) |
      CMD(unifying_UnifyingCommand_LogitechKeyboardMode) |
      CMD(unifying_UnifyingCommand_LogitechMouseMode) |
      CMD(unifying_UnifyingCommand_SniffPairing)


      )
  },
  {DOMAIN_PHY,
  (whad_capability_t)(discovery_Capability_Sniff | discovery_Capability_Inject | discovery_Capability_Jam | discovery_Capability_NoRawData),
  (
    CMD(phy_PhyCommand_SetGFSKModulation) |
    CMD(phy_PhyCommand_GetSupportedFrequencies) |
    CMD(phy_PhyCommand_SetFrequency) |
    CMD(phy_PhyCommand_SetDataRate) |
    CMD(phy_PhyCommand_SetEndianness) |
    CMD(phy_PhyCommand_SetTXPower) |
    CMD(phy_PhyCommand_SetPacketSize) |
    CMD(phy_PhyCommand_SetSyncWord) |
    CMD(phy_PhyCommand_Sniff) |
    CMD(phy_PhyCommand_Send) |
    CMD(phy_PhyCommand_Start) |
    CMD(phy_PhyCommand_Stop) |
    CMD(phy_PhyCommand_Jam) |
    CMD(phy_PhyCommand_Monitor)

    )
},
  {DOMAIN_NONE, CAP_NONE, 0x00000000}
};

#ifdef BOARD_CLUE

/* ---- Board domain (CLUE only) ----------------------------------------
 *
 * Only handlers live in the current build are advertised.
 * Update incrementally as each handler is implemented.
 *
 * Includes dominas/board.h to bring in WHAD_BOARD_CMD_* constants.
 * The main header (whad.h) has already been included by the time any
 * source includes capabilities.h, so the enum definitions are available
 * but the include guard inside this header is safe. */
#include "domains/board.h"

#define BOARD_ADVERTISED_COMMANDS ( \
      CMD(WHAD_BOARD_CMD_GET_BOARD_INFO)    | \
      CMD(WHAD_BOARD_CMD_GET_RUNTIME_CONFIG)| \
      CMD(WHAD_BOARD_CMD_SET_RUNTIME_MODE)  | \
      CMD(WHAD_BOARD_CMD_SET_RUNTIME_CONFIG)  | \
      CMD(WHAD_BOARD_CMD_LIST_SENSORS)        | \
      CMD(WHAD_BOARD_CMD_READ_SENSOR)         | \
      CMD(WHAD_BOARD_CMD_CONFIGURE_STREAM)    | \
      CMD(WHAD_BOARD_CMD_STOP_STREAM)         | \
      CMD(WHAD_BOARD_CMD_CALIBRATE)           | \
      CMD(WHAD_BOARD_CMD_GET_CALIBRATION)       \
      | CMD(WHAD_BOARD_CMD_GET_INPUT_STATE)  \
      | CMD(WHAD_BOARD_CMD_CONFIGURE_INPUT)  \
      | CMD(WHAD_BOARD_CMD_REMOTE_PROFILE_GET) \
      | CMD(WHAD_BOARD_CMD_REMOTE_PROFILE_SET) \
      | CMD(WHAD_BOARD_CMD_AUDIO_CONFIGURE)  \
      | CMD(WHAD_BOARD_CMD_RAW_PCM_DIAGNOSTICS) \
      | CMD(WHAD_BOARD_CMD_SET_OUTPUT) \
      | CMD(WHAD_BOARD_CMD_I2C_TRANSFER) \
      | CMD(WHAD_BOARD_CMD_SPI_TRANSFER) \
      | CMD(WHAD_BOARD_CMD_RELEASE_PIN) \
      | CMD(WHAD_BOARD_CMD_GPIO_CONFIGURE) \
      | CMD(WHAD_BOARD_CMD_GPIO_READ) \
      | CMD(WHAD_BOARD_CMD_GPIO_WRITE) \
      | CMD(WHAD_BOARD_CMD_ADC_READ) \
      | CMD(WHAD_BOARD_CMD_STORAGE_INFO) \
      | CMD(WHAD_BOARD_CMD_STORAGE_ADOPT) \
      | CMD(WHAD_BOARD_CMD_STORAGE_READ_LOG) \
      | CMD(WHAD_BOARD_CMD_STORAGE_ERASE_LOG) \
)

#define BOARD_CAPABILITIES ((whad_capability_t)(CAP_READ | CAP_WRITE))

/* Raw-WHAD: five radio domains + Board. */
const whad_domain_desc_t CAPABILITIES_RAW_WHAD[] = {
  {
    DOMAIN_BTLE,
    (whad_capability_t)(CAP_SNIFF | CAP_INJECT | CAP_HIJACK | CAP_SIMULATE_ROLE),
    (
      CMD(ble_BleCommand_SniffAdv) |
      CMD(ble_BleCommand_SniffConnReq) |
      CMD(ble_BleCommand_SniffActiveConn) |
      CMD(ble_BleCommand_Start) |
      CMD(ble_BleCommand_Stop) |
      CMD(ble_BleCommand_SendRawPDU) |
      CMD(ble_BleCommand_SendPDU) |
      CMD(ble_BleCommand_HijackMaster) |
      CMD(ble_BleCommand_HijackSlave) |
      CMD(ble_BleCommand_HijackBoth) |
      CMD(ble_BleCommand_CentralMode) |
      CMD(ble_BleCommand_PeripheralMode) |
      CMD(ble_BleCommand_SniffAccessAddress) |
      CMD(ble_BleCommand_ReactiveJam) |
      CMD(ble_BleCommand_ConnectTo) |
      CMD(ble_BleCommand_Disconnect) |
      CMD(ble_BleCommand_PrepareSequence) |
      CMD(ble_BleCommand_TriggerSequence) |
      CMD(ble_BleCommand_DeleteSequence) |
      CMD(ble_BleCommand_ScanMode) |
      CMD(ble_BleCommand_SetBdAddress) |
      CMD(ble_BleCommand_SetEncryption)
    )
  },
  {
    DOMAIN_DOT15D4,
    (whad_capability_t)(CAP_SNIFF | CAP_INJECT | CAP_JAM | CAP_SIMULATE_ROLE),
    (
      CMD(dot15d4_Dot15d4Command_Sniff) |
      CMD(dot15d4_Dot15d4Command_Jam) |
      CMD(dot15d4_Dot15d4Command_EnergyDetection) |
      CMD(dot15d4_Dot15d4Command_Send) |
      CMD(dot15d4_Dot15d4Command_Start) |
      CMD(dot15d4_Dot15d4Command_Stop) |
      CMD(dot15d4_Dot15d4Command_SetNodeAddress) |
      CMD(dot15d4_Dot15d4Command_EndDeviceMode) |
      CMD(dot15d4_Dot15d4Command_CoordinatorMode) |
      CMD(dot15d4_Dot15d4Command_RouterMode) |
      CMD(dot15d4_Dot15d4Command_ManInTheMiddle)
    )
  },
  {
    DOMAIN_ESB,
    (whad_capability_t)(discovery_Capability_Sniff | discovery_Capability_Inject | discovery_Capability_Jam | discovery_Capability_SimulateRole),
    (
      CMD(esb_ESBCommand_Sniff) |
      CMD(esb_ESBCommand_Send) |
      CMD(esb_ESBCommand_Start) |
      CMD(esb_ESBCommand_Stop) |
      CMD(esb_ESBCommand_SetNodeAddress) |
      CMD(esb_ESBCommand_PrimaryReceiverMode) |
      CMD(esb_ESBCommand_PrimaryTransmitterMode)
    )
  },
  {
    DOMAIN_LOGITECH_UNIFYING,
    (whad_capability_t)(discovery_Capability_Sniff | discovery_Capability_Inject | discovery_Capability_Jam | discovery_Capability_SimulateRole),
    (
      CMD(unifying_UnifyingCommand_Sniff) |
      CMD(unifying_UnifyingCommand_Send) |
      CMD(unifying_UnifyingCommand_Start) |
      CMD(unifying_UnifyingCommand_Stop) |
      CMD(unifying_UnifyingCommand_SetNodeAddress) |
      CMD(unifying_UnifyingCommand_LogitechDongleMode) |
      CMD(unifying_UnifyingCommand_LogitechKeyboardMode) |
      CMD(unifying_UnifyingCommand_LogitechMouseMode) |
      CMD(unifying_UnifyingCommand_SniffPairing)
    )
  },
  {
    DOMAIN_PHY,
    (whad_capability_t)(discovery_Capability_Sniff | discovery_Capability_Inject | discovery_Capability_Jam | discovery_Capability_NoRawData),
    (
      CMD(phy_PhyCommand_SetGFSKModulation) |
      CMD(phy_PhyCommand_GetSupportedFrequencies) |
      CMD(phy_PhyCommand_SetFrequency) |
      CMD(phy_PhyCommand_SetDataRate) |
      CMD(phy_PhyCommand_SetEndianness) |
      CMD(phy_PhyCommand_SetTXPower) |
      CMD(phy_PhyCommand_SetPacketSize) |
      CMD(phy_PhyCommand_SetSyncWord) |
      CMD(phy_PhyCommand_Sniff) |
      CMD(phy_PhyCommand_Send) |
      CMD(phy_PhyCommand_Start) |
      CMD(phy_PhyCommand_Stop) |
      CMD(phy_PhyCommand_Jam) |
      CMD(phy_PhyCommand_Monitor)
    )
  },
  {
    DOMAIN_BOARD,
    BOARD_CAPABILITIES,
    BOARD_ADVERTISED_COMMANDS
  },
  {DOMAIN_NONE, CAP_NONE, 0ULL}
};

/* BLE-HID: Board only (radio owned by SoftDevice). */
const whad_domain_desc_t CAPABILITIES_BLE_HID[] = {
  {
    DOMAIN_BOARD,
    BOARD_CAPABILITIES,
    BOARD_ADVERTISED_COMMANDS
  },
  {DOMAIN_NONE, CAP_NONE, 0ULL}
};

/* Runtime-aware capability selector — declaration.
 * Defined in boardModule.cpp to avoid pulling runtime.h into every
 * includer of capabilities.h. Core::processDiscoveryInputMessage
 * calls through this pointer. */
#ifdef __cplusplus
extern "C" {
#endif
const whad_domain_desc_t *getRuntimeCapabilities(runtime_mode_t mode);
#ifdef __cplusplus
}
#endif

#endif /* BOARD_CLUE */

const whad_phy_frequency_range_t SUPPORTED_FREQUENCY_RANGES[]  = {
    {2400000000, 2500000000},
    {0, 0}
};
#endif
