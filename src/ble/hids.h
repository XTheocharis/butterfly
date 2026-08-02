/*
 * hids.h - HID-over-GATT Service for BLE-HID runtime.
 *
 * Wraps the pure-C hids_eval layer with SDK 15.3 ble_hids calls.
 * Registers the composite HID service (mouse + keyboard + consumer)
 * with the GATT server, handles CCCD subscription, protocol mode,
 * and HVX notification dispatch with the 2-credit pipeline.
 *
 * Must be called AFTER GattServer::init() (Todo 20) and BEFORE
 * advertising starts.
 */
#ifndef HIDS_H
#define HIDS_H

#include "hids_eval.h"

#ifdef __cplusplus

#include <stdint.h>

#include "ble.h"
#include "ble_hids.h"

/*
 * HidsService owns the BLE HID Service lifecycle:
 *   1. Register the composite report map via ble_hids_init
 *   2. Configure input/output report permissions (encrypted)
 *   3. Track CCCD subscription state per connection
 *   4. Dispatch HID reports via sd_ble_gatts_hvx with pipeline-of-2
 *   5. Handle protocol mode (report-only) and control point
 *
 * The refcount state machine (hids_state_t) is updated by callers
 * (button handlers, air mouse, APDS gesture), then this class
 * flushes dirty reports through the HVX pipeline.
 */
class HidsService {
public:
	HidsService();
	~HidsService();

	/* Register the HIDS service with the GATT server.
	 * Returns false on SDK error. */
	bool init(void);

	/* Handle BLE events (BLE_GATTS_EVT_WRITE, TX-complete, etc.). */
	void onBleEvent(const ble_evt_t *p_ble_evt);

	/* Returns true if the peer has subscribed to HID notifications
	 * (CCCD enabled on any input report). */
	bool isSubscribed(void) const { return m_subscribed; }

	/* Returns true if the peer has requested suspend mode. */
	bool isSuspended(void) const { return m_suspended; }

	/* Flush dirty reports through the HVX pipeline.
	 * Called from the main loop. Returns the report ID sent, or 0. */
	uint8_t flushPending(void);

	/* Called when a TX-complete event arrives (restores credit). */
	void onTxComplete(void);

	/* Access the refcount state for callers to press/release. */
	hids_state_t *getState(void) { return &m_state; }

	/* Send a mouse report from current state. */
	bool sendMouseReport(void);

	/* Send a keyboard report from current state. */
	bool sendKeyboardReport(void);

	/* Send a consumer report from current state. */
	bool sendConsumerReport(void);

	/* Notify connection state changes. */
	void onConnect(uint16_t conn_handle);
	void onDisconnect(void);

private:
	hids_state_t  m_state;
	ble_hids_t   *m_p_hids;        /* SDK HIDS instance */
	uint16_t      m_connHandle;
	bool          m_subscribed;     /* CCCD enabled */
	bool          m_suspended;      /* control point = SUSPEND */
	bool          m_protocolBoot;   /* false = report protocol */

	/* Attempt to send one report via sd_ble_gatts_hvx. */
	bool sendReport(uint8_t report_id, const uint8_t *data, uint8_t len);

	/* Handle CCCD write events for input report subscription. */
	void onCccdWrite(const ble_evt_t *p_ble_evt);

	/* Handle control point write (suspend/exit). */
	void onControlPointWrite(const ble_evt_t *p_ble_evt);
};

#endif /* __cplusplus */

#endif /* HIDS_H */
