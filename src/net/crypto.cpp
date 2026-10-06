#include "crypto.h"
#include <chrono>
#include <cstring>

#if defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#elif defined(__ANDROID__)
// Android: the NDK ships neither OpenSSL nor BCrypt, and the game only needs the standard
// primitives (SHA-256/1, HMAC, HKDF, the random source), so they are implemented in
// src/net/crypto_portable.cpp. Same behaviour, same test vectors.
#include "crypto_portable.h"
#else
#include <openssl/evp.h>
#include <openssl/rand.h>
#endif

namespace net {
namespace crypto {

namespace {

#if defined(_WIN32)
// Algorithm providers are opened once and kept for the life of the process.
BCRYPT_ALG_HANDLE provider(LPCWSTR id) {
    BCRYPT_ALG_HANDLE h = nullptr;
    if (BCryptOpenAlgorithmProvider(&h, id, nullptr, 0) != 0) return nullptr;
    return h;
}
BCRYPT_ALG_HANDLE sha256Provider() {
    static BCRYPT_ALG_HANDLE h = provider(BCRYPT_SHA256_ALGORITHM);
    return h;
}
BCRYPT_ALG_HANDLE sha1Provider() {
    static BCRYPT_ALG_HANDLE h = provider(BCRYPT_SHA1_ALGORITHM);
    return h;
}
bool bcryptDigest(BCRYPT_ALG_HANDLE alg, const void* data, size_t n, uint8_t* out, ULONG outLen) {
    BCRYPT_HASH_HANDLE hh = nullptr;
    if (!alg || BCryptCreateHash(alg, &hh, nullptr, 0, nullptr, 0, 0) != 0) return false;
    bool ok = BCryptHashData(hh, (PUCHAR)data, ULONG(n), 0) == 0 && BCryptFinishHash(hh, out, outLen, 0) == 0;
    BCryptDestroyHash(hh);
    return ok;
}
#endif

// Hashes prefix || suffix for many suffixes, reusing the state after the prefix.
class PrefixHasher {
public:
    explicit PrefixHasher(const std::string& prefix) {
#if defined(_WIN32)
        ok_ = sha256Provider() && BCryptCreateHash(sha256Provider(), &prefix_, nullptr, 0, nullptr, 0, 0) == 0 &&
              BCryptHashData(prefix_, (PUCHAR)prefix.data(), ULONG(prefix.size()), 0) == 0;
#elif defined(__ANDROID__)
        portable::sha256Init(prefix_);
        portable::sha256Update(prefix_, prefix.data(), prefix.size());
        ok_ = true;
#else
        prefix_ = EVP_MD_CTX_new();
        work_ = EVP_MD_CTX_new();
        ok_ = prefix_ && work_ && EVP_DigestInit_ex(prefix_, EVP_sha256(), nullptr) == 1 &&
              EVP_DigestUpdate(prefix_, prefix.data(), prefix.size()) == 1;
#endif
    }
    ~PrefixHasher() {
#if defined(_WIN32)
        if (prefix_) BCryptDestroyHash(prefix_);
#elif defined(__ANDROID__)
        // no state to free
#else
        EVP_MD_CTX_free(prefix_);
        EVP_MD_CTX_free(work_);
#endif
    }
    PrefixHasher(const PrefixHasher&) = delete;
    PrefixHasher& operator=(const PrefixHasher&) = delete;
    bool ok() const { return ok_; }

