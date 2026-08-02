#include "mock_ble.h"
#include <string.h>

static mock_ble_callback_t g_callback;
static void               *g_user;
static size_t              g_counts[MOCK_BLE_EVT_COUNT];

void mock_ble_reset(void) {
    g_callback = NULL;
    g_user = NULL;
    memset(g_counts, 0, sizeof g_counts);
}

void mock_ble_set_callback(mock_ble_callback_t cb, void *user) {
    g_callback = cb;
    g_user = user;
}

void mock_ble_emit(mock_ble_event_t evt) {
    if (evt < MOCK_BLE_EVT_COUNT) ++g_counts[evt];
    if (g_callback) g_callback(evt, g_user);
}

size_t mock_ble_event_count(mock_ble_event_t evt) {
    return (evt < MOCK_BLE_EVT_COUNT) ? g_counts[evt] : 0;
}

size_t mock_ble_total_events(void) {
    size_t total = 0;
    for (int i = 0; i < MOCK_BLE_EVT_COUNT; ++i) total += g_counts[i];
    return total;
}
