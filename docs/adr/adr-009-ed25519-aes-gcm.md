# ADR-009 — Ed25519 + AES-256-GCM for OTA image signing and encryption

- **Date:** 2026-11-18
- **Status:** Accepted
- **Decision makers:** Project maintainer
- **Project context:** The `.ota` images must be authentic (signed) and, optionally, confidential (encrypted), with verification in the bootloader before applying.

## Context

The `.ota` format requires:

- **Authenticity and integrity:** guarantee that the image came from the legitimate author and was not altered.
- **Confidentiality (optional):** prevent an attacker from reading the binary (to hinder reverse engineering or protect embedded secrets).
- **Efficiency:** verification must be fast on an MCU (or in the POSIX bootloader).
- **Size:** signatures and keys must not bloat the `.ota` unnecessarily.
- **Standardization:** widely adopted and audited algorithms.

Two combinations were evaluated:

1. **RSA-2048 + AES-256-CBC + SHA-256** (the classic combination).
2. **Ed25519 + AES-256-GCM** (the modern combination).

## Decision

**Ed25519 + AES-256-GCM is chosen:**

- **Signature:** Ed25519 (RFC 8032), with a 32 B private key and a 64 B signature.
- **Encryption:** AES-256-GCM (NIST SP 800-38D), with a 12 B nonce and a 16 B tag.
- **Hash:** SHA-256 for `payload_sha256` (redundant with GCM, but useful for debugging).

## Alternatives considered

### Alternative A: RSA-2048 + AES-256-CBC + SHA-256

**Advantages:**
- **Widely adopted:** RSA has been the de facto standard in secure boot for decades.
- **Audited:** mature implementations on every platform.
- **Compatible with old HSMs:** many HSMs only support RSA.

**Drawbacks:**
- **Large keys:** RSA-2048 requires 256 B for the public key and 256 B for the signature.
- **Slow verification on MCU:** ~10-50 ms on Cortex-M4, vs. ~1 ms for Ed25519.
- **AES-CBC requires padding:** it adds complexity and error surface (padding oracle attacks).
- **AES-CBC does not authenticate:** it requires a separate hash (SHA-256) and a separate verification.
- **Two operations:** encryption + hash + signature verification. More error surface.

**Discarded because:** it is slower, larger and more error-prone than the modern combination.

### Alternative B: Ed25519 + AES-256-GCM (chosen)

**Advantages:**
- **Small keys:** Ed25519 uses 32 B for the public key and 64 B for the signature (vs. 256+256 for RSA).
- **Fast verification:** ~1 ms on Cortex-M4, vs. ~10-50 ms for RSA.
- **AES-GCM authenticates:** the 16 B tag guarantees integrity **and** authenticity in a single pass.
- **A single encryption operation:** it does not require a separate hash for integrity (although `payload_sha256` is kept for debugging).
- **Modern standard:** TLS 1.3, Signal, WireGuard, Zephyr, MCUboot, ESP32 Secure Boot v2.
- **Side-channel resistance:** Ed25519 is designed with constant-time operations.
- **Explicit nonce:** AES-GCM requires a 12 B nonce, which is included in the header.

**Drawbacks:**
- **Less adoption than RSA:** although growing, some old HSMs do not support Ed25519.
- **Nonce reuse is catastrophic:** reusing the nonce with the same key breaks GCM completely. Mitigation: `os.urandom(12)` on every packaging operation, and never reuse.
- **Implementation in C requires a library:** `libsodium` or `mbedtls`. Implementing Ed25519 from scratch is not trivial.

**Chosen because:** it is faster, smaller, more secure and more modern than RSA + AES-CBC.

### Alternative C: ECDSA P-256 + AES-256-GCM (not evaluated in depth)

**Advantages:**
- **Small keys:** similar to Ed25519.
- **Widely adopted:** ECDSA is the standard in TLS and many secure boot implementations.
- **Compatible with modern HSMs:** most HSMs support ECDSA P-256.

**Drawbacks:**
- **Slower verification than Ed25519:** ECDSA requires more operations during verification.
- **More complex implementation:** ECDSA has more parameters (curve, point, order) than Ed25519.
- **Not deterministic:** ECDSA requires a random nonce per signature, which adds a source of error (nonce reuse is catastrophic, just like in GCM).

