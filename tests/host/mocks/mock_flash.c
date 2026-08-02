#include "mock_flash.h"
#include <string.h>

static uint8_t g_storage[MOCK_FLASH_SIZE_BYTES];

void mock_flash_reset(void) {
    memset(g_storage, 0xFF, sizeof g_storage);
}

int mock_flash_read(uint32_t addr, void *buf, size_t len) {
    if ((size_t)addr + len > MOCK_FLASH_SIZE_BYTES) return -1;
    memcpy(buf, g_storage + addr, len);
    return 0;
}

int mock_flash_program(uint32_t addr, const void *buf, size_t len) {
    if ((size_t)addr + len > MOCK_FLASH_SIZE_BYTES) return -1;
    const uint8_t *src = (const uint8_t *)buf;
    for (size_t i = 0; i < len; ++i) {
        uint8_t cur = g_storage[addr + i];
        uint8_t want = src[i];
        /* NOR AND: a program operation can only clear bits. */
        if ((cur & want) != want) return -1;
        g_storage[addr + i] = cur & want;
    }
    return 0;
}

int mock_flash_erase_sector(uint32_t sector_addr) {
    if (sector_addr % MOCK_FLASH_SECTOR_SIZE != 0) return -1;
    if ((size_t)sector_addr >= MOCK_FLASH_SIZE_BYTES) return -1;
    memset(g_storage + sector_addr, 0xFF, MOCK_FLASH_SECTOR_SIZE);
    return 0;
}

const uint8_t *mock_flash_raw(size_t *out_size) {
    if (out_size) *out_size = sizeof g_storage;
    return g_storage;
}
