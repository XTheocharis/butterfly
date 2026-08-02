#include "platformRuntime.h"

#include "app_util_platform.h"

#ifdef SOFTDEVICE_PRESENT
nrf_nvic_state_t nrf_nvic_state;
#endif

static volatile platform_runtime_mode_t m_runtime_mode = PLATFORM_RUNTIME_RAW;
static volatile uint32_t m_last_error = NRF_SUCCESS;
static uint32_t m_raw_critical_depth = 0;
static uint32_t m_raw_outer_primask = 0;

static void platform_runtime_record_error(uint32_t err_code) { if (err_code != NRF_SUCCESS) { m_last_error = err_code; } }
static bool platform_runtime_uses_softdevice(void) { return m_runtime_mode == PLATFORM_RUNTIME_BLE; }

void platform_runtime_set_mode(platform_runtime_mode_t mode) { m_runtime_mode = mode; }
platform_runtime_mode_t platform_runtime_get_mode(void) { return m_runtime_mode; }
bool platform_runtime_softdevice_active(void) { return platform_runtime_uses_softdevice(); }
uint32_t platform_runtime_last_error_get(void) { return m_last_error; }
void platform_runtime_last_error_clear(void) { m_last_error = NRF_SUCCESS; }

static uint32_t raw_critical_region_enter(uint8_t *p_nested)
{
	uint32_t primask = __get_PRIMASK();

	__disable_irq();
	if (p_nested != NULL)
	{
		*p_nested = (m_raw_critical_depth != 0) ? 1U : 0U;
	}
	if (m_raw_critical_depth == 0)
	{
		m_raw_outer_primask = primask;
	}
	m_raw_critical_depth++;

	return NRF_SUCCESS;
}

static uint32_t raw_critical_region_exit(void)
{
	if (m_raw_critical_depth == 0)
	{
		return NRF_ERROR_INVALID_STATE;
	}

	m_raw_critical_depth--;
	if ((m_raw_critical_depth == 0) && (m_raw_outer_primask == 0))
	{
		__enable_irq();
	}

	return NRF_SUCCESS;
}

uint32_t platform_runtime_critical_region_enter(uint8_t *p_nested)
{
	uint32_t err_code;

	if (!platform_runtime_uses_softdevice())
	{
		return raw_critical_region_enter(p_nested);
	}

#ifdef SOFTDEVICE_PRESENT
	uint8_t nested = 0;
	uint8_t *nested_ptr = (p_nested != NULL) ? p_nested : &nested;
	err_code = sd_nvic_critical_region_enter(nested_ptr);
#else
	err_code = NRF_ERROR_INVALID_STATE;
#endif
	platform_runtime_record_error(err_code);
	return err_code;
}

uint32_t platform_runtime_critical_region_exit(uint8_t nested)
{
	uint32_t err_code;

	if (!platform_runtime_uses_softdevice())
	{
		err_code = raw_critical_region_exit();
		platform_runtime_record_error(err_code);
		return err_code;
	}

#ifdef SOFTDEVICE_PRESENT
	err_code = sd_nvic_critical_region_exit(nested);
#else
	err_code = NRF_ERROR_INVALID_STATE;
#endif
	platform_runtime_record_error(err_code);
	return err_code;
}

uint32_t platform_runtime_event_wait(void)
{
	uint32_t err_code;

	if (!platform_runtime_uses_softdevice())
	{
		__WFE();
		return NRF_SUCCESS;
	}

#ifdef SOFTDEVICE_PRESENT
	err_code = sd_app_evt_wait();
#else
	err_code = NRF_ERROR_INVALID_STATE;
#endif
	platform_runtime_record_error(err_code);
	return err_code;
}

#ifdef SOFTDEVICE_PRESENT
#define PLATFORM_RUNTIME_SOFTDEVICE_CALL(call) (call)
#else
#define PLATFORM_RUNTIME_SOFTDEVICE_CALL(call) (NRF_ERROR_INVALID_STATE)
#endif

#define PLATFORM_RUNTIME_NVIC_WRAPPER(name, raw_call, softdevice_call) \
uint32_t name(IRQn_Type irq) \
{ \
	uint32_t err_code; \
	if (!platform_runtime_uses_softdevice()) \
	{ \
		raw_call; \
		return NRF_SUCCESS; \
	} \
	err_code = PLATFORM_RUNTIME_SOFTDEVICE_CALL(softdevice_call); \
	platform_runtime_record_error(err_code); \
	return err_code; \
}

PLATFORM_RUNTIME_NVIC_WRAPPER(platform_runtime_nvic_enable_irq, NVIC_EnableIRQ(irq), sd_nvic_EnableIRQ(irq))
PLATFORM_RUNTIME_NVIC_WRAPPER(platform_runtime_nvic_disable_irq, NVIC_DisableIRQ(irq), sd_nvic_DisableIRQ(irq))
PLATFORM_RUNTIME_NVIC_WRAPPER(platform_runtime_nvic_clear_pending_irq, NVIC_ClearPendingIRQ(irq), sd_nvic_ClearPendingIRQ(irq))

uint32_t platform_runtime_nvic_set_priority(IRQn_Type irq, uint32_t priority)
{
	uint32_t err_code;

	if (!platform_runtime_uses_softdevice())
	{
		NVIC_SetPriority(irq, priority);
		return NRF_SUCCESS;
	}
	err_code = PLATFORM_RUNTIME_SOFTDEVICE_CALL(sd_nvic_SetPriority(irq, priority));
	platform_runtime_record_error(err_code);
	return err_code;
}

