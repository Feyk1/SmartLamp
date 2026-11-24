/**
 * @file crc32.c
 * @brief CRC32 IEEE 802.3 (polynomial 0xEDB88320).
 */

#include "crc32.h"

uint32_t crc32_ieee(const void *data, size_t len)
{
    const uint8_t *bytes = (const uint8_t *)data;
    uint32_t crc = 0xFFFFFFFFU;
    size_t i;
    unsigned bit;

    if ((bytes == NULL) && (len > 0U)) {
        return 0U;
    }

    for (i = 0U; i < len; i++) {
        crc ^= bytes[i];
        for (bit = 0U; bit < 8U; bit++) {
            if ((crc & 1U) != 0U) {
                crc = (crc >> 1U) ^ 0xEDB88320U;
            } else {
                crc >>= 1U;
            }
        }
    }
    return crc ^ 0xFFFFFFFFU;
}
