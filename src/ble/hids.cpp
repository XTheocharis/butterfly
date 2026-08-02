/*
 * hids.cpp - HID-over-GATT Service SDK implementation.
 *
 * Wraps hids_eval pure-logic functions with SDK 15.3 ble_hids calls.
 * The report map, refcount state, and HVX pipeline logic are all
 * defined in hids_eval.{h,c} and tested on host. This file only
 * provides the SDK integration glue.
 */
#include "hids.h"

#ifdef __cplusplus

#include "ble_hids.h"
#include "ble_srv_common.h"
#include "app_error.h"
#include <string.h>

/* SDK HIDS instance — must be declared via the macro to initialize the
 * const p_link_ctx_storage member (a default-constructed struct won't link
 * under C++). Single peripheral client; reports sized per HIDS_REPORT_*. */
BLE_HIDS_DEF(s_hids,
             1,
             HIDS_MOUSE_REPORT_SIZE,
             HIDS_KB_INPUT_REPORT_SIZE,
             HIDS_CONSUMER_REPORT_SIZE);

/* Report map is defined in hids_eval.c as a compile-time constant. */

HidsService::HidsService()
	: m_p_hids(&s_hids)
	, m_connHandle(0xFFFFu)
	, m_subscribed(false)
	, m_suspended(false)
	, m_protocolBoot(false)
{
	hids_state_init(&m_state);
}

HidsService::~HidsService() {
}

bool HidsService::init(void) {
	ret_code_t err;

	/* Configure the HID service with our composite report map. */
	ble_hids_init_t hids_init_obj;
	memset(&hids_init_obj, 0, sizeof(hids_init_obj));

	hids_init_obj.rep_map.p_data      = (uint8_t *)hids_report_map;
	hids_init_obj.rep_map.data_len    = HIDS_REPORT_MAP_SIZE;
	hids_init_obj.rep_map.rd_sec      = SEC_JUST_WORKS;

	/* HID Information: bcdHID 1.1, flags normally-connectable + remote-wake. */
	hids_init_obj.hid_information.bcd_hid        = HIDS_BCD_HID;
	hids_init_obj.hid_information.b_country_code = 0u;
	hids_init_obj.hid_information.flags          = HIDS_INFO_FLAGS;
	hids_init_obj.hid_information.rd_sec          = SEC_JUST_WORKS;

	/* Encrypted permissions (post-bonding). */
	hids_init_obj.protocol_mode_rd_sec = SEC_JUST_WORKS;
	hids_init_obj.protocol_mode_wr_sec = SEC_JUST_WORKS;
	hids_init_obj.ctrl_point_wr_sec    = SEC_JUST_WORKS;

	/* Input report configurations (mouse, keyboard, consumer). */
	ble_hids_inp_rep_init_t input_reports[3];
	memset(input_reports, 0, sizeof(input_reports));

	/* Mouse input report (index 0). */
	input_reports[HIDS_REPORT_IDX_MOUSE].max_len              = HIDS_MOUSE_REPORT_SIZE;
	input_reports[HIDS_REPORT_IDX_MOUSE].sec.rd               = SEC_JUST_WORKS;
	input_reports[HIDS_REPORT_IDX_MOUSE].sec.wr               = SEC_NO_ACCESS;
	input_reports[HIDS_REPORT_IDX_MOUSE].sec.cccd_wr          = SEC_JUST_WORKS;
	input_reports[HIDS_REPORT_IDX_MOUSE].rep_ref.report_id    = HIDS_REPORT_ID_MOUSE;
	input_reports[HIDS_REPORT_IDX_MOUSE].rep_ref.report_type  = HIDS_REPORT_TYPE_INPUT;

	/* Keyboard input report (index 1). */
	input_reports[HIDS_REPORT_IDX_KEYBOARD].max_len              = HIDS_KB_INPUT_REPORT_SIZE;
	input_reports[HIDS_REPORT_IDX_KEYBOARD].sec.rd               = SEC_JUST_WORKS;
	input_reports[HIDS_REPORT_IDX_KEYBOARD].sec.wr               = SEC_NO_ACCESS;
	input_reports[HIDS_REPORT_IDX_KEYBOARD].sec.cccd_wr          = SEC_JUST_WORKS;
	input_reports[HIDS_REPORT_IDX_KEYBOARD].rep_ref.report_id    = HIDS_REPORT_ID_KEYBOARD;
	input_reports[HIDS_REPORT_IDX_KEYBOARD].rep_ref.report_type  = HIDS_REPORT_TYPE_INPUT;

	/* Consumer input report (index 2). */
	input_reports[HIDS_REPORT_IDX_CONSUMER].max_len              = HIDS_CONSUMER_REPORT_SIZE;
	input_reports[HIDS_REPORT_IDX_CONSUMER].sec.rd               = SEC_JUST_WORKS;
	input_reports[HIDS_REPORT_IDX_CONSUMER].sec.wr               = SEC_NO_ACCESS;
	input_reports[HIDS_REPORT_IDX_CONSUMER].sec.cccd_wr          = SEC_JUST_WORKS;
	input_reports[HIDS_REPORT_IDX_CONSUMER].rep_ref.report_id    = HIDS_REPORT_ID_CONSUMER;
	input_reports[HIDS_REPORT_IDX_CONSUMER].rep_ref.report_type  = HIDS_REPORT_TYPE_INPUT;

	hids_init_obj.p_inp_rep_array = input_reports;
	hids_init_obj.inp_rep_count = 3u;

	/* Keyboard LED output report (index 0). */
	ble_hids_outp_rep_init_t output_reports[1];
	memset(output_reports, 0, sizeof(output_reports));
	output_reports[HIDS_REPORT_IDX_KB_LED].max_len              = HIDS_KB_LED_REPORT_SIZE;
	output_reports[HIDS_REPORT_IDX_KB_LED].sec.rd               = SEC_NO_ACCESS;
	output_reports[HIDS_REPORT_IDX_KB_LED].sec.wr               = SEC_JUST_WORKS;
	output_reports[HIDS_REPORT_IDX_KB_LED].rep_ref.report_id    = HIDS_REPORT_ID_KEYBOARD;
	output_reports[HIDS_REPORT_IDX_KB_LED].rep_ref.report_type  = HIDS_REPORT_TYPE_OUTPUT;

	hids_init_obj.p_outp_rep_array = output_reports;
	hids_init_obj.outp_rep_count = 1u;

	hids_init_obj.feature_rep_count = 0u;

	hids_init_obj.is_kb    = false;
	hids_init_obj.is_mouse = false;

	err = ble_hids_init(&s_hids, &hids_init_obj);
	if (err != NRF_SUCCESS) {
		return false;
	}

	/* Validate the report map we just registered. */
	if (!hids_report_map_validate(hids_report_map, HIDS_REPORT_MAP_SIZE)) {
		return false;
	}

	return true;
}

