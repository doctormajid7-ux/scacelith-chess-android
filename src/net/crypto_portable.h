// Portable cryptographic primitives for the platforms that have neither OpenSSL (the Linux
// development builds) nor the Windows BCrypt API: the Android NDK ships no crypto library the
// game may use, and what the client needs from it is small and entirely standardised (SHA-256,
// SHA-1 for the WebSocket accept key, HMAC-SHA256, HKDF-SHA256 and the OS random source).
//
// Implemented from FIPS 180-4 and RFC 2104 / RFC 5869; checked against their published test
// vectors (tools/ and tests). Deliberately boring: no tables beyond the round constants, no
// intrinsics, no floating point.
#pragma once
#include <cstddef>
#include <cstdint>

namespace net {
namespace portable {

// ---- SHA-256 (FIPS 180-4), incremental so large downloads are hashed as they arrive ----
struct Sha256State {
    uint32_t h[8] = {0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
                     0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u};
    uint64_t bytes = 0;
    uint8_t buf[64] = {};
    size_t bufLen = 0;
};
void sha256Init(Sha256State& s);
void sha256Update(Sha256State& s, const void* data, size_t n);
void sha256Final(Sha256State& s, uint8_t out[32]);
void sha256(const void* data, size_t n, uint8_t out[32]);

// ---- SHA-1 (FIPS 180-4): the WebSocket handshake's accept key only, never for a secret ----
void sha1(const void* data, size_t n, uint8_t out[20]);

// ---- MAC and key derivation ----
void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen, uint8_t out[32]);
// RFC 5869, outLen <= 255 * 32.
void hkdfSha256(const uint8_t* salt, size_t saltLen, const uint8_t* ikm, size_t ikmLen, const uint8_t* info, size_t infoLen,
                uint8_t* out, size_t outLen);

// The OS random source (/dev/urandom; false when it cannot be read).
bool randomBytes(void* out, size_t n);

}  // namespace portable
}  // namespace net
