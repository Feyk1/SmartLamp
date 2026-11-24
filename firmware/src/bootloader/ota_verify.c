/**
 * @file ota_verify.c
 * @brief Verification and decryption of the `.ota` artifact (docs/04 §4, §7).
 *
 * Critical code: any change here must be reviewed together with
 * `tools/package_ota.py` and the vectors in `test_ota_verify`.
 */

#include "ota.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <mbedtls/gcm.h>
#include <sodium.h>

#define OTA_RESERVED_SIZE 28U

/** @brief DER SPKI of an Ed25519 key (12-byte prefix). */
static const uint8_t k_ed25519_spki_prefix[12] = {
    0x30U, 0x2AU, 0x30U, 0x05U, 0x06U, 0x03U,
    0x2BU, 0x65U, 0x70U, 0x03U, 0x21U, 0x00U
};

static uint16_t read_u16_le(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8U));
}

static uint32_t read_u32_le(const uint8_t *p)
{
    return ((uint32_t)p[0]) | ((uint32_t)p[1] << 8U) |
           ((uint32_t)p[2] << 16U) | ((uint32_t)p[3] << 24U);
}

int ota_version_compare(const ota_version_t *a, const ota_version_t *b)
{
    if ((a == NULL) || (b == NULL)) {
        return 0;
    }
    if (a->major != b->major) {
        return (a->major < b->major) ? -1 : 1;
    }
    if (a->minor != b->minor) {
        return (a->minor < b->minor) ? -1 : 1;
    }
    if (a->patch != b->patch) {
        return (a->patch < b->patch) ? -1 : 1;
    }
    return 0;
}

ota_status_t ota_parse(const uint8_t *data, size_t len, ota_info_t *info)
{
    uint32_t payload_len;
    size_t expected;
    ota_version_t version;

    if ((data == NULL) || (info == NULL)) {
        return OTA_ERR_ARGUMENT;
    }
    if (len < OTA_MIN_SIZE) {
        return OTA_FAULT_FORMAT;
    }
    if (memcmp(data + OTA_OFF_MAGIC, OTA_MAGIC, OTA_MAGIC_SIZE) != 0) {
        return OTA_FAULT_FORMAT;
    }
    if (read_u16_le(data + OTA_OFF_FORMAT) != OTA_FORMAT_VERSION) {
        return OTA_FAULT_FORMAT;
    }

    version.major = read_u16_le(data + OTA_OFF_VERSION);
    version.minor = read_u16_le(data + OTA_OFF_VERSION + 2U);
    version.patch = read_u16_le(data + OTA_OFF_VERSION + 4U);
    payload_len = read_u32_le(data + OTA_OFF_PAYLOAD_LEN);

    expected = OTA_HEADER_SIZE + (size_t)payload_len + OTA_GCM_TAG_SIZE +
               OTA_SIGNATURE_SIZE;
    if (expected != len) {
        return OTA_FAULT_FORMAT;
    }

    info->version = version;
    info->hw_compat = read_u32_le(data + OTA_OFF_HW_COMPAT);
    info->payload_len = payload_len;
    return OTA_OK;
}

/**
 * @brief Recomputes the header aad_hash (aad_hash and reserved zeroed).
 *
 * @param header 128-byte header.
 * @param out    Resulting SHA-256 (32 bytes).
 */
static void compute_aad_hash(const uint8_t *header, uint8_t out[OTA_AAD_HASH_SIZE])
{
    uint8_t provisional[OTA_HEADER_SIZE];

    (void)memcpy(provisional, header, OTA_HEADER_SIZE);
    (void)memset(provisional + OTA_OFF_AAD_HASH, 0, OTA_AAD_HASH_SIZE);
    (void)memset(provisional + OTA_OFF_RESERVED, 0, OTA_RESERVED_SIZE);
    (void)crypto_hash_sha256(out, provisional, OTA_HEADER_SIZE);
}

ota_status_t ota_verify(const uint8_t *data, size_t len, const ota_keys_t *keys,
                        const ota_version_t *active_version, uint16_t hw_rev,
                        ota_info_t *info)
{
    ota_info_t parsed;
    ota_status_t status;
    uint8_t aad_hash[OTA_AAD_HASH_SIZE];
    size_t signed_len;
    uint32_t hw_bit;

    if ((data == NULL) || (keys == NULL) || (keys->public_key == NULL) ||
        (info == NULL)) {
        return OTA_ERR_ARGUMENT;
    }
    if ((hw_rev < 1U) || (hw_rev > 32U)) {
        return OTA_ERR_ARGUMENT;
    }

    if (sodium_init() < 0) {
        return OTA_ERR_ARGUMENT;
    }

    status = ota_parse(data, len, &parsed);
    if (status != OTA_OK) {
        return status;
    }

    hw_bit = 1UL << (uint32_t)(hw_rev - 1U);
    if ((parsed.hw_compat & hw_bit) == 0U) {
        return OTA_FAULT_HARDWARE;
    }

    if ((active_version != NULL) &&
        (ota_version_compare(&parsed.version, active_version) <= 0)) {
        return OTA_FAULT_DOWNGRADE;
    }

    compute_aad_hash(data, aad_hash);
    if (memcmp(aad_hash, data + OTA_OFF_AAD_HASH, OTA_AAD_HASH_SIZE) != 0) {
        return OTA_FAULT_FORMAT;
    }

    signed_len = OTA_HEADER_SIZE + (size_t)parsed.payload_len + OTA_GCM_TAG_SIZE;
    if (crypto_sign_ed25519_verify_detached(data + signed_len, data, signed_len,
                                            keys->public_key) != 0) {
        return OTA_FAULT_SIGNATURE;
    }

    *info = parsed;
    return OTA_OK;
}

