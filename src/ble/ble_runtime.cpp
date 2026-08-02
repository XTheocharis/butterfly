/*
 * ble_runtime.cpp - BLE-HID SoftDevice coordinator implementation.
 *
 * Wraps ble_eval pure-logic evaluations with SDK 15.3 nrf_sdh calls.
 * All firmware-only SDK includes are here — ble_eval.c stays pure-C.
 */
#include "ble_runtime.h"

#ifdef __cplusplus

#include "gatt.h"
#include "advertising.h"
#include "security.h"
#include "hids.h"
#include "bond.h"
#include "profiles.h"

#include "nrf_sdh.h"
#include "nrf_sdh_ble.h"
#include "nrf_sdh_soc.h"
#include "nrf_power.h"
#include "ble_err.h"

/* Observer priority for our BLE event handler. Must be >= APP priority. */
#define BLE_OBSERVER_PRIO   2u
/* SoC observer priority — must be < NRF_SDH_SOC_OBSERVER_PRIO_LEVELS (2). */
#define SOC_OBSERVER_PRIO   1u

/* Connection config tag for our peripheral link. */
#define BLE_CONN_CFG_TAG    1u

/* Singleton for static observer dispatch. */
static BleRuntime *g_runtime = nullptr;

/* SD memory read backend for ble_validate_sd. */
static uint32_t sd_read32(uint32_t addr) {
	return *(volatile uint32_t *)addr;
}

BleRuntime::BleRuntime()
	: m_state(BleRuntimeState::Uninit)
	, m_validation(BLE_SD_MAGIC_MISMATCH)
	, m_ramCheck(BLE_RAM_INSUFFICIENT)
	, m_sdRamStart(0)
	, m_gatt(nullptr)
	, m_advertising(nullptr)
	, m_security(nullptr)
	, m_hids(nullptr)
	, m_bond(nullptr)
	, m_profiles(nullptr)
{
}

BleRuntime::~BleRuntime() {
	delete m_profiles;
	delete m_bond;
	delete m_hids;
	delete m_security;
	delete m_advertising;
	delete m_gatt;
	g_runtime = nullptr;
}

bool BleRuntime::validateSoftDevice(void) {
	static const ble_sd_backend_t backend = { sd_read32 };
	m_validation = ble_validate_sd(&backend);
	if (m_validation != BLE_SD_VALID) {
		m_state = BleRuntimeState::Error;
		return false;
	}
	m_state = BleRuntimeState::SdValidated;
	return true;
}

bool BleRuntime::enableSoftDevice(void) {
	ret_code_t err;

	/* Step 1: Request SDH enable. This starts the SoftDevice. */
	err = nrf_sdh_enable_request();
	if (err != NRF_SUCCESS) {
		m_state = BleRuntimeState::Error;
		return false;
	}

	/* Step 2: Configure BLE stack defaults, then override link counts.
	 * nrf_sdh_ble_default_cfg_set reads sdk_config.h macros for
	 * peripheral/central link counts, HVN queue, etc. */
	err = nrf_sdh_ble_default_cfg_set(BLE_CONN_CFG_TAG, &m_sdRamStart);
	if (err != NRF_SUCCESS) {
		m_state = BleRuntimeState::Error;
		return false;
	}

	/* Step 2b: Explicit overrides for the BLE-HID profile.
	 * These match the eval-layer constants in ble_eval.h. The defaults
	 * usually agree, but a stale sdk_config.h must not silently widen
	 * the link budget. */
	ble_cfg_t cfg = {};

	/* HVN TX queue: at least BLE_HVN_TX_QUEUE_MIN (2) for the
	 * HID pipeline-of-2 (one in-flight + one queued). */
	memset(&cfg, 0, sizeof(cfg));
	cfg.conn_cfg.conn_cfg_tag = BLE_CONN_CFG_TAG;
	cfg.conn_cfg.params.gatts_conn_cfg.hvn_tx_queue_size =
		BLE_HVN_TX_QUEUE_MIN;
	err = sd_ble_cfg_set(BLE_CONN_CFG_GATTS, &cfg, m_sdRamStart);
	if (err != NRF_SUCCESS) {
		m_state = BleRuntimeState::Error;
		return false;
	}

	/* Role counts: 1 peripheral, 0 central — overrides any sdk_config
	 * default that would reserve central RAM. */
	memset(&cfg, 0, sizeof(cfg));
	cfg.gap_cfg.role_count_cfg.periph_role_count  = BLE_PERIPHERAL_LINK_COUNT;
	cfg.gap_cfg.role_count_cfg.central_role_count = BLE_CENTRAL_LINK_COUNT;
	cfg.gap_cfg.role_count_cfg.central_sec_count  = 0u;
	err = sd_ble_cfg_set(BLE_GAP_CFG_ROLE_COUNT, &cfg, m_sdRamStart);
	if (err != NRF_SUCCESS) {
		m_state = BleRuntimeState::Error;
		return false;
	}

	/* Step 3: Check RAM origin compatibility.
	 * The linker script reserves 0x20006000 (clue.ld RAM origin).
	 * If the SD requires higher, the image is incompatible — must
	 * rebuild, not fix in-app. */
	m_ramCheck = ble_check_ram_origin(BLE_RAM_ORIGIN_BASELINE, m_sdRamStart);
	if (m_ramCheck != BLE_RAM_OK) {
		m_state = BleRuntimeState::Error;
		return false;
	}

	/* Step 4: Enable the BLE stack. */
	err = nrf_sdh_ble_enable(&m_sdRamStart);
	if (err != NRF_SUCCESS) {
		m_state = BleRuntimeState::Error;
		return false;
	}

	m_state = BleRuntimeState::SdEnabled;
	return true;
}

