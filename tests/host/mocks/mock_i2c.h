/*
 * mock_i2c.h - TWIM bus mock with installable per-address handlers.
 *
 * Replaces SDK nrfx_twim master transfers. Each test installs handlers for
 * the addresses it cares about (LSM6DS33 0x6A, LIS3MDL 0x1C, APDS9960 0x39,
 * SHT31-D 0x44, BMP280 0x77); transfers to uninstalled addresses NACK.
 */
#ifndef MOCK_I2C_H
#define MOCK_I2C_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOCK_I2C_MAX_DEVICES 8

/* read=1 for master-read, read=0 for master-write. Return 0 on ACK, !0 on NACK. */
typedef int (*mock_i2c_xfer_fn)(uint8_t addr,
                                uint8_t *buf, size_t len,
                                int read,
                                void *user);

typedef struct {
    uint8_t          addr;
    mock_i2c_xfer_fn xfer;
    void            *user;
} mock_i2c_device_t;

void mock_i2c_reset(void);
void mock_i2c_install(const mock_i2c_device_t *dev);

int mock_i2c_transfer(uint8_t addr, uint8_t *buf, size_t len, int read);

#ifdef __cplusplus
}
#endif
#endif /* MOCK_I2C_H */
