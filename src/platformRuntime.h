#ifndef PLATFORM_RUNTIME_H
#define PLATFORM_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "nrf.h"
#include "nrf_error.h"

#ifdef SOFTDEVICE_PRESENT
#include "nrf_nvic.h"
#include "nrf_soc.h"
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum platform_runtime_mode {
	PLATFORM_RUNTIME_RAW = 0,
	PLATFORM_RUNTIME_BLE = 1,
} platform_runtime_mode_t;

void platform_runtime_set_mode(platform_runtime_mode_t mode);
platform_runtime_mode_t platform_runtime_get_mode(void);
bool platform_runtime_softdevice_active(void);

uint32_t platform_runtime_last_error_get(void);
void platform_runtime_last_error_clear(void);

uint32_t platform_runtime_critical_region_enter(uint8_t *p_nested);
uint32_t platform_runtime_critical_region_exit(uint8_t nested);
uint32_t platform_runtime_event_wait(void);

uint32_t platform_runtime_nvic_enable_irq(IRQn_Type irq);
uint32_t platform_runtime_nvic_disable_irq(IRQn_Type irq);
uint32_t platform_runtime_nvic_clear_pending_irq(IRQn_Type irq);
uint32_t platform_runtime_nvic_set_priority(IRQn_Type irq, uint32_t priority);
void platform_runtime_system_reset(void);

uint32_t platform_runtime_power_gpregret_set(uint32_t gpregret_id, uint32_t mask);
uint32_t platform_runtime_power_gpregret_clr(uint32_t gpregret_id, uint32_t mask);
uint32_t platform_runtime_power_gpregret_get(uint32_t gpregret_id, uint32_t *p_value);
uint32_t platform_runtime_power_dcdc_mode_set(uint8_t dcdc_mode);
uint32_t platform_runtime_protected_register_write(volatile uint32_t *p_register, uint32_t value);

#ifdef __cplusplus
}
#endif

#endif