void BleRuntime::bleEvtHandler(ble_evt_t const *p_ble_evt, void *p_context) {
	(void)p_context;
	if (g_runtime == nullptr) return;

	if (g_runtime->m_advertising) {
		g_runtime->m_advertising->onBleEvent(p_ble_evt);
	}
	if (g_runtime->m_security) {
		g_runtime->m_security->onBleEvent(p_ble_evt);
	}
	if (g_runtime->m_gatt) {
		g_runtime->m_gatt->onBleEvent(p_ble_evt);
	}
	if (g_runtime->m_hids) {
		g_runtime->m_hids->onBleEvent(p_ble_evt);
	}
	if (g_runtime->m_bond) {
		g_runtime->m_bond->onBleEvent(p_ble_evt);
	}
}

void BleRuntime::socEvtHandler(uint32_t evt_id, void *p_context) {
	(void)p_context;
	/* SoC events from the SoftDevice. BLE-HID cares about:
	 *   - USB power events: VBUS detect/remove + 3.3V ready, used to
	 *     decide when the dongle is host-powered and can advertise.
	 *   - Flash operation results: surfaced to FDS wait loops via
	 *     the FDS internal handler; nothing to do here directly.
	 */
	switch (evt_id) {
	case NRF_EVT_POWER_USB_DETECTED:
	case NRF_EVT_POWER_USB_POWER_READY:
	case NRF_EVT_POWER_USB_REMOVED:
		/* Forwarded via nrf_power's USB event hook for the runtime
		 * power manager. The decision itself happens in runtime.cpp
		 * (BLE/WHAD mode switch on USB state); here we only mark the
		 * event received so the polling path can short-circuit. */
		break;

	case NRF_EVT_FLASH_OPERATION_SUCCESS:
	case NRF_EVT_FLASH_OPERATION_ERROR:
		/* FDS owns the wait loop; no app-level action needed. */
		break;

	default:
		break;
	}
}

bool BleRuntime::initSubManagers(void) {
	g_runtime = this;

	/* Register BLE observer before any sd_ble_* calls. */
	NRF_SDH_BLE_OBSERVER(m_ble_observer, BLE_OBSERVER_PRIO,
	                     bleEvtHandler, nullptr);

	/* Register SoC observer for power events (USB, flash). */
	NRF_SDH_SOC_OBSERVER(m_soc_observer, SOC_OBSERVER_PRIO,
	                     socEvtHandler, nullptr);

	/* Initialize GATT server (device name, PPCP, services). */
	m_gatt = new GattServer();
	if (!m_gatt->init()) {
		m_state = BleRuntimeState::Error;
		return false;
	}
	m_state = BleRuntimeState::GattReady;

	/* Initialize security (Peer Manager, LESC). PM inits FDS internally. */
	m_security = new SecurityManager();
	if (!m_security->init()) {
		m_state = BleRuntimeState::Error;
		return false;
	}

	/* Initialize bond storage (Peer Manager events + FDS CCCD store).
	 * Comes after SecurityManager so PM and FDS are already up. */
	m_bond = new BondStorage();
	if (!m_bond->init()) {
		/* Bond storage failure is non-fatal: bonding is unavailable
		 * but the device still works without persistence. */
		delete m_bond;
		m_bond = nullptr;
	}

	/* Initialize HIDS service (registers HID characteristics with GATTS).
	 * Comes after GattServer so the GATTS layer is ready. */
	m_hids = new HidsService();
	if (!m_hids->init()) {
		m_state = BleRuntimeState::Error;
		return false;
	}

	/* Initialize profile manager; wire it to the HIDS refcount state. */
	m_profiles = new ProfileManager();
	m_profiles->setHidsState(m_hids->getState());

	/* Initialize advertising (fast/slow/directed states). Advertising
	 * reads bond state on disconnect, so it needs the bond pointer set
	 * before its first transition. */
	m_advertising = new AdvertisingManager();
	m_advertising->setBondStore(m_bond);
	if (!m_advertising->init()) {
		m_state = BleRuntimeState::Error;
		return false;
	}

	return true;
}

bool BleRuntime::init(void) {
	if (m_state != BleRuntimeState::Uninit) {
		return m_state != BleRuntimeState::Error;
	}

	/* IRQ assertions: BLE mode must never touch raw timers. */
	static_assert(BLE_IRQ_PRIO_SD_RESERVED == 0u,
		"SD-reserved IRQ priority must be 0");
	static_assert(BLE_IRQ_PRIO_APP_DEFAULT == 6u,
		"App IRQ priority must be 6");

	if (!validateSoftDevice()) {
		return false;
	}
	if (!enableSoftDevice()) {
		return false;
	}
	if (!initSubManagers()) {
		return false;
	}

	/* Start advertising. */
	if (m_advertising && m_advertising->start()) {
		m_state = BleRuntimeState::Advertising;
	}

	return m_state != BleRuntimeState::Error;
}

void BleRuntime::process(void) {
	/* Service LESC handler for ongoing LE Secure Connections. */
	if (m_security) {
		m_security->processLesc();
	}
	/* Flush any dirty HIDS reports (deferred from input handlers). */
	if (m_hids) {
		m_hids->flushPending();
	}
}

#endif /* __cplusplus */