void HidsService::onConnect(uint16_t conn_handle) {
	m_connHandle = conn_handle;
	m_subscribed = false;
	m_suspended = false;
	m_protocolBoot = false;
	hids_hvx_init(&m_state.hvx);
}

void HidsService::onDisconnect(void) {
	m_connHandle = 0xFFFFu;
	m_subscribed = false;
	m_suspended = false;
	hids_hvx_init(&m_state.hvx);
}

bool HidsService::sendReport(uint8_t report_id, const uint8_t *data, uint8_t len) {
	if (!m_subscribed || m_connHandle == 0xFFFFu || m_suspended) {
		return false;
	}

	uint8_t report_idx;
	switch (report_id) {
	case HIDS_REPORT_ID_MOUSE:
		report_idx = HIDS_REPORT_IDX_MOUSE;
		break;
	case HIDS_REPORT_ID_KEYBOARD:
		report_idx = HIDS_REPORT_IDX_KEYBOARD;
		break;
	case HIDS_REPORT_ID_CONSUMER:
		report_idx = HIDS_REPORT_IDX_CONSUMER;
		break;
	default:
		return false;
	}

	/* hvx_params for notification. */
	ble_gatts_hvx_params_t hvx_params;
	memset(&hvx_params, 0, sizeof(hvx_params));

	uint16_t hvx_len = len;

	hvx_params.handle = m_p_hids->inp_rep_array[report_idx].char_handles.value_handle;
	hvx_params.type = BLE_GATT_HVX_NOTIFICATION;
	hvx_params.offset = 0;
	hvx_params.p_len = &hvx_len;
	hvx_params.p_data = (uint8_t *)data;

	ret_code_t err = sd_ble_gatts_hvx(m_connHandle, &hvx_params);

	if (err == NRF_SUCCESS) {
		hids_hvx_on_sent(&m_state.hvx, report_id);
		return true;
	}

	/* NRF_ERROR_RESOURCES → no TX buffer available, keep dirty. */
	/* NRF_ERROR_INVALID_STATE → CCCD disabled. */
	return false;
}

