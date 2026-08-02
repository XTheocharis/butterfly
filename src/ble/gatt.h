/*
 * gatt.h - GATT server configuration for BLE-HID runtime.
 *
 * Sets the device name "Butterfly CLUE Remote", configures PPCP
 * (Preferred Peripheral Connection Parameters), and provides a
 * registration hook for HIDS/DIS services (added by Todo 21).
 */
#ifndef GATT_H
#define GATT_H

#include "ble_eval.h"

#ifdef __cplusplus

#include <stdint.h>

/* ble_evt_t is an anonymous-struct typedef (typedef struct {...} ble_evt_t; in
 * the SoftDevice header), so it cannot be forward-declared as `struct ble_evt_t`.
 * Include the SDK header directly. */
#include "ble.h"

/*
 * GattServer configures the GAP/GATTS layer:
 *   - Device name via sd_ble_gap_device_name_set
 *   - PPCP via sd_ble_gap_ppcp_set (7.5-15ms, latency 0, timeout 4s)
 *   - Appearance (HID Generic remote)
 *   - Connection handle tracking
 *
 * HIDS service registration is deferred to Todo 21, which will call
 * registerService() with the compiled HID service handle.
 */
class GattServer {
public:
	GattServer();
	~GattServer();

	/* Initialize GAP + GATTS. Called from BleRuntime::initSubManagers. */
	bool init(void);

	/* Handle BLE events (connection/disconnection). */
	void onBleEvent(const ble_evt_t *p_ble_evt);

	/* Get the current connection handle (0xFFFF if not connected). */
	uint16_t getConnectionHandle(void) const { return m_connHandle; }

	/* Returns true if a peer is connected and secured. */
	bool isConnected(void) const { return m_connHandle != 0xFFFFu; }

private:
	uint16_t m_connHandle;

	/* Configure GAP device name and appearance. */
	bool configGap(void);

	/* Set PPCP via sd_ble_gap_ppcp_set. */
	bool setPpcp(void);

	/* Initialize nrf_ble_gatt module for MTU exchange. */
	bool initGatt(void);
};

#endif /* __cplusplus */

#endif /* GATT_H */
