#include "mock_i2c.h"
#include <string.h>

static mock_i2c_device_t g_devices[MOCK_I2C_MAX_DEVICES];
static size_t g_device_count;

void mock_i2c_reset(void) {
    memset(g_devices, 0, sizeof g_devices);
    g_device_count = 0;
}

void mock_i2c_install(const mock_i2c_device_t *dev) {
    if (g_device_count >= MOCK_I2C_MAX_DEVICES) return;
    /* Last install wins: overwrite an existing entry for the same address. */
    for (size_t i = 0; i < g_device_count; ++i) {
        if (g_devices[i].addr == dev->addr) {
            g_devices[i] = *dev;
            return;
        }
    }
    g_devices[g_device_count++] = *dev;
}

int mock_i2c_transfer(uint8_t addr, uint8_t *buf, size_t len, int read) {
    for (size_t i = 0; i < g_device_count; ++i) {
        if (g_devices[i].addr == addr && g_devices[i].xfer) {
            return g_devices[i].xfer(addr, buf, len, read, g_devices[i].user);
        }
    }
    return -1; /* NACK: no device at address */
}
