// Android build of src/net/direct_crypto.h: direct match (two computers playing each other
// without a server) needs AES-256-GCM and ECDH on P-256, which neither the NDK nor the app may
// provide (no libcrypto, no BCrypt). The parts that only need hashing and random bytes -- the
// join codes, SHA-256, HMAC, HKDF, constant-time comparison -- are implemented in full over the
// portable primitives (crypto_portable.cpp); the two that need a cipher report the failure the
// interface already has a name for (SecureChannel::Failure::Crypto) so a host or a guest sees
// "crypto unavailable" instead of a handshake that never completes.
//
// When direct match comes to Android, the natural way is to let Java do it: javax.crypto
// (AES/GCM/NoPadding) and KeyAgreement("ECDH") over JNI, with these same signatures.
#if !defined(__ANDROID__)
// The desktop CMakeLists globs src/net/*.cpp into scacelith_core, and this file is Android-only (the
// desktop builds get direct match from direct_crypto.cpp, over BCrypt or OpenSSL). Its body is
// therefore compiled for Android alone: on any other target this is an empty translation unit.
#else
#include "direct_crypto.h"
#include "crypto_portable.h"
#include "../core/log.h"

#include <algorithm>
#include <cstring>

namespace net {
namespace direct {

namespace {
void wipe(void* p, size_t n) {
    volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
    while (n--) *v++ = 0;
}
bool warnOnce() {
    static bool warned = false;
    if (!warned) {
        warned = true;
        LOGW("direct match: this build has no AES-GCM / ECDH P-256 (Android); direct matches cannot be played");
    }
    return false;
}
}  // namespace

// ---- join codes (the same rules as the desktop builds: 31 symbols, no modulo bias) ----
std::string newJoinCode() {
    const size_t alpha = std::strlen(kCodeAlphabet);            // 31
    const unsigned limit = unsigned(256 / alpha * alpha);       // 248: rejection sampling
    std::string code;
    while (code.size() < size_t(kCodeLength)) {
        uint8_t buf[32];
        if (!randomBytes(buf, sizeof buf)) return "";
        for (uint8_t b : buf)
            if (b < limit && code.size() < size_t(kCodeLength)) code += kCodeAlphabet[b % alpha];
    }
    return code;
}

bool normalizeJoinCode(const std::string& in, std::string& out) {
    out.clear();
    for (char c : in) {
        if (c == '-' || c == ' ' || c == '\t') continue;
        if (c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
        if (!std::strchr(kCodeAlphabet, c) || c == 0) {
            out.clear();
            return false;
        }
        out += c;
        if (out.size() > size_t(kCodeLength)) {
            out.clear();
            return false;
        }
    }
    if (out.size() != size_t(kCodeLength)) {
        out.clear();
        return false;
    }
    return true;
}

std::string formatJoinCode(const std::string& c) {
    if (c.size() != size_t(kCodeLength)) return c;
    return c.substr(0, 4) + "-" + c.substr(4, 4) + "-" + c.substr(8, 4);
}

// ---- primitives ----
bool randomBytes(uint8_t* out, size_t n) { return portable::randomBytes(out, n); }
bool sha256(const uint8_t* p, size_t n, uint8_t out[32]) {
    portable::sha256(p, n, out);
    return true;
}
bool hmacSha256(const uint8_t* key, size_t keyLen, const uint8_t* msg, size_t n, uint8_t out[32]) {
    portable::hmacSha256(key, keyLen, msg, n, out);
    return true;
}
bool hkdfSha256(const uint8_t* salt, size_t saltLen, const uint8_t* ikm, size_t ikmLen, const uint8_t* info, size_t infoLen,
                uint8_t* out, size_t outLen) {
    if (outLen > 255 * 32) return false;
    portable::hkdfSha256(salt, saltLen, ikm, ikmLen, info, infoLen, out, outLen);
    return true;
}
bool constantTimeEqual(const uint8_t* a, const uint8_t* b, size_t n) {
    volatile uint8_t diff = 0;
    for (size_t i = 0; i < n; ++i) diff = uint8_t(diff | (a[i] ^ b[i]));
    return diff == 0;
}

// ---- AES-256-GCM: not available in this build ----
struct AesGcm::State {};
AesGcm::AesGcm() : s_(new State) {}
AesGcm::~AesGcm() = default;
bool AesGcm::setKey(const uint8_t[32]) { return false; }
bool AesGcm::seal(const uint8_t[12], const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*) { return false; }
bool AesGcm::open(const uint8_t[12], const uint8_t*, size_t, const uint8_t*, size_t, uint8_t*) { return false; }

// ---- ECDH P-256: not available in this build ----
struct EcdhP256::State {};
EcdhP256::EcdhP256() : s_(new State) {}
EcdhP256::~EcdhP256() = default;
bool EcdhP256::generate() { return false; }
bool EcdhP256::setKeyPair(const uint8_t[32], const uint8_t[65]) { return false; }
bool EcdhP256::agree(const uint8_t[65], uint8_t[32]) { return false; }

// ---- the channel: fails at start() with Failure::Crypto, the failure the UI already shows ----
SecureChannel::SecureChannel(Role role, const std::string& code) : role_(role), code_(code) {}
SecureChannel::~SecureChannel() { wipe(nonce_, sizeof nonce_); }
bool SecureChannel::fail(Failure f) {
    status_ = Status::Failed;
    failure_ = f;
    return false;
}
void SecureChannel::makeHello(uint8_t out[kHelloLen]) const { std::memset(out, 0, kHelloLen); }
bool SecureChannel::checkHello(const uint8_t*) { return false; }
bool SecureChannel::deriveKeys(const uint8_t*) { return false; }
bool SecureChannel::makeConfirm(const uint8_t[32], const char*, uint8_t out[kConfirmLen]) {
    std::memset(out, 0, kConfirmLen);
    return false;
}
bool SecureChannel::checkConfirm(const uint8_t[32], const char*, const uint8_t*) { return false; }
bool SecureChannel::processFrames() { return false; }
void SecureChannel::frameNonce(uint64_t, uint8_t nonce[12]) { std::memset(nonce, 0, 12); }
bool SecureChannel::start() {
    warnOnce();
    return fail(Failure::Crypto);
}
bool SecureChannel::receive(const uint8_t*, size_t) { return false; }
bool SecureChannel::popMessage(std::vector<uint8_t>&) { return false; }
bool SecureChannel::send(const uint8_t*, size_t) { return false; }

}  // namespace direct
}  // namespace net
#endif  // __ANDROID__
