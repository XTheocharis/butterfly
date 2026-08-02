/*
 * mock_ble.h - SoftDevice event sink for code-under-test.
 *
 * Replaces sd_ble_* event delivery with a single callback hook plus
 * per-event-type counters. Tests either poll counters or install a
 * callback to drive state transitions in the code-under-test.
 *
 * Used by Todo 20 BLE-HID runtime, Todo 21 HIDS+GATT, Todo 22 Peer Manager,
 * and Todo 23 button-mapping tests.
 */
#ifndef MOCK_BLE_H
#define MOCK_BLE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    MOCK_BLE_EVT_CONNECTED = 0,
    MOCK_BLE_EVT_DISCONNECTED,
    MOCK_BLE_EVT_SECURED,
    MOCK_BLE_EVT_BOND_WRITTEN,
    MOCK_BLE_EVT_ADV_STARTED,
    MOCK_BLE_EVT_ADV_STOPPED,
    MOCK_BLE_EVT_HID_REPORT_SENT,
    MOCK_BLE_EVT_COUNT
} mock_ble_event_t;

typedef void (*mock_ble_callback_t)(mock_ble_event_t evt, void *user);

void mock_ble_reset(void);
void mock_ble_set_callback(mock_ble_callback_t cb, void *user);

void   mock_ble_emit(mock_ble_event_t evt);
size_t mock_ble_event_count(mock_ble_event_t evt);
size_t mock_ble_total_events(void);

#ifdef __cplusplus
}
#endif
#endif /* MOCK_BLE_H */
