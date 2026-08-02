/* Linker script to configure memory regions. */

SEARCH_DIR(.)
GROUP(-lgcc -lc -lnosys)

MEMORY
{
  FLASH (rx) : ORIGIN = 0x26000, LENGTH = 0xba000  /* Above S140 6.1.1, stop before app data */
  RAM (rwx) :  ORIGIN = 0x20006000, LENGTH = 0x3a000  /* S140 6.1.1 RAM contract */
}

SECTIONS
{
}

SECTIONS
{
  . = ALIGN(4);
  .mem_section_dummy_ram :
  {
  }
  .log_dynamic_data :
  {
    PROVIDE(__start_log_dynamic_data = .);
    KEEP(*(SORT(.log_dynamic_data*)))
    PROVIDE(__stop_log_dynamic_data = .);
  } > RAM
  .log_filter_data :
  {
    PROVIDE(__start_log_filter_data = .);
    KEEP(*(SORT(.log_filter_data*)))
    PROVIDE(__stop_log_filter_data = .);
  } > RAM
  .cli_sorted_cmd_ptrs :
  {
    PROVIDE(__start_cli_sorted_cmd_ptrs = .);
    KEEP(*(.cli_sorted_cmd_ptrs))
    PROVIDE(__stop_cli_sorted_cmd_ptrs = .);
  } > RAM

} INSERT AFTER .data;

SECTIONS
{
  .mem_section_dummy_rom :
  {
  }
    .nrf_queue :
  {
    PROVIDE(__start_nrf_queue = .);
    KEEP(*(.nrf_queue))
    PROVIDE(__stop_nrf_queue = .);
  } > FLASH
  .log_const_data :
  {
    PROVIDE(__start_log_const_data = .);
    KEEP(*(SORT(.log_const_data*)))
    PROVIDE(__stop_log_const_data = .);
  } > FLASH
  .log_backends :
  {
    PROVIDE(__start_log_backends = .);
    KEEP(*(SORT(.log_backends*)))
    PROVIDE(__stop_log_backends = .);
  } > FLASH
    .cli_command :
  {
    PROVIDE(__start_cli_command = .);
    KEEP(*(.cli_command))
    PROVIDE(__stop_cli_command = .);
  } > FLASH
    .crypto_data :
  {
    PROVIDE(__start_crypto_data = .);
    KEEP(*(SORT(.crypto_data*)))
    PROVIDE(__stop_crypto_data = .);
  } > FLASH
  .pwr_mgmt_data :
  {
    PROVIDE(__start_pwr_mgmt_data = .);
    KEEP(*(SORT(.pwr_mgmt_data*)))
    PROVIDE(__stop_pwr_mgmt_data = .);
  } > FLASH
    .nrf_balloc :
  {
    PROVIDE(__start_nrf_balloc = .);
    KEEP(*(.nrf_balloc))
    PROVIDE(__stop_nrf_balloc = .);
  } > FLASH
  .sdh_ble_observers :
  {
    PROVIDE(__start_sdh_ble_observers = .);
    KEEP(*(SORT(.sdh_ble_observers*)))
    PROVIDE(__stop_sdh_ble_observers = .);
  } > FLASH
  .sdh_soc_observers :
  {
    PROVIDE(__start_sdh_soc_observers = .);
    KEEP(*(SORT(.sdh_soc_observers*)))
    PROVIDE(__stop_sdh_soc_observers = .);
  } > FLASH
  .sdh_req_observers :
  {
    PROVIDE(__start_sdh_req_observers = .);
    KEEP(*(SORT(.sdh_req_observers*)))
    PROVIDE(__stop_sdh_req_observers = .);
  } > FLASH
  .sdh_state_observers :
  {
    PROVIDE(__start_sdh_state_observers = .);
    KEEP(*(SORT(.sdh_state_observers*)))
    PROVIDE(__stop_sdh_state_observers = .);
  } > FLASH
  .sdh_stack_observers :
  {
    PROVIDE(__start_sdh_stack_observers = .);
    KEEP(*(SORT(.sdh_stack_observers*)))
    PROVIDE(__stop_sdh_stack_observers = .);
  } > FLASH

} INSERT AFTER .text

INCLUDE "nrf_common.ld"

__clue_app_origin = 0x26000;
__clue_app_end = 0xE0000;
__clue_app_data_start = 0xE0000;
__clue_fds_start = 0xF1000;
__clue_fds_end = 0xF4000;
__clue_bootloader_start = 0xF4000;
__clue_double_tap_word = 0x20007F7C;
__clue_bond_handoff_start = 0x20007F80;
__clue_bond_handoff_end = 0x20008000;
PROVIDE(__clue_retained_start = __clue_bond_handoff_end);
PROVIDE(__clue_retained_end = __clue_bond_handoff_end);

ASSERT(ORIGIN(FLASH) == __clue_app_origin, "CLUE app must start at 0x26000 after S140 6.1.1")
ASSERT((ORIGIN(FLASH) + LENGTH(FLASH)) <= __clue_app_end, "CLUE FLASH region overlaps app data or bootloader")
ASSERT((ORIGIN(FLASH) + TotalFlashUsed) <= __clue_app_end, "CLUE image overlaps 0xE0000-0xF0FFF app-data gap")
ASSERT(__clue_fds_start >= 0xF1000 && __clue_fds_end <= __clue_bootloader_start, "CLUE FDS partition must remain 0xF1000-0xF3FFF")
ASSERT(__clue_bootloader_start >= 0xF4000, "CLUE bootloader partition must start at or above 0xF4000")
ASSERT(ORIGIN(RAM) == 0x20006000, "CLUE RAM origin must match S140 6.1.1 baseline")
ASSERT(LENGTH(RAM) == 0x3A000, "CLUE RAM length must match S140 6.1.1 baseline")
ASSERT(__clue_double_tap_word >= ORIGIN(RAM) && __clue_bond_handoff_end <= (ORIGIN(RAM) + LENGTH(RAM)), "CLUE bootloader retained RAM markers must stay inside app RAM")
ASSERT(!DEFINED(__clue_retained_start) || ((__clue_retained_start >= __clue_bond_handoff_end) || (__clue_retained_end <= __clue_double_tap_word)), "CLUE retained/NOLOAD sections must not claim bootloader double-tap or bond handoff RAM")