**Discarded because:** Ed25519 is simpler, faster and more secure for the use case.

## Consequences

### Positive

- **Fast verification:** ~1 ms on Cortex-M4, which allows fast boots.
- **Small signatures:** 64 B vs. 256 B for RSA, which reduces the `.ota` size.
- **A single encryption operation:** AES-GCM combines confidentiality and integrity in one pass.
- **Modern standard:** aligned with current best practices.
- **Side-channel resistance:** Ed25519 is designed with constant-time operations.

### Negative

- **Less adoption than RSA:** RSA is more common in existing designs. Mitigation: the choice is documented in the README and in this ADR.
- **Catastrophic nonce reuse:** it requires discipline during packaging. Mitigation: `os.urandom(12)` for every `.ota`, and never reuse.
- **Dependence on `libsodium` or `mbedtls`:** it cannot be implemented from scratch without risk. Mitigation: use mature, audited libraries.

### Neutral

- **The header is sent in clear text but signed:** the bootloader can read `fw_version` and `hw_compat` before decrypting.
- **`payload_sha256` is kept:** although GCM already authenticates, the SHA-256 of the clear payload makes debugging easier and provides defense in depth.
- **The nonce is included in the header:** the bootloader needs it to decrypt.

## Implementation notes

### Packaging (`tools/package_ota.py`)

```python
import os, hashlib, struct
from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from nacl.signing import SigningKey

MAGIC = b"LAMPOTA1"
FORMAT_VERSION = 1

def package(payload: bytes, version: tuple, aes_key: bytes, signing_key: bytes) -> bytes:
    major, minor, patch = version
    payload_sha256 = hashlib.sha256(payload).digest()
    nonce = os.urandom(12)  # NEVER reuse
    aesgcm = AESGCM(aes_key)
    # Provisional header for AAD
    header_prov = struct.pack(
        "<8sH6sI32s12s32s28s",
        MAGIC, FORMAT_VERSION,
        struct.pack("<HHH", major, minor, patch),
        0x00000001, len(payload), payload_sha256, nonce,
        b"\x00" * 32, b"\x00" * 28,
    )
    aad_hash = hashlib.sha256(header_prov).digest()
    ct_with_tag = aesgcm.encrypt(nonce, payload, aad_hash)
    ciphertext, gcm_tag = ct_with_tag[:-16], ct_with_tag[-16:]
    header = struct.pack(
        "<8sH6sI32s12s32s28s",
        MAGIC, FORMAT_VERSION,
        struct.pack("<HHH", major, minor, patch),
        0x00000001, len(payload), payload_sha256, nonce,
        aad_hash, b"\x00" * 28,
    )
    message = header + ciphertext + gcm_tag
    signature = SigningKey(signing_key).sign(message).signature
    return message + signature
```

### Verification in the bootloader (`firmware/src/bootloader/ota_verify.c`)

```c
int verify_ota(const uint8_t *ota, size_t len, const uint8_t *pubkey) {
    // 1. Verify magic and format_version
    if (memcmp(ota, "LAMPOTA1", 8) != 0) return FAULT_FORMAT;
    // 2. Verify Ed25519 over header || ciphertext || gcm_tag
    const uint8_t *message = ota;
    size_t message_len = len - 64;
    const uint8_t *signature = ota + message_len;
    if (crypto_sign_verify_detached(signature, message, message_len, pubkey) != 0)
        return FAULT_SIGNATURE;
    // 3. Decrypt AES-GCM
    // ...
    // 4. Verify payload_sha256
    // ...
    return 0;
}
```

### Key rotation

- The new public key must be in the bootloader **before** rotating the private key.
- This requires a bootloader OTA (which is a special case: the bootloader cannot update itself on a real MCU, but in POSIX it can be simulated).

## References

- [`04-ota-and-image-security.md`](../04-ota-and-image-security.md) — section 3
- [Ed25519 (RFC 8032)](https://datatracker.ietf.org/doc/html/rfc8032)
- [AES-GCM (NIST SP 800-38D)](https://csrc.nist.gov/publications/detail/sp/800-38d/final)
- [libsodium — Ed25519](https://doc.libsodium.org/public-key_cryptography/public-key_signatures)
- [MCUboot image format](https://docs.mcuboot.com/design.html)
- [Why Ed25519?](https://blog.cloudflare.com/ed25519-performance/)
