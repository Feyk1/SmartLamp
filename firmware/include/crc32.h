#ifndef CRC32_H
#define CRC32_H

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Computes CRC32 IEEE 802.3 (polynomial 0xEDB88320).
 *
 * @param data Input buffer.
 * @param len Length in bytes.
 * @return Computed CRC32.
 */
uint32_t crc32_ieee(const void *data, size_t len);

#endif /* CRC32_H */
