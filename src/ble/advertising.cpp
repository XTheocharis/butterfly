/*
 * advertising.cpp - BLE advertising implementation.
 *
 * Wraps ble_eval.h FSM with SDK ble_advertising module calls.
 * Handles fast→slow→directed/whitelist transitions and the
 * connection-parameter latency state machine.
 */
#include "advertising.h"

#ifdef __cplusplus

#include "nrf_sdh_ble.h"
#include "nrf_sdh_soc.h"
#include "ble_advertising.h"
#include "ble_gap.h"
#include "ble.h"
#include "ble_advdata.h"

#include "bond.h"

/* Instance of the ble_advertising module. */
BLE_ADVERTISING_DEF(m_advertising);

AdvertisingManager::AdvertisingManager()
	: m_advState(BLE_ADV_STATE_IDLE)
	, m_latState(BLE_LAT_STATE_ACTIVE)
	, m_hasBond(false)
	, m_bond(nullptr)
	, m_connHandle(0xFFFFu)
	, m_lastActivityMs(0)
	, m_backoffStartMs(0)
	, m_lastLatencyUpdateMs(0)
	, m_backoffActive(false)
{
}

AdvertisingManager::~AdvertisingManager() {
}

bool AdvertisingManager::init(void) {
	/* Advertising data: flags + device name + appearance. */
	ble_advdata_t advdata = {};
	advdata.name_type          = BLE_ADVDATA_FULL_NAME;
	advdata.include_appearance = true;
	advdata.flags              = BLE_GAP_ADV_FLAGS_LE_ONLY_GENERAL_DISC_MODE;

	/* Scan response: not used (full name fits in adv packet). */
	ble_advdata_t srdata = {};

	/* Fast advertising: 30ms interval, 30s timeout.
	 * Interval units: 0.625ms (30ms → 48). Timeout: 10ms units (30s → 3000). */
	ble_adv_modes_config_t advConfig = {};
	advConfig.ble_adv_fast_enabled   = true;
	advConfig.ble_adv_fast_interval  = ble_adv_interval_from_ms(BLE_ADV_FAST_INTERVAL_MS);
	advConfig.ble_adv_fast_timeout   = BLE_ADV_TIMEOUT_FROM_MS(BLE_ADV_FAST_TIMEOUT_MS);

	/* Slow advertising: 250ms interval, unlimited. */
	advConfig.ble_adv_slow_enabled   = true;
	advConfig.ble_adv_slow_interval  = ble_adv_interval_from_ms(BLE_ADV_SLOW_INTERVAL_MS);
	advConfig.ble_adv_slow_timeout   = BLE_ADV_SLOW_TIMEOUT_UNLIM;

	/* Directed/whitelist for bonded peers. */
	advConfig.ble_adv_directed_high_duty_enabled = false;
	advConfig.ble_adv_directed_enabled            = true;
	advConfig.ble_adv_directed_interval           = ble_adv_interval_from_ms(BLE_ADV_SLOW_INTERVAL_MS);
	advConfig.ble_adv_directed_timeout            = BLE_ADV_TIMEOUT_FROM_MS(BLE_ADV_DIRECTED_TIMEOUT_MS);

	/* Whitelist for slow advertising when bonded. */
	advConfig.ble_adv_whitelist_enabled = true;

	ble_advertising_init_t initCfg = {
		.advdata    = advdata,
		.srdata     = srdata,
		.config     = advConfig,
	};
	uint32_t err = ble_advertising_init(&m_advertising, &initCfg);
	if (err != NRF_SUCCESS) return false;

	m_advState = BLE_ADV_STATE_IDLE;
	return true;
}

bool AdvertisingManager::start(void) {
	uint32_t err = ble_advertising_start(&m_advertising, BLE_ADV_MODE_FAST);
	if (err != NRF_SUCCESS) return false;

	m_advState = BLE_ADV_STATE_FAST;
	return true;
}

void AdvertisingManager::transition(ble_adv_event_t evt) {
	ble_adv_state_t next = ble_adv_fsm(m_advState, evt, m_hasBond);
	if (next != m_advState) {
		m_advState = next;

		/* Drive the SDK module based on the new state. */
		switch (next) {
		case BLE_ADV_STATE_FAST:
			ble_advertising_start(&m_advertising, BLE_ADV_MODE_FAST);
			break;
		case BLE_ADV_STATE_SLOW:
			ble_advertising_start(&m_advertising,
				m_hasBond ? BLE_ADV_MODE_SLOW : BLE_ADV_MODE_SLOW);
			break;
		case BLE_ADV_STATE_DIRECTED:
			ble_advertising_start(&m_advertising, BLE_ADV_MODE_DIRECTED);
			break;
		case BLE_ADV_STATE_STOPPED:
			/* Advertising auto-stops on connect. */
			break;
		default:
			break;
		}
	}
}

