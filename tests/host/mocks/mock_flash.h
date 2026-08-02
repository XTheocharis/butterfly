/*
 * mock_flash.h - RAM-backed NOR flash with realistic erase semantics.
 *
 * Mirrors the CLUE onboard GD25Q16C: 2 MiB, 4 KiB erase sector.
 * Programs can only clear bits (1->0). Re-raising a bit requires erase.
 * Used by Todo 29 QSPI journal/recovery tests.
 */
#ifndef MOCK_FLASH_H
#define MOCK_FLASH_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MOCK_FLASH_SIZE_BYTES   (2u * 1024u * 1024u)
#define MOCK_FLASH_SECTOR_SIZE  4096u

void mock_flash_reset(void); /* fills with 0xFF */

int mock_flash_read(uint32_t addr, void *buf, size_t len);
int mock_flash_program(uint32_t addr, const void *buf, size_t len);  /* 0 ok, -1 not-erased/OOR */
int mock_flash_erase_sector(uint32_t sector_addr);                   /* 0 ok, -1 unaligned/OOR */

/* Direct view for tests that need to inspect raw contents. */
const uint8_t *mock_flash_raw(size_t *out_size);

#ifdef __cplusplus
}
#endif
#endif /* MOCK_FLASH_H */
