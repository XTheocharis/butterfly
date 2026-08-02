#include "mock_spi.h"
#include <string.h>

static mock_spi_device_t g_devices[MOCK_SPI_MAX_DEVICES];
static size_t g_device_count;

void mock_spi_reset(void) {
    memset(g_devices, 0, sizeof g_devices);
    g_device_count = 0;
}

void mock_spi_install(const mock_spi_device_t *dev) {
    if (g_device_count >= MOCK_SPI_MAX_DEVICES) return;
    for (size_t i = 0; i < g_device_count; ++i) {
        if (g_devices[i].cs == dev->cs) {
            g_devices[i] = *dev;
            return;
        }
    }
    g_devices[g_device_count++] = *dev;
}

int mock_spi_transfer(uint8_t cs, uint8_t *buf, size_t len) {
    for (size_t i = 0; i < g_device_count; ++i) {
        if (g_devices[i].cs == cs && g_devices[i].xfer) {
            return g_devices[i].xfer(buf, len, g_devices[i].user);
        }
    }
    return -1; /* no device at CS */
}