uint8_t HidsService::flushPending(void) {
	uint8_t report_id = hids_hvx_next_send(&m_state.hvx);
	if (report_id == 0u) return 0u;

	uint8_t report[HIDS_KB_INPUT_REPORT_SIZE]; /* largest report size */

	switch (report_id) {
	case HIDS_REPORT_ID_MOUSE:
		hids_build_mouse_report(&m_state, report);
		if (!sendReport(HIDS_REPORT_ID_MOUSE, report, HIDS_MOUSE_REPORT_SIZE)) {
			return 0u;
		}
		break;
	case HIDS_REPORT_ID_KEYBOARD:
		hids_build_keyboard_report(&m_state, report);
		if (!sendReport(HIDS_REPORT_ID_KEYBOARD, report, HIDS_KB_INPUT_REPORT_SIZE)) {
			return 0u;
		}
		break;
	case HIDS_REPORT_ID_CONSUMER:
		hids_build_consumer_report(&m_state, report);
		if (!sendReport(HIDS_REPORT_ID_CONSUMER, report, HIDS_CONSUMER_REPORT_SIZE)) {
			return 0u;
		}
		break;
	default:
		return 0u;
	}

	return report_id;
}

void HidsService::onTxComplete(void) {
	bool has_pending = hids_hvx_on_tx_complete(&m_state.hvx);

	/* If there is pending work, flush immediately (credits restored). */
	if (has_pending) {
		flushPending();
	}
}

bool HidsService::sendMouseReport(void) {
	hids_hvx_mark_dirty(&m_state.hvx, HIDS_REPORT_ID_MOUSE);
	return flushPending() != 0u;
}

bool HidsService::sendKeyboardReport(void) {
	hids_hvx_mark_dirty(&m_state.hvx, HIDS_REPORT_ID_KEYBOARD);
	return flushPending();
}

bool HidsService::sendConsumerReport(void) {
	hids_hvx_mark_dirty(&m_state.hvx, HIDS_REPORT_ID_CONSUMER);
	return flushPending();
}

void HidsService::onCccdWrite(const ble_evt_t *p_ble_evt) {
	ble_gatts_evt_write_t const *p_write =
		&p_ble_evt->evt.gatts_evt.params.write;

	/* Check if any input report CCCD was enabled. */
	for (uint8_t i = 0; i < 3u; i++) {
		if (p_write->handle == m_p_hids->inp_rep_array[i].char_handles.cccd_handle) {
			uint16_t cccd_value = (p_write->data[0] | (p_write->data[1] << 8));
			m_subscribed = (cccd_value & BLE_GATT_HVX_NOTIFICATION) != 0;
			break;
		}
	}
}

void HidsService::onControlPointWrite(const ble_evt_t *p_ble_evt) {
	ble_gatts_evt_write_t const *p_write =
		&p_ble_evt->evt.gatts_evt.params.write;

	if (p_write->len == 1u) {
		if (p_write->data[0] == HIDS_CTRL_SUSPEND) {
			m_suspended = true;
		} else if (p_write->data[0] == HIDS_CTRL_EXIT_SUSP) {
			m_suspended = false;
		}
	}
}

void HidsService::onBleEvent(const ble_evt_t *p_ble_evt) {
	if (p_ble_evt == nullptr) return;

	switch (p_ble_evt->header.evt_id) {
	case BLE_GATTS_EVT_WRITE:
		onCccdWrite(p_ble_evt);
		onControlPointWrite(p_ble_evt);
		break;

	case BLE_GATTS_EVT_HVN_TX_COMPLETE:
		onTxComplete();
		break;

	case BLE_GAP_EVT_CONNECTED:
		onConnect(p_ble_evt->evt.gap_evt.conn_handle);
		break;

	case BLE_GAP_EVT_DISCONNECTED:
		onDisconnect();
		break;

	default:
		break;
	}
}

#endif /* __cplusplus */
