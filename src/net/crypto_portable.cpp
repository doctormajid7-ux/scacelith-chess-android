// Portable SHA-256, SHA-1, HMAC-SHA256, HKDF-SHA256 and /dev/urandom (crypto_portable.h).
// Compiled wherever the platform has no crypto library of its own (the Android build); the
// desktop builds use OpenSSL or BCrypt and never reach this file.
#include "crypto_portable.h"
#include <cstring>
#include <cstdio>
#include <vector>

namespace net {
namespace portable {
namespace {

inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
inline uint32_t rotl(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
inline uint32_t loadBe32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | uint32_t(p[3]);
}
inline void storeBe32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24);
    p[1] = uint8_t(v >> 16);
    p[2] = uint8_t(v >> 8);
    p[3] = uint8_t(v);
}
inline void storeBe64(uint8_t* p, uint64_t v) {
    for (int i = 0; i < 8; ++i) p[i] = uint8_t(v >> (56 - 8 * i));
}

const uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u};

void sha256Block(uint32_t h[8], const uint8_t block[64]) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i) w[i] = loadBe32(block + 4 * i);
    for (int i = 16; i < 64; ++i) {
        const uint32_t a = w[i - 15], b = w[i - 2];
        const uint32_t s0 = rotr(a, 7) ^ rotr(a, 18) ^ (a >> 3);
        const uint32_t s1 = rotr(b, 17) ^ rotr(b, 19) ^ (b >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = hh + S1 + ch + kSha256K[i] + w[i];
        const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
        const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        const uint32_t t2 = S0 + maj;
        hh = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
    h[5] += f;
    h[6] += g;
    h[7] += hh;
}

void sha1Block(uint32_t h[5], const uint8_t block[64]) {
    uint32_t w[80];
    for (int i = 0; i < 16; ++i) w[i] = loadBe32(block + 4 * i);
    for (int i = 16; i < 80; ++i) w[i] = rotl(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 80; ++i) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d);
            k = 0x5a827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ed9eba1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8f1bbcdcu;
        } else {
            f = b ^ c ^ d;
            k = 0xca62c1d6u;
        }
        const uint32_t tmp = rotl(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rotl(b, 30);
        b = a;
        a = tmp;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
    h[4] += e;
}

// Final block(s) of an MD-style hash: 0x80, zeros, the length in bits big-endian.
void padTail(uint8_t* tail, size_t& tailLen, uint64_t bytes, size_t blockLen) {
    const size_t bitsLen = 8;
    size_t pad = blockLen - (bytes % blockLen);
    if (pad < 1 + bitsLen) pad += blockLen;
    tail[0] = 0x80;
    for (size_t i = 1; i < pad - bitsLen; ++i) tail[i] = 0;
    storeBe64(tail + pad - bitsLen, bytes * 8);
    tailLen = pad;
}

}  // namespace

void sha256Init(Sha256State& s) {
    s.h[0] = 0x6a09e667u;
    s.h[1] = 0xbb67ae85u;
    s.h[2] = 0x3c6ef372u;
    s.h[3] = 0xa54ff53au;
    s.h[4] = 0x510e527fu;
    s.h[5] = 0x9b05688cu;
    s.h[6] = 0x1f83d9abu;
    s.h[7] = 0x5be0cd19u;
    s.bytes = 0;
    s.bufLen = 0;
}

void sha256Update(Sha256State& s, const void* data, size_t n) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    s.bytes += n;
    if (s.bufLen) {
        const size_t take = (64 - s.bufLen) < n ? (64 - s.bufLen) : n;
        std::memcpy(s.buf + s.bufLen, p, take);
        s.bufLen += take;
        p += take;
        n -= take;
        if (s.bufLen == 64) {
            sha256Block(s.h, s.buf);
            s.bufLen = 0;
        }
    }
    while (n >= 64) {
        sha256Block(s.h, p);
        p += 64;
        n -= 64;
    }
    if (n) {
        std::memcpy(s.buf, p, n);
        s.bufLen = n;
    }
}

