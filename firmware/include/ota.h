#ifndef OTA_H
#define OTA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * `.ota` format (docs/04-ota-y-seguridad-de-imagen.md §4).
 *
 *   HEADER (128 B, plaintext and signed)
 *   PAYLOAD encrypted with AES-256-GCM (payload_len B) + TAG (16 B)
 *   SIGNATURE Ed25519 (64 B) over header || ciphertext || tag
 *
 * The header is the GCM AAD through `aad_hash` (SHA-256 of the header with
 * `aad_hash` and `reserved` zeroed).
 */

#define OTA_MAGIC             "LAMPOTA1"
#define OTA_MAGIC_SIZE        8U
#define OTA_FORMAT_VERSION    1U
#define OTA_HEADER_SIZE       128U
#define OTA_AAD_HASH_SIZE     32U
#define OTA_NONCE_SIZE        12U
#define OTA_GCM_TAG_SIZE      16U
#define OTA_SIGNATURE_SIZE    64U
#define OTA_PUBLIC_KEY_SIZE   32U
#define OTA_AES_KEY_SIZE      32U
#define OTA_MIN_SIZE          (OTA_HEADER_SIZE + OTA_GCM_TAG_SIZE + OTA_SIGNATURE_SIZE)

/* Header offsets (little-endian). */
#define OTA_OFF_MAGIC          0U
#define OTA_OFF_FORMAT         8U
#define OTA_OFF_VERSION        10U
#define OTA_OFF_HW_COMPAT      16U
#define OTA_OFF_PAYLOAD_LEN    20U
#define OTA_OFF_PAYLOAD_SHA256 24U
#define OTA_OFF_NONCE          56U
#define OTA_OFF_AAD_HASH       68U
#define OTA_OFF_RESERVED       100U

/**
 * @brief Result codes; the values 8-12 match the bootloader fault codes
 * (docs/04 §7.1) so they can be reported as-is.
 */
typedef enum {
    OTA_OK = 0,
    OTA_ERR_ARGUMENT = -1,
    OTA_ERR_BUFFER = -2,
    OTA_FAULT_SIGNATURE = 8,
    OTA_FAULT_DOWNGRADE = 9,
    OTA_FAULT_SHA = 10,
    OTA_FAULT_HARDWARE = 11,
    OTA_FAULT_FORMAT = 12
} ota_status_t;

/** @brief Firmware version (major.minor.patch). */
typedef struct {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
} ota_version_t;

/** @brief Header data the bootloader needs before decrypting. */
typedef struct {
    ota_version_t version;
    uint32_t hw_compat;
    uint32_t payload_len;
} ota_info_t;

/** @brief Verification/decryption keys. */
typedef struct {
    const uint8_t *public_key; /* 32 B, mandatory */
    const uint8_t *aes_key;    /* 32 B; NULL = do not decrypt */
} ota_keys_t;

/**
 * @brief Compares two versions.
 *
 * @return <0 if a<b, 0 if a==b, >0 if a>b.
 */
int ota_version_compare(const ota_version_t *a, const ota_version_t *b);

/**
 * @brief Validates the header (magic, format, length and payload_len).
 *
 * @param data Complete artifact.
 * @param len  Artifact length.
 * @param info Output with version, hw_compat and payload_len.
 * @return OTA_OK or OTA_FAULT_FORMAT.
 */
ota_status_t ota_parse(const uint8_t *data, size_t len, ota_info_t *info);

/**
 * @brief Verifies header, hardware, anti-downgrade, AAD and Ed25519 signature.
 *
 * Does not decrypt. Order of checks: format → hardware → anti-downgrade
 * → aad_hash → signature (docs/04 §7).
 *
 * @param data           Complete artifact.
 * @param len            Artifact length.
 * @param keys           Public key (aes_key is ignored here).
 * @param active_version Active version, or NULL if there is none (first boot).
 * @param hw_rev         Hardware revision (>=1); the accepted bit is
 *                       `hw_compat & (1 << (hw_rev - 1))`.
 * @param info           Output with the header data.
 * @return OTA_OK or OTA_FAULT_*.
 */
ota_status_t ota_verify(const uint8_t *data, size_t len, const ota_keys_t *keys,
                        const ota_version_t *active_version, uint16_t hw_rev,
                        ota_info_t *info);

/**
 * @brief Verifies and decrypts the payload into `out`.
 *
 * @param data           Complete artifact.
 * @param len            Artifact length.
 * @param keys           Public key + AES key (mandatory).
 * @param active_version Active version or NULL.
 * @param hw_rev         Hardware revision.
 * @param out            Output buffer (plaintext payload).
 * @param out_size       Capacity of `out` (>= payload_len).
 * @param out_len        Length of the decrypted payload.
 * @param info           Output with the header data.
 * @return OTA_OK, OTA_ERR_BUFFER, OTA_ERR_ARGUMENT or OTA_FAULT_*.
 */
ota_status_t ota_verify_decrypt(const uint8_t *data, size_t len,
                                const ota_keys_t *keys,
                                const ota_version_t *active_version,
                                uint16_t hw_rev, uint8_t *out, size_t out_size,
                                size_t *out_len, ota_info_t *info);

/**
 * @brief Loads an Ed25519 public key from a PEM SPKI (or 64 hex chars).
 *
 * @param path PEM path (`keys/ota_public.pem`) or hex file.
 * @param out  32-byte destination.
 * @return 0 on success; -1 if the file or the format is not valid.
 */
int ota_load_public_key(const char *path, uint8_t out[OTA_PUBLIC_KEY_SIZE]);

#endif /* OTA_H */
