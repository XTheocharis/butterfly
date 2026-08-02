/*
 * advertising.h - BLE advertising state machine for CLUE.
 *
 * Fast 30ms for 30s → slow 250ms indefinitely. Bonded peers get
 * directed/whitelist with a timed open fallback. The pure FSM logic
 * lives in ble_eval.h; this class wraps it with the SDK ble_advertising
 * module and connection-parameter latency transitions.
 */
#ifndef ADVERTISING_H
#define ADVERTISING_H

#include "ble_eval.h"

#ifdef __cplusplus

#include <stdint.h>

#include "ble.h"

class BondStorage;

/*
 * AdvertisingManager drives the ble_advertising SDK module through the
 * states defined in ble_adv_state_t. It also tracks connection-parameter
 * latency transitions (active latency 0 → idle latency 4 after 5s
 * inactivity → back to 0 on user activity).
 */
class AdvertisingManager {
public:
	AdvertisingManager();
	~AdvertisingManager();

	/* Initialize ble_advertising module with fast/slow config. */
	bool init(void);

	/* Start fast advertising. Returns false if ble_advertising_start fails. */
	bool start(void);

	/* Handle BLE events (adv timeout, connect, disconnect). */
	void onBleEvent(const ble_evt_t *p_ble_evt);

	/* Notify user activity (pointer/key/consumer) — restarts fast adv
	 * or requests active latency. */
	void onUserActivity(void);

	/* Notify idle timeout (5s without activity). Requests idle latency. */
	void onIdleTimeout(void);

	/* Set the bond storage used to detect bonded peers on disconnect.
	 * May be called with nullptr to disable bonded advertising. */
	void setBondStore(BondStorage *bond) { m_bond = bond; }

	/* Get current advertising state. */
	ble_adv_state_t getState(void) const { return m_advState; }

	/* Get current latency state. */
	ble_latency_state_t getLatencyState(void) const { return m_latState; }

	/* Process periodic checks (idle timer, backoff timer). */
	void process(uint32_t now_ms);

private:
	ble_adv_state_t     m_advState;
	ble_latency_state_t m_latState;
	bool                m_hasBond;
	BondStorage        *m_bond;
	uint16_t            m_connHandle;

	/* Timestamps for latency FSM. */
	uint32_t            m_lastActivityMs;
	uint32_t            m_backoffStartMs;
	uint32_t            m_lastLatencyUpdateMs;
	bool                m_backoffActive;

	/* Transition the advertising FSM. */
	void transition(ble_adv_event_t evt);

	/* Request connection parameter update for current latency state. */
	void requestLatencyUpdate(void);

	/* Check if idle/backoff timers have elapsed. */
	void checkTimers(uint32_t now_ms);
};

#endif /* __cplusplus */

#endif /* ADVERTISING_H */