void sha256Final(Sha256State& s, uint8_t out[32]) {
    const uint64_t bytes = s.bytes;
    uint8_t tail[128];
    size_t tailLen = 0;
    padTail(tail, tailLen, bytes, 64);
    Sha256State copy = s;
    sha256Update(copy, tail, tailLen);   // bufLen + tailLen is a whole number of blocks
    for (int i = 0; i < 8; ++i) storeBe32(out + 4 * i, copy.h[i]);
    s.bufLen = 0;
}

void sha256(const void* data, size_t n, uint8_t out[32]) {
    Sha256State s;
    sha256Init(s);
    sha256Update(s, data, n);
    sha256Final(s, out);
}

void sha1(const void* data, size_t n, uint8_t out[20]) {
    uint32_t h[5] = {0x67452301u, 0xefcdab89u, 0x98badcfeu, 0x10325476u, 0xc3d2e1f0u};
    const uint8_t* p = static_cast<const uint8_t*>(data);
    size_t left = n;
    while (left >= 64) {
        sha1Block(h, p);
        p += 64;
        left -= 64;
    }
    uint8_t block[128] = {};
    if (left) std::memcpy(block, p, left);
    block[left] = 0x80;
    const size_t pad = (left < 56) ? 64 : 128;
    storeBe64(block + pad - 8, uint64_t(n) * 8);
    sha1Block(h, block);
    if (pad == 128) sha1Block(h, block + 64);
    for (int i = 0; i < 5; ++i) storeBe32(out + 4 * i, h[i]);
}

void hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t msgLen, uint8_t out[32]) {
    uint8_t k[64] = {};
    if (keyLen > 64) sha256(key, keyLen, k);   // longer keys are hashed first (RFC 2104)
    else std::memcpy(k, key, keyLen);
    uint8_t ipad[64], opad[64];
    for (int i = 0; i < 64; ++i) {
        ipad[i] = uint8_t(k[i] ^ 0x36);
        opad[i] = uint8_t(k[i] ^ 0x5c);
    }
    uint8_t inner[32];
    Sha256State s;
    sha256Init(s);
    sha256Update(s, ipad, sizeof(ipad));
    sha256Update(s, msg, msgLen);
    sha256Final(s, inner);
    sha256Init(s);
    sha256Update(s, opad, sizeof(opad));
    sha256Update(s, inner, sizeof(inner));
    sha256Final(s, out);
    volatile uint8_t* w = k;
    for (int i = 0; i < 64; ++i) w[i] = 0;   // the padded key is key material
}

void hkdfSha256(const uint8_t* salt, size_t saltLen, const uint8_t* ikm, size_t ikmLen, const uint8_t* info, size_t infoLen,
                uint8_t* out, size_t outLen) {
    uint8_t prk[32];
    const uint8_t zero[32] = {};
    if (!salt || saltLen == 0) hmacSha256(zero, sizeof(zero), ikm, ikmLen, prk);   // RFC 5869: a zero salt
    else hmacSha256(salt, saltLen, ikm, ikmLen, prk);
    uint8_t t[32];
    size_t tLen = 0, done = 0;
    uint8_t counter = 1;
    while (done < outLen) {
        // T(n) = HMAC-SHA256(PRK, T(n-1) || info || n); block is one such message (T(n-1) is at
        // most 32 bytes, then the caller's info).
        std::vector<uint8_t> block;
        block.reserve(tLen + infoLen + 1);
        block.insert(block.end(), t, t + tLen);
        if (infoLen) block.insert(block.end(), info, info + infoLen);
        block.push_back(counter);
        hmacSha256(prk, sizeof(prk), block.data(), block.size(), t);
        tLen = sizeof(t);
        const size_t take = (outLen - done) < tLen ? (outLen - done) : tLen;
        std::memcpy(out + done, t, take);
        done += take;
        ++counter;
    }
    volatile uint8_t* w = prk;
    for (int i = 0; i < 32; ++i) w[i] = 0;
}

bool randomBytes(void* out, size_t n) {
    if (n == 0) return true;
    FILE* f = std::fopen("/dev/urandom", "rb");
    if (!f) return false;
    const size_t got = std::fread(out, 1, n, f);
    std::fclose(f);
    return got == n;
}

}  // namespace portable
}  // namespace net
