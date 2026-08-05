#include <stdbool.h>
#include <stdint.h>
#include "core.h"

#ifdef BOARD_CLUE
#include "nrf.h"
#include "core_cm4.h"
#include "timebase.h"
#include "pinRegistry.h"
#include "platformRuntime.h"
#include "runtime.h"
#include "storage/calib.h"
#include "i2cBus.h"
#include "i2c_twim_backend.h"

#define SD_INFO_ADDR    0x3000
#define SD_MAGIC        0x51B1E5DB
#define APP_VTOR_ADDR   0x26000

/*
 * Raw-mode boot path: disable the SoftDevice if present, then redirect the
 * vector table to the application image.
 *
 * CRITICAL: The SVC instruction MUST execute while the MBR (at address 0)
 * still owns the SVC vector. Once SCB->VTOR is changed, SVC would dispatch
 * through the application's vector table, which has no MBR SVC handler and
 * would HardFault.
 *
 * This is ONLY for RUNTIME_RAW_WHAD. In RUNTIME_BLE_HID the SoftDevice must
 * stay alive (nrf_sdh_enable_request in BleRuntime::init uses SVC), and VTOR
 * stays at MBR-default (0x0) so SD's SWI/SVC vectors resolve through the MBR
 * until the SD itself takes over the table.
 */
static inline void clue_raw_boot_path(void) {
	volatile uint32_t *sd_magic = (volatile uint32_t *)SD_INFO_ADDR;
	if (*sd_magic == SD_MAGIC) {
		register uint32_t ret __asm("r0");
		__asm volatile("svc %1\n" : "=r"(ret) : "I"(0x11) : "memory");
		(void)ret;
	}
	SCB->VTOR = APP_VTOR_ADDR;
	__DSB();
	__ISB();
}

/*
 * Timebase backend: TIMER4 is started by TimerModule's constructor at
 * 1 MHz (PRESCALER=4 → 16 MHz / 16), 32-bit. timebase_init seeds
 * s_lastCounter from whatever the counter currently reads, so calling
 * it before Core construction (TIMER4 still at reset value 0) is safe —
 * the first tick after Core is up will advance the accumulator correctly.
 */
static uint32_t timer4_read(void) {
	NRF_TIMER4->TASKS_CAPTURE[5] = 1UL;
	return NRF_TIMER4->CC[5];
}

static const timebase_backend_t TIMEBASE_BACKEND = {
	.read      = timer4_read,
	.frequency = 1000000ul,
	.bits      = 32,
};

/*
 * Pinreg critical-section trampolines. platform_runtime_critical_region_*
 * take/return a nesting flag, but pinreg_critical_fn is void(void).
 * The raw-mode implementation tracks its own depth internally and ignores
 * the nested argument on exit, so passing 0 is correct at boot. pinreg's
 * CritGuard is scope-bounded so nesting cannot arise from its usage.
 */
static void clue_crit_enter(void) {
	uint8_t nested;
	(void)platform_runtime_critical_region_enter(&nested);
}

static void clue_crit_exit(void) {
	(void)platform_runtime_critical_region_exit(0);
}

/*
 * GPREGRET2 trampolines. platformRuntime's API differs from runtime_backend_t:
 * platform functions return an error code and take a gpregret_id (1=GPREGRET2),
 * while the backend expects void return + value semantics.
 *
 * platform_runtime_power_gpregret_set uses mask-based OR (*p_register |= mask)
 * in the raw path, so a stale value would corrupt the new one. Clear-then-set
 * ensures the written byte is exactly what the caller passed.
 */
static uint32_t backend_gpregret2_read(void) {
	uint32_t v = 0;
	(void)platform_runtime_power_gpregret_get(1, &v);
	return v;
}

static void backend_gpregret2_set(uint8_t v) {
	(void)platform_runtime_power_gpregret_clr(1, 0xFFu);
	(void)platform_runtime_power_gpregret_set(1, (uint32_t)v);
}

static void backend_gpregret2_clear(void) {
	(void)platform_runtime_power_gpregret_clr(1, 0xFFu);
}

/*
 * Platform runtime backend. wdt_arm/wdt_feed are NULL — runtime.cpp
 * NULL-checks them before invoking, so the switch path runs without
 * the safety-net reset. A future task can wire WDT once chosen.
 */
static const runtime_backend_t PLATFORM_RUNTIME_BACKEND = {
	.gpregret2_read  = backend_gpregret2_read,
	.gpregret2_clear = backend_gpregret2_clear,
	.gpregret2_set   = backend_gpregret2_set,
	.store_read      = calib_runtime_store_read,
	.store_write     = calib_runtime_store_write,
	.system_reset    = platform_runtime_system_reset,
	.now_us          = timebase_now_us,
	.wdt_arm         = NULL,
	.wdt_feed        = NULL,
};
#endif

int main(void) {
#ifdef BOARD_CLUE
	/* Initialize 64-bit monotonic timebase (TIMER4 backend, raw mode).
	 * Safe before Core: seeds from current counter value (0 at reset);
	 * TimerModule's constructor inside Core will start TIMER4 shortly. */
	timebase_init(&TIMEBASE_BACKEND);

	/* Initialize pin registry critical-section hooks. Must precede any
	 * subsystem that calls pinreg_acquire_* (gpio/adc/pdm expert paths). */
	pinreg_init(clue_crit_enter, clue_crit_exit);

	/* Initialize the TWIM1 backend + the pure-logic i2cBus manager and
	 * probe the five onboard sensors (LSM6DS33, LIS3MDL, APDS9960,
	 * BMP280, SHT31-D). Probe results are queried by the sensor driver
	 * wrappers via i2cbus_is_present() when BoardModule begins them.
	 * T20 wires the bus; T21+ instantiates SensorDrivers inside
	 * BoardModule to tick the wrappers and feed parsed samples into
	 * the motion subsystem. */
	pinreg_token_t i2c_lease = PINREG_TOKEN_INVALID;
	(void)pinreg_acquire_group(PINREG_GROUP_TWIM1,
	                            PINREG_OWNER_SENSOR_BUS, NULL, &i2c_lease);
	const i2cbus_backend_t *i2c_be = i2c_twim_backend_get();
	if (i2c_be != NULL) {
		i2cbus_init(i2c_be, (uint32_t)i2c_lease);
		i2cbus_probe_all();
	}

	/* Initialize the runtime selector with the platform backend so that
	 * runtime_request_switch() (called from SetRuntimeMode and the rotation
	 * gesture) becomes functional. runtime_select() consumes the GPREGRET2
	 * one-shot and consults the RuntimeConfigStore, applying the precedence
	 * contract documented in runtime.h. Must run BEFORE Core construction
	 * so the selected mode can guide Core's runtime branch. */
	runtime_init(&PLATFORM_RUNTIME_BACKEND);
	runtime_mode_t mode = runtime_select(NULL);

	/* Mode-gated boot path: raw mode kills the SoftDevice and repoints
	 * VTOR to the application; BLE mode leaves both alone so the SD can
	 * be (re)enabled from BleRuntime::init() via nrf_sdh_enable_request. */
	if (mode == RUNTIME_RAW_WHAD) {
		clue_raw_boot_path();
	}

	Core core(mode);
#else
	Core core;
#endif
	core.init();
	core.loop();
}
