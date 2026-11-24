/**
 * @file flash.c
 * @brief mmap-backed flash.bin HAL (ADR-002).
 *
 * The file is created with 1 MB if it does not exist and mapped with
 * MAP_SHARED. Writes and erases msync(MS_SYNC) the affected range.
 * It is not thread-safe: the caller must serialize access.
 */

#include "flash.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

static int s_fd = -1;
static void *s_map = NULL;
static size_t s_size = 0U;
static bool s_initialized = false;

/**
 * @brief Syncs a range with the file, aligning it to the page size.
 *
 * @param addr Logical start address.
 * @param len Length in bytes.
 * @return FLASH_OK or FLASH_ERR_IO.
 */
static flash_status_t flash_msync_range(uint32_t addr, size_t len)
{
    long page_size;
    uintptr_t base;
    uintptr_t aligned;
    size_t offset;
    size_t sync_len;

    if (len == 0U) {
        return FLASH_OK;
    }

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return (msync(s_map, s_size, MS_SYNC) == 0) ? FLASH_OK : FLASH_ERR_IO;
    }

    base = (uintptr_t)s_map + addr;
    aligned = base & ~((uintptr_t)page_size - 1U);
    offset = (size_t)(base - aligned);
    sync_len = offset + len;
    sync_len = (sync_len + (size_t)page_size - 1U) & ~((size_t)page_size - 1U);

    if (msync((void *)aligned, sync_len, MS_SYNC) != 0) {
        return FLASH_ERR_IO;
    }
    return FLASH_OK;
}

/**
 * @brief Validates a range against the mapped size.
 *
 * @param addr Logical start address.
 * @param len Length in bytes.
 * @return FLASH_OK if the range is valid.
 */
static flash_status_t flash_check_range(uint32_t addr, size_t len)
{
    if (!s_initialized) {
        return FLASH_ERR_NOT_INIT;
    }
    if (addr > s_size) {
        return FLASH_ERR_RANGE;
    }
    if (len > (s_size - (size_t)addr)) {
        return FLASH_ERR_RANGE;
    }
    return FLASH_OK;
}

flash_status_t flash_init(const char *path)
{
    struct stat st;
    off_t expected_size;

    if (path == NULL) {
        return FLASH_ERR_INVALID;
    }
    if (s_initialized) {
        return FLASH_ERR_INVALID;
    }

    s_fd = open(path, O_RDWR | O_CREAT, 0644);
    if (s_fd < 0) {
        return FLASH_ERR_IO;
    }

    if (fstat(s_fd, &st) != 0) {
        (void)close(s_fd);
        s_fd = -1;
        return FLASH_ERR_IO;
    }

    expected_size = (off_t)FLASH_TOTAL_SIZE;
    if (st.st_size != expected_size) {
        if (ftruncate(s_fd, expected_size) != 0) {
            (void)close(s_fd);
            s_fd = -1;
            return FLASH_ERR_IO;
        }
    }

    s_map = mmap(NULL, (size_t)FLASH_TOTAL_SIZE, PROT_READ | PROT_WRITE,
                 MAP_SHARED, s_fd, 0);
    if (s_map == MAP_FAILED) {
        s_map = NULL;
        (void)close(s_fd);
        s_fd = -1;
        return FLASH_ERR_IO;
    }

    s_size = (size_t)FLASH_TOTAL_SIZE;
    s_initialized = true;
    return FLASH_OK;
}

void flash_deinit(void)
{
    if (!s_initialized) {
        return;
    }
    (void)msync(s_map, s_size, MS_SYNC);
    (void)munmap(s_map, s_size);
    (void)close(s_fd);
    s_map = NULL;
    s_fd = -1;
    s_size = 0U;
    s_initialized = false;
}

bool flash_is_initialized(void)
{
    return s_initialized;
}

uint32_t flash_size(void)
{
    return (uint32_t)s_size;
}

flash_status_t flash_read(uint32_t addr, void *buf, size_t len)
{
    flash_status_t status;

    if ((buf == NULL) && (len > 0U)) {
        return FLASH_ERR_INVALID;
    }
    status = flash_check_range(addr, len);
    if (status != FLASH_OK) {
        return status;
    }
    if (len > 0U) {
        (void)memcpy(buf, (const void *)((const uint8_t *)s_map + addr), len);
    }
    return FLASH_OK;
}

flash_status_t flash_write(uint32_t addr, const void *buf, size_t len)
{
    flash_status_t status;

    if ((buf == NULL) && (len > 0U)) {
        return FLASH_ERR_INVALID;
    }
    status = flash_check_range(addr, len);
    if (status != FLASH_OK) {
        return status;
    }
    if (len > 0U) {
        (void)memcpy((void *)((uint8_t *)s_map + addr), buf, len);
        return flash_msync_range(addr, len);
    }
    return FLASH_OK;
}

flash_status_t flash_erase(uint32_t addr, size_t len)
{
    flash_status_t status;

    status = flash_check_range(addr, len);
    if (status != FLASH_OK) {
        return status;
    }
    if (len > 0U) {
        (void)memset((void *)((uint8_t *)s_map + addr), 0xFF, len);
        return flash_msync_range(addr, len);
    }
    return FLASH_OK;
}

flash_status_t flash_sync(void)
{
    if (!s_initialized) {
        return FLASH_ERR_NOT_INIT;
    }
    return (msync(s_map, s_size, MS_SYNC) == 0) ? FLASH_OK : FLASH_ERR_IO;
}