ota_status_t ota_verify_decrypt(const uint8_t *data, size_t len,
                                const ota_keys_t *keys,
                                const ota_version_t *active_version,
                                uint16_t hw_rev, uint8_t *out, size_t out_size,
                                size_t *out_len, ota_info_t *info)
{
    ota_info_t parsed;
    ota_status_t status;
    mbedtls_gcm_context ctx;
    uint8_t digest[OTA_AAD_HASH_SIZE];
    const uint8_t *ciphertext;
    const uint8_t *tag;
    int rc;

    if ((keys == NULL) || (keys->aes_key == NULL) || (out == NULL) ||
        (out_len == NULL) || (info == NULL)) {
        return OTA_ERR_ARGUMENT;
    }

    status = ota_verify(data, len, keys, active_version, hw_rev, &parsed);
    if (status != OTA_OK) {
        return status;
    }
    if (out_size < (size_t)parsed.payload_len) {
        return OTA_ERR_BUFFER;
    }

    ciphertext = data + OTA_HEADER_SIZE;
    tag = ciphertext + parsed.payload_len;

    mbedtls_gcm_init(&ctx);
    rc = mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, keys->aes_key,
                            OTA_AES_KEY_SIZE * 8U);
    if (rc == 0) {
        rc = mbedtls_gcm_auth_decrypt(
            &ctx, (size_t)parsed.payload_len, data + OTA_OFF_NONCE,
            OTA_NONCE_SIZE, data + OTA_OFF_AAD_HASH, OTA_AAD_HASH_SIZE, tag,
            OTA_GCM_TAG_SIZE, ciphertext, out);
    }
    mbedtls_gcm_free(&ctx);

    if (rc != 0) {
        /* Valid signature but invalid GCM tag: wrong AES key or tampered
         * payload (the docs/04 §7 flow reports it as fault 8). */
        return OTA_FAULT_SIGNATURE;
    }

    (void)crypto_hash_sha256(digest, out, parsed.payload_len);
    if (memcmp(digest, data + OTA_OFF_PAYLOAD_SHA256, OTA_AAD_HASH_SIZE) != 0) {
        return OTA_FAULT_SHA;
    }

    *out_len = (size_t)parsed.payload_len;
    *info = parsed;
    return OTA_OK;
}

/**
 * @brief Interprets the contents of a file as an Ed25519 key.
 *
 * Accepts PEM SPKI ("-----BEGIN PUBLIC KEY-----") or 64 hex characters.
 *
 * @return 0 on success; -1 otherwise.
 */
static int parse_public_key_text(const char *text,
                                 uint8_t out[OTA_PUBLIC_KEY_SIZE])
{
    const char *begin;
    const char *end;
    char b64[512];
    size_t b64_len = 0U;
    uint8_t der[128];
    size_t der_len = 0U;
    size_t text_len = strlen(text);

    begin = strstr(text, "-----BEGIN PUBLIC KEY-----");
    if (begin == NULL) {
        /* Raw hex format. */
        size_t digits = 0U;

        for (size_t i = 0U; i < text_len; i++) {
            if (isspace((unsigned char)text[i]) == 0) {
                b64[digits++] = text[i];
            }
            if (digits >= sizeof(b64)) {
                return -1;
            }
        }
        b64[digits] = '\0';
        if ((digits != (OTA_PUBLIC_KEY_SIZE * 2U)) ||
            (sodium_hex2bin(out, OTA_PUBLIC_KEY_SIZE, b64, digits, NULL, NULL,
                            NULL) != 0)) {
            return -1;
        }
        return 0;
    }

    end = strstr(begin, "-----END PUBLIC KEY-----");
    if ((end == NULL) || (end <= begin)) {
        return -1;
    }
    begin += strlen("-----BEGIN PUBLIC KEY-----");
    for (const char *p = begin; p < end; p++) {
        if (isspace((unsigned char)*p) == 0) {
            if (b64_len >= (sizeof(b64) - 1U)) {
                return -1;
            }
            b64[b64_len++] = *p;
        }
    }
    b64[b64_len] = '\0';

    if (sodium_base642bin(der, sizeof(der), b64, b64_len, NULL, &der_len, NULL,
                          sodium_base64_VARIANT_ORIGINAL) != 0) {
        return -1;
    }
    if (der_len == OTA_PUBLIC_KEY_SIZE) {
        (void)memcpy(out, der, OTA_PUBLIC_KEY_SIZE);
        return 0;
    }
    if ((der_len == (sizeof(k_ed25519_spki_prefix) + OTA_PUBLIC_KEY_SIZE)) &&
        (memcmp(der, k_ed25519_spki_prefix,
                sizeof(k_ed25519_spki_prefix)) == 0)) {
        (void)memcpy(out, der + sizeof(k_ed25519_spki_prefix),
                     OTA_PUBLIC_KEY_SIZE);
        return 0;
    }
    return -1;
}

int ota_load_public_key(const char *path, uint8_t out[OTA_PUBLIC_KEY_SIZE])
{
    FILE *fp;
    char text[512];
    size_t used;

    if ((path == NULL) || (out == NULL)) {
        return -1;
    }
    fp = fopen(path, "rb");
    if (fp == NULL) {
        return -1;
    }
    used = fread(text, 1U, sizeof(text) - 1U, fp);
    (void)fclose(fp);
    text[used] = '\0';

    return parse_public_key_text(text, out);
}
