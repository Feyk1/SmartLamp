#ifndef FLASH_H
#define FLASH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* flash.bin layout (ADR-002, docs/01-arquitectura-general.md §7.1). */
#define FLASH_TOTAL_SIZE        (1024U * 1024U)

#define FLASH_BOOTLOADER_OFFSET 0x00000U
#define FLASH_BOOTLOADER_SIZE   (64U * 1024U)

#define FLASH_SLOT_A_OFFSET     0x10000U
#define FLASH_SLOT_A_SIZE       (352U * 1024U)

#define FLASH_SLOT_B_OFFSET     0x68000U
#define FLASH_SLOT_B_SIZE       (352U * 1024U)

#define FLASH_METADATA_OFFSET   0xC0000U
#define FLASH_METADATA_SIZE     (16U * 1024U)

#define FLASH_CONFIG_OFFSET     0xC4000U
#define FLASH_CONFIG_SIZE       (16U * 1024U)

#define FLASH_STAGING_OFFSET    0xC8000U
#define FLASH_STAGING_SIZE      (208U * 1024U)

#define FLASH_RESERVED_OFFSET   0xFC000U
#define FLASH_RESERVED_SIZE     (16U * 1024U)

typedef enum {
    FLASH_OK = 0,
    FLASH_ERR_INVALID = -1,
    FLASH_ERR_IO = -2,
    FLASH_ERR_NOT_INIT = -3,
    FLASH_ERR_RANGE = -4
} flash_status_t;

flash_status_t flash_init(const char *path);
void flash_deinit(void);
bool flash_is_initialized(void);
uint32_t flash_size(void);

flash_status_t flash_read(uint32_t addr, void *buf, size_t len);
flash_status_t flash_write(uint32_t addr, const void *buf, size_t len);
flash_status_t flash_erase(uint32_t addr, size_t len);
flash_status_t flash_sync(void);

#endif /* FLASH_H */