    bool hash(const char* suffix, size_t n, Sha256& out) {
#if defined(_WIN32)
        BCRYPT_HASH_HANDLE w = nullptr;
        if (BCryptDuplicateHash(prefix_, &w, nullptr, 0, 0) != 0) return false;
        bool ok = BCryptHashData(w, (PUCHAR)suffix, ULONG(n), 0) == 0 && BCryptFinishHash(w, out.data(), 32, 0) == 0;
        BCryptDestroyHash(w);
        return ok;
#elif defined(__ANDROID__)
        portable::Sha256State copy = prefix_;
        portable::sha256Update(copy, suffix, n);
        portable::sha256Final(copy, out.data());
        return true;
#else
        unsigned len = 0;
        return EVP_MD_CTX_copy_ex(work_, prefix_) == 1 && EVP_DigestUpdate(work_, suffix, n) == 1 &&
               EVP_DigestFinal_ex(work_, out.data(), &len) == 1 && len == 32;
#endif
    }

private:
    bool ok_ = false;
#if defined(_WIN32)
    BCRYPT_HASH_HANDLE prefix_ = nullptr;
#elif defined(__ANDROID__)
    portable::Sha256State prefix_;
#else
    EVP_MD_CTX* prefix_ = nullptr;
    EVP_MD_CTX* work_ = nullptr;
#endif
};

const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
const char kB64Url[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

std::string encode64(const void* data, size_t n, const char* alphabet, bool pad) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 3 <= n; i += 3) {
        uint32_t v = uint32_t(p[i]) << 16 | uint32_t(p[i + 1]) << 8 | p[i + 2];
        out += alphabet[v >> 18];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        out += alphabet[v & 63];
    }
    if (n - i == 1) {
        uint32_t v = uint32_t(p[i]) << 16;
        out += alphabet[v >> 18];
        out += alphabet[(v >> 12) & 63];
        if (pad) out += "==";
    } else if (n - i == 2) {
        uint32_t v = uint32_t(p[i]) << 16 | uint32_t(p[i + 1]) << 8;
        out += alphabet[v >> 18];
        out += alphabet[(v >> 12) & 63];
        out += alphabet[(v >> 6) & 63];
        if (pad) out += '=';
    }
    return out;
}

// Strict decoder: unused trailing bits must be zero, padding only where it belongs.
bool decode64(const std::string& in, std::vector<uint8_t>& out, bool url, bool padRequired) {
    std::string s = in;
    size_t padCount = 0;
    while (!s.empty() && s.back() == '=' && padCount < 2) { s.pop_back(); ++padCount; }
    if (padCount && (s.size() + padCount) % 4 != 0) return false;
    if (padRequired && (s.size() + padCount) % 4 != 0) return false;
    if (s.size() % 4 == 1) return false;
    out.clear();
    out.reserve(s.size() * 3 / 4);
    uint32_t acc = 0;
    int bits = 0;
    for (char c : s) {
        int v;
        if (c >= 'A' && c <= 'Z') v = c - 'A';
        else if (c >= 'a' && c <= 'z') v = c - 'a' + 26;
        else if (c >= '0' && c <= '9') v = c - '0' + 52;
        else if (c == (url ? '-' : '+')) v = 62;
        else if (c == (url ? '_' : '/')) v = 63;
        else return false;
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(uint8_t(acc >> bits));
            acc &= (1u << bits) - 1;
        }
    }
    return acc == 0;
}

// Decimal ASCII of v into buf (no terminator); returns the length.
size_t decimal(uint64_t v, char* buf) {
    char tmp[24];
    size_t n = 0;
    do { tmp[n++] = char('0' + v % 10); v /= 10; } while (v);
    for (size_t i = 0; i < n; ++i) buf[i] = tmp[n - 1 - i];
    return n;
}

}  // namespace

const char* backendName() {
#if defined(_WIN32)
    return "bcrypt";
#elif defined(__ANDROID__)
    return "portable";
#else
    return "openssl";
#endif
}

Sha256 sha256(const void* data, size_t n) {
    Sha256 out{};
#if defined(_WIN32)
    bcryptDigest(sha256Provider(), data, n, out.data(), 32);
#elif defined(__ANDROID__)
    portable::sha256(data, n, out.data());
#else
    unsigned len = 0;
    EVP_Digest(data, n, out.data(), &len, EVP_sha256(), nullptr);
#endif
    return out;
}

// ---- incremental SHA-256 ----
#if defined(_WIN32)
struct Sha256Stream::State {
    BCRYPT_HASH_HANDLE h = nullptr;
    void open() {
        if (sha256Provider()) BCryptCreateHash(sha256Provider(), &h, nullptr, 0, nullptr, 0, 0);
    }
    void close() {
        if (h) BCryptDestroyHash(h);
        h = nullptr;
    }
};
#elif defined(__ANDROID__)
struct Sha256Stream::State {
    portable::Sha256State st;
    void open() { portable::sha256Init(st); }
    void close() {}
};
#else
struct Sha256Stream::State {
    EVP_MD_CTX* ctx = nullptr;
    void open() {
        ctx = EVP_MD_CTX_new();
        if (ctx && EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) close();
    }
    void close() {
        EVP_MD_CTX_free(ctx);
        ctx = nullptr;
    }
};
#endif

Sha256Stream::Sha256Stream() : st_(new State) { st_->open(); }
Sha256Stream::~Sha256Stream() { st_->close(); }

void Sha256Stream::reset() {
    st_->close();
    st_->open();
    bytes_ = 0;
}

void Sha256Stream::update(const void* data, size_t n) {
    bytes_ += n;
#if defined(_WIN32)
    const uint8_t* p = static_cast<const uint8_t*>(data);
    while (n && st_->h) {
        ULONG k = n > 0x40000000u ? 0x40000000u : ULONG(n);
        BCryptHashData(st_->h, (PUCHAR)p, k, 0);
        p += k;
        n -= k;
    }
#elif defined(__ANDROID__)
    portable::sha256Update(st_->st, data, n);
#else
    if (st_->ctx) EVP_DigestUpdate(st_->ctx, data, n);
#endif
}

Sha256 Sha256Stream::finish() {
    Sha256 out{};
#if defined(_WIN32)
    if (st_->h) BCryptFinishHash(st_->h, out.data(), 32, 0);
#elif defined(__ANDROID__)
    portable::sha256Final(st_->st, out.data());
#else
    unsigned len = 0;
    if (st_->ctx) EVP_DigestFinal_ex(st_->ctx, out.data(), &len);
#endif
    reset();
    return out;
}

