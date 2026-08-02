/*
 * gatt.cpp - GATT server implementation.
 *
 * Configures GAP device name, PPCP, and GATTS. The HIDS/DIS services
 * are registered by Todo 21 (hids.cpp) — this file sets the GAP layer
 * foundation they depend on.
 */
#include "gatt.h"

#ifdef __cplusplus

#include "ble_gap.h"
#include "ble_gatts.h"
#include "ble.h"
#include "nrf_ble_gatt.h"
#include "ble_dis.h"
#include "app_error.h"

/* HID Generic appearance (per HID over GATT profile spec). */
#define BLE_APPEARANCE_HID_REMOTE  962u

GattServer::GattServer()
	: m_connHandle(BLE_CONN_HANDLE_INVALID)
{
}

GattServer::~GattServer() {
}

bool GattServer::configGap(void) {
	ret_code_t err;

	/* Set device name with encrypted write permission (requires bonding).
	 * Read is open to all for discovery. */
	ble_gap_conn_sec_mode_t sec_mode;
	BLE_GAP_CONN_SEC_MODE_SET_OPEN(&sec_mode);

	err = sd_ble_gap_device_name_set(&sec_mode,
	                                 (const uint8_t *)BLE_DEVICE_NAME,
	                                 BLE_DEVICE_NAME_LEN);
	if (err != NRF_SUCCESS) return false;

	/* Set appearance to HID remote. */
	err = sd_ble_gap_appearance_set(BLE_APPEARANCE_HID_REMOTE);
	return err == NRF_SUCCESS;
}

bool GattServer::setPpcp(void) {
	/* PPCP: min 7.5ms, max 15ms, latency 0 (active), timeout 4s.
	 * Interval units: 1.25ms (so 7.5→6, 15→12).
	 * Timeout units: 10ms (so 4000→400). */
	ble_gap_conn_params_t ppcp = {
		.min_conn_interval = ble_gap_interval_from_ms(BLE_CONN_MIN_INTERVAL_MS),
		.max_conn_interval = ble_gap_interval_from_ms(BLE_CONN_MAX_INTERVAL_MS),
		.slave_latency     = BLE_LATENCY_ACTIVE,
		.conn_sup_timeout  = ble_sup_timeout_from_ms(BLE_CONN_TIMEOUT_MS),
	};

	ret_code_t err = sd_ble_gap_ppcp_set(&ppcp);
	return err == NRF_SUCCESS;
}

bool GattServer::initGatt(void) {
	ret_code_t err = nrf_ble_gatt_init(nullptr, nullptr);
	return err == NRF_SUCCESS;
}

bool GattServer::init(void) {
	if (!configGap()) return false;
	if (!setPpcp()) return false;
	if (!initGatt()) return false;

	/* Device Information Service (DIS) — required by HID over GATT spec.
	 * Manufacturer, model, firmware are populated from build macros. */
	ble_dis_init_t dis_init = {};
	dis_init.dis_char_rd_sec = SEC_OPEN;

	ret_code_t err = ble_dis_init(&dis_init);
	if (err != NRF_SUCCESS) return false;

	return true;
}

void GattServer::onBleEvent(const ble_evt_t *p_ble_evt) {
	if (p_ble_evt == nullptr) return;

	switch (p_ble_evt->header.evt_id) {
	case BLE_GAP_EVT_CONNECTED:
		m_connHandle = p_ble_evt->evt.gap_evt.conn_handle;
		break;

	case BLE_GAP_EVT_DISCONNECTED:
		m_connHandle = BLE_CONN_HANDLE_INVALID;
		break;

	default:
		break;
	}
}

#endif /* __cplusplus */
