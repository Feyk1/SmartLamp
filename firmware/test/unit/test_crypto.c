/**
 * @file test_crypto.c
 * @brief Test vectors for the vendored cryptographic dependencies.
 *
 * - Ed25519 (libsodium): RFC 8032 vector 1.
 * - AES-256-GCM (mbedTLS): NIST SP 800-38D vector (64 bytes).
 * - SHA-256 (mbedTLS): "abc".
 */

#include <stdio.h>
#include <string.h>

#include "unity.h"

#include <sodium.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>

#define HEX_BUF_MAX 96U

/**
 * @brief Converts a hex string to bytes.
 *
 * @return 0 if `hex` is exactly `out_len` bytes; -1 otherwise.
 */
static int hex_to_bytes(const char *hex, unsigned char *out, size_t out_len)
{
    size_t i;

    if ((hex == NULL) || (out == NULL) || (strlen(hex) != (out_len * 2U))) {
        return -1;
    }
    for (i = 0U; i < out_len; i++) {
        unsigned int value;

        if (sscanf(hex + (2U * i), "%2x", &value) != 1) {
            return -1;
        }
        out[i] = (unsigned char)value;
    }
    return 0;
}

void setUp(void)
{
}

void tearDown(void)
{
}

static void test_ed25519_rfc8032_vector1(void)
{
    unsigned char pk[32];
    unsigned char sig[64];
    unsigned char tampered[64];

    TEST_ASSERT_GREATER_OR_EQUAL_INT(0, sodium_init());
    TEST_ASSERT_EQUAL_INT(
        0, hex_to_bytes("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68"
                        "f707511a",
                        pk, sizeof(pk)));
    TEST_ASSERT_EQUAL_INT(
        0, hex_to_bytes("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065"
                        "224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe2"
                        "4655141438e7a100b",
                        sig, sizeof(sig)));

    TEST_ASSERT_EQUAL_INT(
        0, crypto_sign_ed25519_verify_detached(sig, (const unsigned char *)"",
                                               0U, pk));

    (void)memcpy(tampered, sig, sizeof(tampered));
    tampered[0] ^= 0x01U;
    TEST_ASSERT_NOT_EQUAL(
        0, crypto_sign_ed25519_verify_detached(
               tampered, (const unsigned char *)"", 0U, pk));
}

static void test_aes256_gcm_nist_vector(void)
{
    unsigned char key[32];
    unsigned char iv[12];
    unsigned char plain[64];
    unsigned char cipher[64];
    unsigned char expected[64];
    unsigned char tag[16];
    unsigned char expected_tag[16];
    unsigned char output[64];
    mbedtls_gcm_context ctx;

    TEST_ASSERT_EQUAL_INT(
        0, hex_to_bytes("feffe9928665731c6d6a8f9467308308feffe9928665731c6d6a8f"
                        "9467308308",
                        key, sizeof(key)));
    TEST_ASSERT_EQUAL_INT(
        0, hex_to_bytes("cafebabefacedbaddecaf888", iv, sizeof(iv)));
    TEST_ASSERT_EQUAL_INT(
        0, hex_to_bytes("d9313225f88406e5a55909c5aff5269a86a7a9531534f7da2e4c30"
                        "3d8a318a721c3c0c95956809532fcf0e2449a6b525b16aedf5aa0de"
                        "657ba637b391aafd255",
                        plain, sizeof(plain)));
    TEST_ASSERT_EQUAL_INT(
        0, hex_to_bytes("522dc1f099567d07f47f37a32a84427d643a8cdcbfe5c0c97598a2"
                        "bd2555d1aa8cb08e48590dbb3da7b08b1056828838c5f61e6393ba7"
                        "a0abcc9f662898015ad",
                        expected, sizeof(expected)));
    TEST_ASSERT_EQUAL_INT(
        0, hex_to_bytes("b094dac5d93471bdec1a502270e3cc6c", expected_tag,
                        sizeof(expected_tag)));

    mbedtls_gcm_init(&ctx);
    TEST_ASSERT_EQUAL_INT(0, mbedtls_gcm_setkey(&ctx, MBEDTLS_CIPHER_ID_AES, key,
                                               256U));

    TEST_ASSERT_EQUAL_INT(
        0, mbedtls_gcm_crypt_and_tag(&ctx, MBEDTLS_GCM_ENCRYPT, sizeof(plain),
                                     iv, sizeof(iv), NULL, 0U, plain, cipher,
                                     sizeof(tag), tag));
    TEST_ASSERT_EQUAL_MEMORY(expected, cipher, sizeof(expected));
    TEST_ASSERT_EQUAL_MEMORY(expected_tag, tag, sizeof(expected_tag));

    TEST_ASSERT_EQUAL_INT(
        0, mbedtls_gcm_auth_decrypt(&ctx, sizeof(expected), iv, sizeof(iv), NULL,
                                    0U, expected_tag, sizeof(expected_tag),
                                    expected, output));
    TEST_ASSERT_EQUAL_MEMORY(plain, output, sizeof(plain));

    expected_tag[0] ^= 0x01U;
    TEST_ASSERT_NOT_EQUAL(
        0, mbedtls_gcm_auth_decrypt(&ctx, sizeof(expected), iv, sizeof(iv), NULL,
                                    0U, expected_tag, sizeof(expected_tag),
                                    expected, output));

    mbedtls_gcm_free(&ctx);
}

static void test_sha256_abc(void)
{
    unsigned char digest[32];
    unsigned char expected[32];

    TEST_ASSERT_EQUAL_INT(
        0, hex_to_bytes("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff"
                        "61f20015ad",
                        expected, sizeof(expected)));

    TEST_ASSERT_EQUAL_INT(0,
                          mbedtls_sha256((const unsigned char *)"abc", 3U, digest,
                                         0));
    TEST_ASSERT_EQUAL_MEMORY(expected, digest, sizeof(expected));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ed25519_rfc8032_vector1);
    RUN_TEST(test_aes256_gcm_nist_vector);
    RUN_TEST(test_sha256_abc);
    return UNITY_END();
}