Sha1 sha1(const void* data, size_t n) {
    Sha1 out{};
#if defined(_WIN32)
    bcryptDigest(sha1Provider(), data, n, out.data(), 20);
#elif defined(__ANDROID__)
    portable::sha1(data, n, out.data());
#else
    unsigned len = 0;
    EVP_Digest(data, n, out.data(), &len, EVP_sha1(), nullptr);
#endif
    return out;
}

bool randomBytes(void* out, size_t n) {
    if (n == 0) return true;
#if defined(_WIN32)
    return BCryptGenRandom(nullptr, static_cast<PUCHAR>(out), ULONG(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG) == 0;
#elif defined(__ANDROID__)
    return portable::randomBytes(out, n);
#else
    return RAND_bytes(static_cast<unsigned char*>(out), int(n)) == 1;
#endif
}

std::string base64(const void* data, size_t n) { return encode64(data, n, kB64, true); }
std::string base64url(const void* data, size_t n) { return encode64(data, n, kB64Url, false); }
bool base64Decode(const std::string& s, std::vector<uint8_t>& out) { return decode64(s, out, false, true); }
bool base64urlDecode(const std::string& s, std::vector<uint8_t>& out) { return decode64(s, out, true, false); }

std::string hex(const void* data, size_t n) {
    static const char d[] = "0123456789abcdef";
    const uint8_t* p = static_cast<const uint8_t*>(data);
    std::string s(n * 2, '0');
    for (size_t i = 0; i < n; ++i) {
        s[2 * i] = d[p[i] >> 4];
        s[2 * i + 1] = d[p[i] & 15];
    }
    return s;
}

bool hexDecode(const std::string& s, std::vector<uint8_t>& out) {
    if (s.size() % 2) return false;
    out.clear();
    out.reserve(s.size() / 2);
    auto nib = [](char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < s.size(); i += 2) {
        int a = nib(s[i]), b = nib(s[i + 1]);
        if (a < 0 || b < 0) return false;
        out.push_back(uint8_t(a << 4 | b));
    }
    return true;
}

bool constantTimeEqual(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff = uint8_t(diff | (uint8_t(a[i]) ^ uint8_t(b[i])));
    return diff == 0;
}

std::string pkceChallenge(const std::string& verifier) {
    Sha256 d = sha256(verifier);
    return base64url(d.data(), d.size());
}

bool makePkce(Pkce& out) {
    uint8_t r[32];
    if (!randomBytes(r, sizeof(r))) return false;
    out.verifier = base64url(r, sizeof(r));
    out.challenge = pkceChallenge(out.verifier);
    std::memset(r, 0, sizeof(r));
    return true;
}

int leadingZeroBits(const Sha256& d) {
    int n = 0;
    for (uint8_t b : d) {
        if (b == 0) { n += 8; continue; }
        for (int bit = 7; bit >= 0 && !(b >> bit & 1); --bit) ++n;
        break;
    }
    return n;
}

bool powCheck(const std::string& challenge, const std::string& nonce, int bits) {
    if (bits < 0 || bits > 256 || nonce.empty() || nonce.size() > 20) return false;
    for (char c : nonce)
        if (c < '0' || c > '9') return false;
    return leadingZeroBits(sha256(challenge + ":" + nonce)) >= bits;
}

bool powSolve(const std::string& challenge, int bits, std::string& nonce, const std::atomic<bool>* cancel, PowStats* stats,
              uint64_t maxHashes, std::atomic<uint64_t>* progress) {
    auto t0 = std::chrono::steady_clock::now();
    uint64_t done = 0;
    auto finish = [&](bool ok) {
        if (stats) {
            stats->hashes = done;
            stats->seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        }
        if (progress) progress->store(done, std::memory_order_relaxed);
        return ok;
    };
    if (bits < 0 || bits > kPowMaxBits) return finish(false);
    PrefixHasher hasher(challenge + ":");
    if (!hasher.ok()) return finish(false);
    // A full leading byte check first, then the partial one: most hashes fail on byte 0.
    const int fullBytes = bits / 8, restBits = bits % 8;
    const uint8_t restMask = uint8_t(0xFF00 >> restBits);
    char buf[24];
    Sha256 d;
    for (uint64_t i = 0; done < maxHashes; ++i) {
        if ((i & 4095) == 0) {
            if (cancel && cancel->load(std::memory_order_relaxed)) return finish(false);
            if (progress) progress->store(done, std::memory_order_relaxed);
        }
        size_t len = decimal(i, buf);
        if (!hasher.hash(buf, len, d)) return finish(false);
        ++done;
        bool ok = true;
        for (int k = 0; k < fullBytes && ok; ++k) ok = d[size_t(k)] == 0;
        if (ok && restBits) ok = (d[size_t(fullBytes)] & restMask) == 0;
        if (ok) {
            nonce.assign(buf, len);
            return finish(true);
        }
    }
    return finish(false);
}

}  // namespace crypto
}  // namespace net