void AdvertisingManager::onBleEvent(const ble_evt_t *p_ble_evt) {
	if (p_ble_evt == nullptr) return;

	/* Let ble_advertising module handle its own events first. */
	ble_advertising_on_ble_evt(p_ble_evt, &m_advertising);

	switch (p_ble_evt->header.evt_id) {
	case BLE_GAP_EVT_ADV_SET_TERMINATED:
		/* Fast timeout → transition to slow. */
		transition(BLE_ADV_EVT_FAST_TIMEOUT);
		break;

	case BLE_GAP_EVT_CONNECTED:
		m_connHandle = p_ble_evt->evt.gap_evt.conn_handle;
		transition(BLE_ADV_EVT_CONNECTED);
		break;

	case BLE_GAP_EVT_DISCONNECTED:
		/* Prefer the bond store's view (PM tracks peer IDs that
		 * survive a reconnect); fall back to no bond if unavailable. */
		if (m_bond != nullptr) {
			m_hasBond = m_bond->hasBondedPeer();
		} else {
			m_hasBond = false;
		}
		m_connHandle = 0xFFFFu;
		transition(BLE_ADV_EVT_DISCONNECTED);
		break;

	case BLE_GAP_EVT_CONN_PARAM_UPDATE_REQUEST: {
		/* Host requested conn param update. Accept the host's proposal. */
		/* The SDK ble_advertising module handles this automatically when
		 * using ble_conn_params. If the update is rejected by the host
		 * after we requested it, we enter latency backoff. */
		break;
	}

	case BLE_GAP_EVT_CONN_PARAM_UPDATE:
		/* Host accepted our last latency request — clear backoff. */
		m_backoffActive = false;
		break;

	default:
		break;
	}
}

void AdvertisingManager::onUserActivity(void) {
	m_lastActivityMs = 0; /* Reset by caller's timebase */

	/* Restart fast advertising if in slow/directed. */
	transition(BLE_ADV_EVT_IDLE_ACTIVITY);

	/* Transition latency to active. */
	ble_latency_state_t next = ble_latency_fsm(m_latState, BLE_LAT_EVT_ACTIVITY);
	if (next != m_latState) {
		m_latState = next;
		requestLatencyUpdate();
	}
}

void AdvertisingManager::onIdleTimeout(void) {
	ble_latency_state_t next = ble_latency_fsm(m_latState, BLE_LAT_EVT_IDLE_5S);
	if (next != m_latState) {
		m_latState = next;
		requestLatencyUpdate();
	}
}

void AdvertisingManager::requestLatencyUpdate(void) {
	if (m_connHandle == 0xFFFFu) {
		return;
	}

	/* Rate limit: BLE spec restricts conn param updates to no more than
	 * once per 30 s for the central side; the SoftDevice returns
	 * NRF_ERROR_INVALID_STATE on back-to-back requests. We enforce
	 * BLE_LATENCY_BACKOFF_MS as the minimum spacing between requests. */
	uint32_t now = m_lastActivityMs;
	if (m_lastLatencyUpdateMs != 0u &&
	    (now - m_lastLatencyUpdateMs) < BLE_LATENCY_BACKOFF_MS) {
		return;
	}

	ble_gap_conn_params_t params = {
		.min_conn_interval = ble_gap_interval_from_ms(BLE_CONN_MIN_INTERVAL_MS),
		.max_conn_interval = ble_gap_interval_from_ms(BLE_CONN_MAX_INTERVAL_MS),
		.slave_latency     = ble_latency_value(m_latState),
		.conn_sup_timeout  = ble_sup_timeout_from_ms(BLE_CONN_TIMEOUT_MS),
	};

	ret_code_t err = sd_ble_gap_conn_param_update(m_connHandle, &params);
	if (err == NRF_SUCCESS) {
		m_lastLatencyUpdateMs = now;
	} else if (err == NRF_ERROR_INVALID_STATE) {
		/* Host already negotiating — wait for the result. */
		m_backoffActive = true;
		m_backoffStartMs = now;
	}
}

void AdvertisingManager::checkTimers(uint32_t now_ms) {
	/* Idle detection: 5s without activity. */
	if (m_latState == BLE_LAT_STATE_ACTIVE &&
	    (now_ms - m_lastActivityMs) >= BLE_IDLE_THRESHOLD_MS) {
		onIdleTimeout();
		m_lastActivityMs = now_ms;
	}

	/* Backoff: 30s after host rejection. */
	if (m_backoffActive &&
	    (now_ms - m_backoffStartMs) >= BLE_LATENCY_BACKOFF_MS) {
		m_backoffActive = false;
		ble_latency_state_t next = ble_latency_fsm(m_latState,
		                                            BLE_LAT_EVT_BACKOFF_ELAPSED);
		if (next != m_latState) {
			m_latState = next;
			requestLatencyUpdate();
		}
	}
}

void AdvertisingManager::process(uint32_t now_ms) {
	checkTimers(now_ms);
}

#endif /* __cplusplus */