void platform_runtime_system_reset(void)
{
	if (!platform_runtime_uses_softdevice())
	{
		NVIC_SystemReset();
		while (true) {}
	}

#ifdef SOFTDEVICE_PRESENT
	platform_runtime_record_error(sd_nvic_SystemReset());
#else
	platform_runtime_record_error(NRF_ERROR_INVALID_STATE);
#endif
	while (true) {}
}

static volatile uint32_t *raw_gpregret_register(uint32_t gpregret_id)
{
	if (gpregret_id == 0) { return &NRF_POWER->GPREGRET; }
	if (gpregret_id == 1) { return &NRF_POWER->GPREGRET2; }
	return NULL;
}

typedef enum gpregret_operation {
	GPREGRET_SET,
	GPREGRET_CLEAR,
} gpregret_operation_t;

static uint32_t platform_runtime_power_gpregret_apply(uint32_t gpregret_id,
											  uint32_t mask,
											  gpregret_operation_t operation)
{
	uint32_t err_code;

	if (!platform_runtime_uses_softdevice())
	{
		volatile uint32_t *p_register = raw_gpregret_register(gpregret_id);
		if (p_register == NULL)
		{
			return NRF_ERROR_INVALID_PARAM;
		}
		if (operation == GPREGRET_SET)
		{
			*p_register |= mask;
		}
		else
		{
			*p_register &= ~mask;
		}
		return NRF_SUCCESS;
	}

	if (operation == GPREGRET_SET)
	{
		err_code = PLATFORM_RUNTIME_SOFTDEVICE_CALL(sd_power_gpregret_set(gpregret_id, mask));
	}
	else
	{
		err_code = PLATFORM_RUNTIME_SOFTDEVICE_CALL(sd_power_gpregret_clr(gpregret_id, mask));
	}
	platform_runtime_record_error(err_code);
	return err_code;
}

uint32_t platform_runtime_power_gpregret_set(uint32_t gpregret_id, uint32_t mask)
{
	return platform_runtime_power_gpregret_apply(gpregret_id, mask, GPREGRET_SET);
}

uint32_t platform_runtime_power_gpregret_clr(uint32_t gpregret_id, uint32_t mask)
{
	return platform_runtime_power_gpregret_apply(gpregret_id, mask, GPREGRET_CLEAR);
}

uint32_t platform_runtime_power_gpregret_get(uint32_t gpregret_id, uint32_t *p_value)
{
	uint32_t err_code = NRF_SUCCESS;

	if (p_value == NULL)
	{
		return NRF_ERROR_NULL;
	}

	if (!platform_runtime_uses_softdevice())
	{
		volatile uint32_t *p_register = raw_gpregret_register(gpregret_id);
		if (p_register == NULL)
		{
			return NRF_ERROR_INVALID_PARAM;
		}
		*p_value = *p_register;
		return NRF_SUCCESS;
	}

	err_code = PLATFORM_RUNTIME_SOFTDEVICE_CALL(sd_power_gpregret_get(gpregret_id, p_value));
	platform_runtime_record_error(err_code);
	return err_code;
}

uint32_t platform_runtime_power_dcdc_mode_set(uint8_t dcdc_mode)
{
	uint32_t err_code = NRF_SUCCESS;

	if (!platform_runtime_uses_softdevice())
	{
		NRF_POWER->DCDCEN = dcdc_mode;
		return NRF_SUCCESS;
	}

	err_code = PLATFORM_RUNTIME_SOFTDEVICE_CALL(sd_power_dcdc_mode_set(dcdc_mode));
	platform_runtime_record_error(err_code);
	return err_code;
}

uint32_t platform_runtime_protected_register_write(volatile uint32_t *p_register, uint32_t value)
{
	uint32_t err_code = NRF_SUCCESS;

	if (p_register == NULL)
	{
		return NRF_ERROR_NULL;
	}

	if (!platform_runtime_uses_softdevice())
	{
		*p_register = value;
		return NRF_SUCCESS;
	}

	err_code = PLATFORM_RUNTIME_SOFTDEVICE_CALL(sd_protected_register_write(p_register, value));
	platform_runtime_record_error(err_code);
	return err_code;
}

void app_util_disable_irq(void) { (void)platform_runtime_critical_region_enter(NULL); }
void app_util_enable_irq(void) { (void)platform_runtime_critical_region_exit(0); }
void app_util_critical_region_enter(uint8_t *p_nested) { (void)platform_runtime_critical_region_enter(p_nested); }
void app_util_critical_region_exit(uint8_t nested) { (void)platform_runtime_critical_region_exit(nested); }

uint8_t privilege_level_get(void)
{
#if __CORTEX_M == (0x00U) || defined(_WIN32) || defined(__unix) || defined(__APPLE__)
	return APP_LEVEL_PRIVILEGED;
#elif __CORTEX_M == (0x04U)
	uint32_t isr_vector_num = __get_IPSR() & IPSR_ISR_Msk;
	if (isr_vector_num == 0)
	{
		int32_t control = __get_CONTROL();
		return (control & CONTROL_nPRIV_Msk) ? APP_LEVEL_UNPRIVILEGED : APP_LEVEL_PRIVILEGED;
	}
	return APP_LEVEL_PRIVILEGED;
#else
	return APP_LEVEL_PRIVILEGED;
#endif
}

uint8_t current_int_priority_get(void)
{
	uint32_t isr_vector_num = __get_IPSR() & IPSR_ISR_Msk;
	if (isr_vector_num > 0)
	{
		int32_t irq_type = ((int32_t)isr_vector_num - EXTERNAL_INT_VECTOR_OFFSET);
		return (uint8_t)(NVIC_GetPriority((IRQn_Type)irq_type) & 0xFF);
	}
	return APP_IRQ_PRIORITY_THREAD;
}
