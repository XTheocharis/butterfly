/*
 * mock_spi.h - SPIM master mock with installable per-CS handlers.
 *
 * Replaces SDK nrfx_spim master transfers (CLUE display ST7789 on SPIM2).
 * Full-duplex: writes len bytes from buf, receives len bytes into buf.
 */
#ifndef MOCK_SPI_H
#define MOCK_SPI_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOCK_SPI_MAX_DEVICES 4

typedef int (*mock_spi_xfer_fn)(uint8_t *buf, size_t len, void *user);

typedef struct {
    uint8_t          cs;   /* chip-select index */
    mock_spi_xfer_fn xfer;
    void            *user;
} mock_spi_device_t;

void mock_spi_reset(void);
void mock_spi_install(const mock_spi_device_t *dev);

int mock_spi_transfer(uint8_t cs, uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* MOCK_SPI_H */
