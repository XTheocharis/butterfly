/*
 * ble_runtime.h - BLE-HID runtime SoftDevice coordinator.
 *
 * Validates the installed S140 6.1.1 SoftDevice header, enables the SDH,
 * checks RAM origin compatibility, and registers a BLE observer that
 * dispatches events to advertising/security/connection modules.
 *
 * The pure-logic evaluation layer (ble_eval.h) handles all host-testable
 * constants, unit conversions, and state machines. This class wraps those
 * evaluations with SDK 15.3 nrf_sdh calls.
 */
#ifndef BLE_RUNTIME_H
#define BLE_RUNTIME_H

#include "ble_eval.h"

#ifdef __cplusplus

#include <stdint.h>

#include "ble.h"

class GattServer;
class AdvertisingManager;
class SecurityManager;
class HidsService;
class BondStorage;
class ProfileManager;

/* BLE runtime lifecycle states. */
enum class BleRuntimeState : uint8_t {
	Uninit       = 0,
	SdValidated  = 1,
	SdEnabled    = 2,
	GattReady    = 3,
	Advertising  = 4,
	Connected    = 5,
	Error        = 0xFF,
};

/*
 * BleRuntime owns the full BLE-HID bring-up sequence:
 *   1. Validate SD header at 0x3004/0x3008/0x300C (pure eval)
 *   2. Request SDH enable
 *   3. Check returned RAM start vs linker origin
 *   4. Configure BLE stack (1 peripheral, 0 central, HVN queue >= 2)
 *   5. Enable BLE stack
 *   6. Register BLE observer
 *   7. Initialize GATT, advertising, security
 *
 * Must be called AFTER platformRuntime is set to BLE mode.
 */
class BleRuntime {
public:
	BleRuntime();
	~BleRuntime();

	/* Full initialization sequence. Returns false on any failure. */
	bool init(void);

	/* Get the current runtime state. */
	BleRuntimeState getState(void) const { return m_state; }

	/* Get the SD-required RAM start returned by sd_ble_enable. */
	uint32_t getSdRamStart(void) const { return m_sdRamStart; }

	/* Get validation result (BLE_SD_VALID on success). */
	ble_sd_validation_t getValidationResult(void) const { return m_validation; }

	/* Get RAM check result (BLE_RAM_OK on success). */
	ble_ram_check_t getRamCheckResult(void) const { return m_ramCheck; }

	/* Process BLE events. Called from main loop. */
	void process(void);

	/* Access sub-managers. Valid after init() returns true. */
	GattServer *getGatt(void) { return m_gatt; }
	AdvertisingManager *getAdvertising(void) { return m_advertising; }
	SecurityManager *getSecurity(void) { return m_security; }
	HidsService *getHids(void) { return m_hids; }
	BondStorage *getBond(void) { return m_bond; }
	ProfileManager *getProfiles(void) { return m_profiles; }

private:
	BleRuntimeState     m_state;
	ble_sd_validation_t m_validation;
	ble_ram_check_t     m_ramCheck;
	uint32_t            m_sdRamStart;

	GattServer        *m_gatt;
	AdvertisingManager *m_advertising;
	SecurityManager   *m_security;
	HidsService       *m_hids;
	BondStorage       *m_bond;
	ProfileManager    *m_profiles;

	/* SDH BLE observer callback (static → dispatches to singleton). */
	static void bleEvtHandler(ble_evt_t const *p_ble_evt, void *p_context);

	/* SDH SoC observer callback (handles flash-op + USB power events). */
	static void socEvtHandler(uint32_t evt_id, void *p_context);

	/* Validate the installed SoftDevice header. */
	bool validateSoftDevice(void);

	/* Enable SDH and configure BLE stack. */
	bool enableSoftDevice(void);

	/* Initialize sub-managers (GATT, advertising, security). */
	bool initSubManagers(void);
};

#endif /* __cplusplus */

#endif /* BLE_RUNTIME_H */
