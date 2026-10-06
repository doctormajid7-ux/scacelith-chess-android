// Audio package tests: every sound is rendered through the full chain and checked numerically
// (no clipping, no DC, no NaN/denormals, durations, spectral centroids), the hall reverb RT60
// and pre-delay are measured, the 3D stage is probed, the lock-free queue is stressed, the
// mixer CPU cost is measured and the live engine is started/stopped. The speech voices (coach)
// are checked for timing, seamless phrase joins, level, directivity, ducking, fades, ownership of
// their chunks and the live API (tests at the end of the file).
// WAV files for listening are written to /tmp/audio_out/ (Windows: %TEMP%\scacelith_audio_out).
#include "test.h"
#include "audio/audio.h"
#include "audio/backend.h"
#include "audio/backend_alsa.h"
#include "audio/mixer.h"
#include "audio/offline.h"
#include "audio/queue.h"
#include "audio/reverb.h"
#include "audio/synth.h"
#include <atomic>
#include <chrono>
#include <cfloat>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <vector>
#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#else
#include <time.h>
#endif

namespace {

constexpr float kFs = 48000.0f;
constexpr float kMinus1dB = 0.8912509f;

void makeDir(const char* p) {
#ifdef _WIN32
    _mkdir(p);
#else
    mkdir(p, 0755);
#endif
}

std::string outDir() {
#ifdef _WIN32
    // %TEMP%\scacelith_audio_out (never the working directory, which is usually the repo root).
    char tmp[MAX_PATH + 1] = {};
    DWORD n = GetTempPathA(MAX_PATH, tmp);
    std::string d = (n > 0 && n <= MAX_PATH) ? std::string(tmp) : std::string(".\\");
    d += "scacelith_audio_out";
#else
    std::string d = "/tmp/audio_out";
#endif
    makeDir(d.c_str());
    return d;
}

double threadCpuSeconds() {
#ifdef _WIN32
    FILETIME c, e, k, u;
    if (!GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)) return 0.0;
    auto f = [](FILETIME t) { return double((uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime) * 1e-7; };
    return f(k) + f(u);
#else
    timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return double(ts.tv_sec) + double(ts.tv_nsec) * 1e-9;
#endif
}

// Magnitude-weighted spectral centroid of a mono signal (Hz).
float spectralCentroid(const std::vector<float>& x) {
    size_t n = 1;
    while (n < x.size() && n < (1u << 18)) n <<= 1;
    std::vector<std::complex<float>> a(n);
    for (size_t i = 0; i < n && i < x.size(); ++i) {
        float w = 0.5f - 0.5f * std::cos(6.2831853f * float(i) / float(std::min(n, x.size())));  // Hann over the data
        a[i] = x[i] * (x.size() > 4096 ? 1.0f : w);
    }
    for (size_t i = 1, j = 0; i < n; ++i) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        float ang = -6.2831853f / float(len);
        std::complex<float> wl(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (size_t j = 0; j < len / 2; ++j) {
                std::complex<float> u = a[i + j], v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
                w *= wl;
            }
        }
    }
    double num = 0.0, den = 0.0;
    for (size_t k = 1; k < n / 2; ++k) {
        double f = double(k) * kFs / double(n);
        double mag = std::abs(a[k]);
        num += f * mag;
        den += mag;
    }
    return den > 0.0 ? float(num / den) : 0.0f;
}

struct Analysis {
    float peak = 0, rms = 0, dcL = 0, dcR = 0, centroid = 0, duration = 0;
    bool finite = true;
    int denormals = 0;
};

// Interleaved stereo analysis. duration = time until the (10 ms RMS) level stays below -60 dB
// relative to the loudest 10 ms window.
Analysis analyzeStereo(const std::vector<float>& s) {
    Analysis a;
    size_t frames = s.size() / 2;
    double sum2 = 0.0, sL = 0.0, sR = 0.0;
    std::vector<float> mono(frames);
    for (size_t i = 0; i < frames; ++i) {
        float l = s[2 * i], r = s[2 * i + 1];
        if (!std::isfinite(l) || !std::isfinite(r)) a.finite = false;
        if ((l != 0.0f && std::fabs(l) < FLT_MIN) || (r != 0.0f && std::fabs(r) < FLT_MIN)) ++a.denormals;
        a.peak = std::max(a.peak, std::max(std::fabs(l), std::fabs(r)));
        sum2 += double(l) * l + double(r) * r;
        sL += l;
        sR += r;
        mono[i] = 0.5f * (l + r);
    }
    if (frames) {
        a.rms = float(std::sqrt(sum2 / double(2 * frames)));
        a.dcL = float(sL / double(frames));
        a.dcR = float(sR / double(frames));
    }
    a.centroid = spectralCentroid(mono);
    const size_t win = 480;
    std::vector<float> lv;
    float maxLv = 0.0f;
    for (size_t i = 0; i + win <= frames; i += win) {
        double e = 0.0;
        for (size_t k = 0; k < win; ++k) e += double(mono[i + k]) * mono[i + k];
        lv.push_back(float(std::sqrt(e / win)));
        maxLv = std::max(maxLv, lv.back());
    }
    size_t last = 0;
    for (size_t i = 0; i < lv.size(); ++i)
        if (lv[i] > maxLv * 1e-3f) last = i + 1;
    a.duration = float(last * win) / kFs;
    return a;
}

float db(float g) { return 20.0f * std::log10(std::max(g, 1e-9f)); }

// Clicks/discontinuities: largest |HP 12 kHz (4th order)| relative to the RMS of the signal.
float clickRatio(const std::vector<float>& s) {
    audio::dsp::Svf a, b;
    a.set(12000.0f, 0.707f, kFs);
    b.set(12000.0f, 0.707f, kFs);
    float mx = 0.0f;
    double e = 0.0;
    for (size_t i = 0; i < s.size() / 2; ++i) {
        float x = 0.5f * (s[2 * i] + s[2 * i + 1]);
        e += double(x) * x;
        mx = std::max(mx, std::fabs(b.hp(a.hp(x))));
    }
    float rms = float(std::sqrt(e / double(std::max<size_t>(1, s.size() / 2))));
    return mx / std::max(rms, 1e-12f);
}

void installAllSounds(audio::Mixer& m, uint32_t seed) {
    for (int s = 0; s < int(audio::Sfx::Count); ++s)
        for (int v = 0; v < audio::bankVariants(audio::Sfx(s)); ++v) {
            audio::SoundBuffer* b = new audio::SoundBuffer();
            b->samples = audio::synthesize(audio::Sfx(s), seed * 131u + uint32_t(s * 17 + v));
            b->sfx = s;
            b->variant = v;
            m.install(b);
        }
}

struct Expect {
    audio::Sfx sfx;
    float seconds;         // render length (tail included)
    float minCentroid, maxCentroid;
    float minDur, maxDur;  // dry synthesis duration (s) at -60 dB
};
const Expect kExpect[] = {
    {audio::Sfx::PiecePickup, 1.2f, 1500.0f, 6000.0f, 0.03f, 0.5f},
    {audio::Sfx::PiecePlace, 1.5f, 400.0f, 2200.0f, 0.04f, 0.5f},
    {audio::Sfx::Capture, 2.5f, 900.0f, 6000.0f, 0.6f, 1.4f},
    {audio::Sfx::ClockPress, 1.5f, 900.0f, 5000.0f, 0.04f, 0.4f},
    {audio::Sfx::Handshake, 2.5f, 200.0f, 2500.0f, 0.5f, 1.6f},
    {audio::Sfx::ServoShort, 1.5f, 250.0f, 2500.0f, 0.2f, 0.9f},
    {audio::Sfx::ChairCreak, 2.0f, 250.0f, 2500.0f, 0.3f, 1.2f},
    {audio::Sfx::UIHover, 0.6f, 900.0f, 4000.0f, 0.005f, 0.08f},
    {audio::Sfx::UIClick, 0.8f, 600.0f, 3500.0f, 0.01f, 0.13f},
    {audio::Sfx::GameStart, 6.0f, 150.0f, 900.0f, 1.5f, 4.5f},
    {audio::Sfx::GameEnd, 7.0f, 120.0f, 800.0f, 2.0f, 5.5f},
    {audio::Sfx::CaptureClick, 1.5f, 1500.0f, 7000.0f, 0.05f, 0.45f},
    {audio::Sfx::TablePlace, 1.5f, 300.0f, 1800.0f, 0.04f, 0.5f},
    {audio::Sfx::PenWrite, 4.0f, 1500.0f, 7000.0f, 2.8f, 3.45f},
    {audio::Sfx::PenTap, 0.8f, 500.0f, 6000.0f, 0.02f, 0.13f},
    {audio::Sfx::PageTurn, 2.0f, 900.0f, 6000.0f, 0.8f, 1.35f},
    {audio::Sfx::PageFlap, 1.2f, 200.0f, 3500.0f, 0.08f, 0.45f},
};
static_assert(sizeof(kExpect) / sizeof(kExpect[0]) == size_t(audio::Sfx::Count), "every sound has expectations");

}  // namespace

TEST(audio_synth_variants_sane) {
    for (const Expect& ex : kExpect) {
        std::vector<float> prev;
        for (uint32_t seed = 1; seed <= 5; ++seed) {
            std::vector<float> v = audio::synthesize(ex.sfx, seed * 7777u);
            CHECK(v.size() > 64);
            CHECK_EQ(v.capacity(), v.size());  // the bank keeps no synthesis scratch capacity
            float peak = 0.0f;
            bool finite = true;
            int den = 0;
            for (float x : v) {
                finite = finite && std::isfinite(x);
                if (x != 0.0f && std::fabs(x) < FLT_MIN) ++den;
                peak = std::max(peak, std::fabs(x));
            }
            CHECK(finite);
            CHECK_EQ(den, 0);
            CHECK(std::fabs(peak - 1.0f) < 1e-3f);
            CHECK(std::fabs(v.front()) < 0.05f);           // no click at the start
            CHECK(std::fabs(v.back()) < 1e-3f);            // faded tail
            {                                              // not truncated: last 20 ms < -45 dB
                size_t n20 = std::min(v.size(), size_t(0.02f * kFs));
                double e = 0.0;
                for (size_t i = v.size() - n20; i < v.size(); ++i) e += double(v[i]) * v[i];
                float lastDb = 10.0f * std::log10(float(e / double(n20)) + 1e-20f);
                if (lastDb > -45.0f) {
                    std::fprintf(stderr, "  %s seed %u: tail truncated (last 20 ms at %.1f dB)\n", audio::sfxName(ex.sfx), seed, lastDb);
                    CHECK(false);
                }
            }
            float dur = float(v.size()) / kFs;
            if (dur < ex.minDur || dur > ex.maxDur + 0.01f) {
                std::fprintf(stderr, "  %s seed %u: duration %.3f s outside [%.3f, %.3f]\n", audio::sfxName(ex.sfx), seed, dur,
                             ex.minDur, ex.maxDur);
                CHECK(false);
            }
            if (!prev.empty()) {  // every variant differs
                size_t n = std::min(prev.size(), v.size());
                double dotp = 0, e1 = 0, e2 = 0;
                for (size_t i = 0; i < n; ++i) {
                    dotp += double(prev[i]) * v[i];
                    e1 += double(prev[i]) * prev[i];
                    e2 += double(v[i]) * v[i];
                }
                double corr = dotp / std::sqrt(e1 * e2 + 1e-30);
                CHECK(corr < 0.995);
            }
            prev = v;
        }
    }
}

TEST(audio_render_all_wavs_and_check) {
    std::string dir = outDir();
    std::fprintf(stderr, "  %-14s %8s %8s %9s %9s %9s %8s\n", "sound", "peak dB", "rms dB", "DC", "centroid", "dur(s)", "limGR dB");
    for (const Expect& ex : kExpect) {
        audio::OfflineStats st;
        std::vector<float> buf = audio::renderSfxOffline(ex.sfx, ex.seconds, 1u, &st);
        Analysis a = analyzeStereo(buf);
        std::fprintf(stderr, "  %-14s %8.2f %8.2f %9.2e %9.0f %9.3f %8.2f\n", audio::sfxName(ex.sfx), db(a.peak), db(a.rms),
                     std::max(std::fabs(a.dcL), std::fabs(a.dcR)), a.centroid, a.duration, db(st.limiterMinGain));
        CHECK(a.finite);
        CHECK_EQ(a.denormals, 0);
        CHECK(a.peak <= kMinus1dB);
        CHECK(a.peak > 0.003f);                                  // audible (> -50 dBFS)
        CHECK(std::fabs(a.dcL) < 2e-4f && std::fabs(a.dcR) < 2e-4f);
        CHECK(st.limiterMinGain > 0.7f);                         // levels are designed, not limited
        if (a.centroid < ex.minCentroid || a.centroid > ex.maxCentroid) {
            std::fprintf(stderr, "  %s: centroid %.0f Hz outside [%.0f, %.0f]\n", audio::sfxName(ex.sfx), a.centroid,
                         ex.minCentroid, ex.maxCentroid);
            CHECK(false);
        }
        std::string path = dir + "/" + audio::sfxName(ex.sfx) + ".wav";
        CHECK(audio::writeWav16(path.c_str(), buf.data(), buf.size() / 2, 2, 48000));
    }
    // Public API path (same content as above for seed 1).
    std::string p = dir + "/piece_place_api.wav";
    CHECK(audio::renderToWav(audio::Sfx::PiecePlace, p.c_str(), 1.5f));
    struct stat sb {};
    CHECK(stat(p.c_str(), &sb) == 0 && sb.st_size == 44 + 48000 * 3 / 2 * 4);
    std::remove(p.c_str());
}

TEST(audio_ambience_30s) {
    std::string dir = outDir();
    audio::OfflineStats st;
    std::vector<float> buf = audio::renderAmbienceOffline(30.0f, 1u, &st);
    Analysis a = analyzeStereo(buf);
    std::fprintf(stderr, "  ambience 30 s: peak %.2f dBFS, rms %.2f dBFS, DC %.1e, centroid %.0f Hz\n", db(a.peak), db(a.rms),
                 std::max(std::fabs(a.dcL), std::fabs(a.dcR)), a.centroid);
    CHECK(a.finite);
    CHECK_EQ(a.denormals, 0);
    CHECK(a.peak <= kMinus1dB);
    CHECK(db(a.rms) > -62.0f && db(a.rms) < -36.0f);  // very low level room tone
    CHECK(a.centroid > 80.0f && a.centroid < 2500.0f);
    CHECK(std::fabs(a.dcL) < 2e-4f && std::fabs(a.dcR) < 2e-4f);
    // No audible loop: the 10-20 s segment is not a copy of the 0-10 s one.
    size_t seg = size_t(10 * kFs) * 2;
    double dotp = 0, e1 = 0, e2 = 0;
    for (size_t i = 0; i < seg; ++i) {
        dotp += double(buf[i]) * buf[seg + i];
        e1 += double(buf[i]) * buf[i];
        e2 += double(buf[seg + i]) * buf[seg + i];
    }
    CHECK(std::fabs(dotp / std::sqrt(e1 * e2 + 1e-30)) < 0.2);
    // Slow level variation (breathing) exists: 1 s RMS spread over the file.
    float lo = 1e9f, hi = 0.0f;
    for (size_t s = 1; s < 29; ++s) {
        double e = 0;
        for (size_t i = size_t(s * kFs) * 2; i < size_t((s + 1) * kFs) * 2; ++i) e += double(buf[i]) * buf[i];
        float r = float(std::sqrt(e / (2.0 * kFs)));
        lo = std::min(lo, r);
        hi = std::max(hi, r);
    }
    std::fprintf(stderr, "  ambience 1 s RMS range: %.1f .. %.1f dBFS\n", db(lo), db(hi));
    CHECK(db(hi) - db(lo) > 1.0f);
    CHECK(st.limiterMinGain > 0.9f);
    float clicks = clickRatio(buf);
    std::fprintf(stderr, "  ambience click ratio (max |HP12k| / RMS): %.2f\n", clicks);
    CHECK(clicks < 1.0f);
    std::string path = dir + "/ambience_30s.wav";
    CHECK(audio::writeWav16(path.c_str(), buf.data(), buf.size() / 2, 2, 48000));
    CHECK(audio::renderAmbienceToWav((dir + "/ambience_api_check.wav").c_str(), 1.0f));
    std::remove((dir + "/ambience_api_check.wav").c_str());
}

TEST(audio_reverb_rt60_and_predelay) {
    audio::HallReverb rv;
    rv.prepare(kFs);
    const int n = int(5.0f * kFs);
    std::vector<float> in(size_t(n), 0.0f), L(size_t(n), 0.0f), R(size_t(n), 0.0f);
    in[0] = 1.0f;
    rv.earlyGain = 0.0f;
    rv.process(in.data(), L.data(), R.data(), n);
    // Energy decay curve (Schroeder backward integration) on L+R.
    auto rt60 = [&](const std::vector<float>& l, const std::vector<float>& r) {
        std::vector<double> edc(size_t(n) + 1, 0.0);
        for (int i = n - 1; i >= 0; --i) edc[size_t(i)] = edc[size_t(i) + 1] + double(l[size_t(i)]) * l[size_t(i)] + double(r[size_t(i)]) * r[size_t(i)];
        double e0 = edc[0];
        int i5 = -1, i35 = -1;
        for (int i = 0; i < n; ++i) {
            double d = 10.0 * std::log10(edc[size_t(i)] / e0 + 1e-30);
            if (i5 < 0 && d <= -5.0) i5 = i;
            if (i35 < 0 && d <= -35.0) { i35 = i; break; }
        }
        if (i5 < 0 || i35 < 0) return 99.0f;
        return float(i35 - i5) / kFs * 2.0f;  // 30 dB span -> 60 dB
    };
    double energy = 0.0;
    int onset = -1;
    for (int i = 0; i < n; ++i) {
        energy += double(L[size_t(i)]) * L[size_t(i)] + double(R[size_t(i)]) * R[size_t(i)];
        if (onset < 0 && (std::fabs(L[size_t(i)]) > 1e-4f || std::fabs(R[size_t(i)]) > 1e-4f)) onset = i;
    }
    float t60 = rt60(L, R);
    // Band-split: high band (> 5 kHz) must decay faster than the low band (< 1 kHz).
    std::vector<float> hl(L), hr(R), ll(L), lr(R);
    audio::dsp::Svf f1, f2, f3, f4;
    f1.set(5000, 0.7f, kFs); f2.set(5000, 0.7f, kFs); f3.set(1000, 0.7f, kFs); f4.set(1000, 0.7f, kFs);
    for (int i = 0; i < n; ++i) {
        hl[size_t(i)] = f1.hp(L[size_t(i)]);
        hr[size_t(i)] = f2.hp(R[size_t(i)]);
        ll[size_t(i)] = f3.lp(L[size_t(i)]);
        lr[size_t(i)] = f4.lp(R[size_t(i)]);
    }
    float tHigh = rt60(hl, hr), tLow = rt60(ll, lr);
    // Stereo decorrelation of the tail.
    double lr2 = 0, l2 = 0, r2 = 0;
    for (int i = int(0.1f * kFs); i < n; ++i) {
        lr2 += double(L[size_t(i)]) * R[size_t(i)];
        l2 += double(L[size_t(i)]) * L[size_t(i)];
        r2 += double(R[size_t(i)]) * R[size_t(i)];
    }
    float corr = float(lr2 / std::sqrt(l2 * r2 + 1e-30));
    std::fprintf(stderr, "  RT60 %.2f s (low %.2f s, high %.2f s), onset %.1f ms, IR energy %.3f, L/R corr %.2f\n", t60, tLow,
                 tHigh, onset * 1000.0f / kFs, energy, corr);
    CHECK(t60 > 2.0f && t60 < 2.7f);
    CHECK(tHigh < tLow * 0.8f);
    CHECK(onset >= int(0.018f * kFs) && onset <= int(0.032f * kFs));
    CHECK(energy > 0.5 && energy < 2.0);
    CHECK(std::fabs(corr) < 0.3f);
    for (float x : L) CHECK(std::isfinite(x));
}

TEST(audio_spatial_pan_itd_behind_distance) {
    using namespace audio;
    ListenerPose lis;
    lis.pos = m::vec3(0, 1.2f, 0);
    lis.fwd = m::vec3(0, 0, -1);
    lis.up = m::vec3(0, 1, 0);
    auto energy = [](const std::vector<float>& b, int ch) {
        double e = 0;
        for (size_t i = ch; i < b.size(); i += 2) e += double(b[i]) * b[i];
        return e;
    };
    // Source 0.5 m to the right: right louder, left ear delayed by the ITD.
    std::vector<float> right = renderSfxOfflineAt(Sfx::PiecePlace, 0.5f, m::vec3(0.5f, 1.2f, 0.0f), lis, 3u, false);
    double eL = energy(right, 0), eR = energy(right, 1);
    int bestLag = 0;
    double best = -1e30;
    for (int lag = -40; lag <= 40; ++lag) {
        double s = 0;
        for (size_t i = 100; i + 100 < right.size() / 2; ++i) {
            long j = long(i) + lag;
            s += double(right[2 * i + 1]) * right[size_t(2 * j)];
        }
        if (s > best) { best = s; bestLag = lag; }
    }
    std::fprintf(stderr, "  right source: R/L %.1f dB, left-ear lag %d samples\n", 10.0 * std::log10(eR / eL), bestLag);
    CHECK(10.0 * std::log10(eR / eL) > 6.0);
    CHECK(bestLag >= 20 && bestLag <= 36);  // ~0.66 ms at 48 kHz
    // Front vs behind: behind is darker.
    std::vector<float> front = renderSfxOfflineAt(Sfx::CaptureClick, 0.5f, m::vec3(0, 1.2f, -0.6f), lis, 3u, false);
    std::vector<float> back = renderSfxOfflineAt(Sfx::CaptureClick, 0.5f, m::vec3(0, 1.2f, 0.6f), lis, 3u, false);
    std::vector<float> fm(front.size() / 2), bm(back.size() / 2);
    for (size_t i = 0; i < fm.size(); ++i) { fm[i] = front[2 * i]; bm[i] = back[2 * i]; }
    float cf = spectralCentroid(fm), cb = spectralCentroid(bm);
    std::fprintf(stderr, "  centroid front %.0f Hz, behind %.0f Hz\n", cf, cb);
    CHECK(cb < cf * 0.85f);
    // Inverse distance: 0.3 m vs 1.2 m -> ~12 dB; below 0.15 m no further boost.
    auto level = [&](float d) {
        std::vector<float> b = renderSfxOfflineAt(Sfx::PiecePlace, 0.4f, m::vec3(0, 1.2f, -d), lis, 3u, false);
        return 10.0 * std::log10(energy(b, 0) + energy(b, 1));
    };
    double l03 = level(0.3f), l12 = level(1.2f), l01 = level(0.1f), l015 = level(0.15f);
    std::fprintf(stderr, "  distance: 0.3 m vs 1.2 m %.1f dB, 0.1 m vs 0.15 m %.1f dB\n", l03 - l12, l01 - l015);
    CHECK(std::fabs((l03 - l12) - 12.04) < 1.0);
    CHECK(std::fabs(l01 - l015) < 0.5);
}

// A device running at 176.4 / 192 kHz (Windows' "Default Format" can be one) up to the 384 kHz limit:
// a far-lateral source still gets the full interaural delay, about 0.66 ms.
TEST(audio_spatial_itd_at_high_device_rates) {
    using namespace audio;
    ListenerPose lis;
    lis.pos = m::vec3(0, 1.2f, 0);
    lis.fwd = m::vec3(0, 0, -1);
    lis.up = m::vec3(0, 1, 0);
    const m::vec3 right(0.5f, 1.2f, 0.0f);
    // The chain alone at 384 kHz, dry and with no head shadow: an impulse reaches the far ear
    // exactly the ITD later (251.8 samples, between two samples).
    const SpatialParams sp = computeSpatial(makeBasis(lis), right, 384000.0f);
    SpatialTarget t;
    t.gL = t.gR = 1.0f;
    t.itd = sp.itd;
    SpatialChain chain;
    chain.begin(t, 512);
    double sum = 0.0, moment = 0.0;
    for (int i = 0; i < 512; ++i) {
        float l = 0.0f, r = 0.0f, room = 0.0f;
        chain.tick(i == 0 ? 1.0f : 0.0f, l, r, room);
        sum += l;
        moment += double(i) * l;
    }
    const double arrival = sum > 0.0 ? moment / sum : -1.0;
    std::fprintf(stderr, "  384 kHz: ITD %.2f samples, impulse at the far ear after %.2f samples\n", sp.itd, arrival);
    CHECK(sp.itd > 250.0f);
    CHECK(std::fabs(arrival - double(sp.itd)) < 0.01);
    // The whole mixer at 192 kHz: the left ear lags by ~126 samples (plus the far-ear shadow's delay).
    Mixer mx(3u);
    mx.prepare(192000.0f);
    mx.setVolumes(1.0f, 1.0f, 1.0f);
    mx.setAmbienceEnabled(false, true);
    mx.setRoomEnabled(false);
    mx.setListener(lis);
    for (int v = 0; v < bankVariants(Sfx::PiecePlace); ++v) {
        SoundBuffer* b = new SoundBuffer();
        b->samples = synthesize(Sfx::PiecePlace, 3u + uint32_t(v));
        b->sfx = int(Sfx::PiecePlace);
        b->variant = v;
        mx.install(b);
    }
    PlayRequest req;
    req.sfx = Sfx::PiecePlace;
    req.pos = right;
    CHECK(mx.play(req));
    std::vector<float> out(size_t(0.3f * 192000.0f) * 2);
    mx.process(out.data(), int(out.size() / 2));
    int bestLag = 0;
    double best = -1e30;
    for (int lag = -160; lag <= 160; ++lag) {
        double s = 0;
        for (size_t i = 200; i + 200 < out.size() / 2; ++i) s += double(out[2 * i + 1]) * out[2 * (size_t(long(i) + lag))];
        if (s > best) {
            best = s;
            bestLag = lag;
        }
    }
    std::fprintf(stderr, "  192 kHz mixer: left-ear lag %d samples\n", bestLag);
    CHECK(bestLag >= 120 && bestLag <= 140);
}

TEST(audio_repeated_triggers_differ) {
    using namespace audio;
    Mixer m(42u);
    m.prepare(kFs);
    m.setAmbienceEnabled(false, true);
    m.setRoomEnabled(false);
    for (int v = 0; v < kVariants; ++v) {
        SoundBuffer* b = new SoundBuffer();
        b->samples = synthesize(Sfx::PiecePlace, 100u + uint32_t(v));
        b->sfx = int(Sfx::PiecePlace);
        b->variant = v;
        m.install(b);
    }
    PlayRequest r;
    r.sfx = Sfx::PiecePlace;
    r.pos = m::vec3(0.0275f, 0.782f, 0.0275f);  // e4
    std::vector<std::vector<float>> takes;
    for (int k = 0; k < 4; ++k) {
        m.play(r);
        std::vector<float> out(size_t(0.4f * kFs) * 2);
        m.process(out.data(), int(out.size() / 2));
        takes.push_back(out);
    }
    for (size_t a = 0; a < takes.size(); ++a)
        for (size_t b = a + 1; b < takes.size(); ++b) {
            double dotp = 0, e1 = 0, e2 = 0;
            for (size_t i = 0; i < takes[a].size(); ++i) {
                dotp += double(takes[a][i]) * takes[b][i];
                e1 += double(takes[a][i]) * takes[a][i];
                e2 += double(takes[b][i]) * takes[b][i];
            }
            CHECK(dotp / std::sqrt(e1 * e2 + 1e-30) < 0.99);
        }
}

// Windowed playback (pen strokes): only 'duration' seconds sound, with click-free edges, and the
// voice is released at the end of the window.
TEST(audio_windowed_play) {
    using namespace audio;
    Mixer m(7u);
    m.prepare(kFs);
    m.setAmbienceEnabled(false, true);
    m.setRoomEnabled(false);
    for (int v = 0; v < bankVariants(Sfx::PenWrite); ++v) {
        SoundBuffer* b = new SoundBuffer();
        b->samples = synthesize(Sfx::PenWrite, 300u + uint32_t(v));
        b->sfx = int(Sfx::PenWrite);
        b->variant = v;
        m.install(b);
    }
    for (float dur : {0.05f, 0.18f, 0.6f}) {
        PlayRequest r;
        r.sfx = Sfx::PenWrite;
        r.pos = defaultPosition(Sfx::PenWrite);
        r.duration = dur;
        CHECK(m.play(r));
        std::vector<float> out(size_t(1.0f * kFs) * 2);
        m.process(out.data(), int(out.size() / 2));
        CHECK_EQ(m.activeVoices(), 0);
        size_t lastLoud = 0;
        float peak = 0.0f, first = std::fabs(out[0]) + std::fabs(out[1]);
        for (size_t i = 0; i < out.size() / 2; ++i) {
            float a = std::max(std::fabs(out[2 * i]), std::fabs(out[2 * i + 1]));
            peak = std::max(peak, a);
            if (a > 1e-5f) lastLoud = i;
        }
        float heard = float(lastLoud) / kFs;
        std::fprintf(stderr, "  pen stroke %.2f s: heard %.3f s, peak %.1f dBFS\n", dur, heard, 20.0f * std::log10(peak + 1e-12f));
        CHECK(peak > 1e-3f);
        CHECK(first < 0.02f * peak);                                    // faded in
        CHECK(heard > dur * 0.85f && heard < dur * 1.1f + 0.003f);    // pitch jitter +-4 %
    }
}

// A huge pitch (only finiteness is checked) moves the read position past INT_MAX after one sample:
// the voice must end there instead of reading silence until it is stolen.
TEST(audio_huge_pitch_voice_ends) {
    using namespace audio;
    Mixer m(7u);
    m.prepare(kFs);
    m.setAmbienceEnabled(false, true);
    for (int v = 0; v < kVariants; ++v) {
        SoundBuffer* b = new SoundBuffer();
        b->samples = synthesize(Sfx::PiecePlace, 500u + uint32_t(v));
        b->sfx = int(Sfx::PiecePlace);
        b->variant = v;
        m.install(b);
    }
    for (float pitch : {3e9f, 1e12f, 1e30f}) {
        PlayRequest r;
        r.sfx = Sfx::PiecePlace;
        r.pos = m::vec3(0.0f, 0.78f, 0.0f);
        r.pitch = pitch;
        CHECK(m.play(r));
        std::vector<float> out(size_t(0.1f * kFs) * 2);
        m.process(out.data(), int(out.size() / 2));
        CHECK_EQ(m.activeVoices(), 0);
        bool finite = true;
        for (float x : out) finite = finite && std::isfinite(x);
        CHECK(finite);
    }
}

TEST(audio_ambience_toggle_fades) {
    using namespace audio;
    Mixer m(7u);
    m.prepare(kFs);
    m.setVolumes(1, 1, 1);
    m.setAmbienceEnabled(true, true);
    std::vector<float> out(size_t(2.0f * kFs) * 2);
    m.process(out.data(), int(out.size() / 2));
    m.setAmbienceEnabled(false);
    std::vector<float> fade(size_t(4.0f * kFs) * 2);
    m.process(fade.data(), int(fade.size() / 2));
    auto rms = [](const std::vector<float>& b, size_t from, size_t to) {
        double e = 0;
        for (size_t i = from * 2; i < to * 2; ++i) e += double(b[i]) * b[i];
        return std::sqrt(e / double(2 * (to - from)));
    };
    double before = rms(out, size_t(1.5f * kFs), size_t(2.0f * kFs));
    double mid = rms(fade, size_t(0.6f * kFs), size_t(0.8f * kFs));
    double after = rms(fade, size_t(3.5f * kFs), size_t(4.0f * kFs));
    std::fprintf(stderr, "  ambience toggle: %.1f -> %.1f (0.7 s) -> %.1f dBFS (3.5 s)\n", db(float(before)), db(float(mid)),
                 db(float(after)));
    CHECK(mid < before && mid > before * 0.05);  // gradual, not a cut
    CHECK(db(float(after)) < db(float(before)) - 30.0f);
}

TEST(audio_queue_mpmc_stress) {
    audio::MpmcQueue<uint32_t, 256> q;
    std::atomic<bool> done{false};
    std::atomic<uint64_t> produced{0};
    const int kProducers = 4, kPerProducer = 20000;
    uint64_t consumedSum = 0, consumedCount = 0;
    std::thread consumer([&] {
        uint32_t v;
        for (;;) {
            if (q.pop(v)) {
                consumedSum += v;
                ++consumedCount;
            } else if (done.load()) {
                while (q.pop(v)) { consumedSum += v; ++consumedCount; }
                break;
            }
        }
    });
    std::vector<std::thread> ps;
    for (int p = 0; p < kProducers; ++p)
        ps.emplace_back([&, p] {
            for (int i = 0; i < kPerProducer; ++i) {
                uint32_t v = uint32_t(p * kPerProducer + i + 1);
                while (!q.push(v)) std::this_thread::yield();
                produced.fetch_add(v);
            }
        });
    for (auto& t : ps) t.join();
    done.store(true);
    consumer.join();
    CHECK_EQ(consumedCount, uint64_t(kProducers * kPerProducer));
    CHECK_EQ(consumedSum, produced.load());
}

TEST(audio_mixer_cpu_cost) {
    using namespace audio;
    Mixer m(9u);
    m.prepare(kFs);
    m.setVolumes(0.9f, 1.0f, 0.7f);
    m.setAmbienceEnabled(true, true);
    const Sfx game[] = {Sfx::PiecePickup, Sfx::PiecePlace, Sfx::ClockPress, Sfx::ServoShort, Sfx::Capture};
    for (Sfx s : game)
        for (int v = 0; v < kVariants; ++v) {
            SoundBuffer* b = new SoundBuffer();
            b->samples = synthesize(s, uint32_t(v) + 1u);
            b->sfx = int(s);
            b->variant = v;
            m.install(b);
        }
    for (int v = 0; v < kVariants; ++v) {
        SoundBuffer* b = new SoundBuffer();
        b->samples = synthesize(Sfx::ChairCreak, uint32_t(v) + 1u);
        b->sfx = int(Sfx::ChairCreak);
        b->variant = v;
        m.install(b);
    }
    // The coach talking all along (44.1 kHz phrases, resampled): 21 s in 2 s chunks, queued up front
    // (the content does not change the cost).
    {
        VoiceParams vp;
        vp.position = coachMouthDefault();
        vp.facing = m::vec3(0.0f, -0.5f, 0.87f);
        m.speechOpen(0, speechParams(vp));
        for (int c = 0; c < 11; ++c) {
            SoundBuffer* b = new SoundBuffer();
            b->samples.resize(88200);
            for (size_t i = 0; i < b->samples.size(); ++i)
                b->samples[i] = 0.1f * std::sin(0.0427f * float(i)) * std::sin(0.00031f * float(i));
            b->sfx = -1;
            m.speechAppend(0, b);
        }
        m.speechClose(0);
    }
    // Typical load: ambience + speech + a burst of game sounds every 0.5 s; plus a stress phase with
    // 32 voices (and the speech).
    const float seconds = 20.0f;
    const int block = 480;
    std::vector<float> out(size_t(block) * 2);
    int blocks = int(seconds * kFs) / block;
    double c0 = threadCpuSeconds();
    auto w0 = std::chrono::steady_clock::now();
    int maxVoices = 0;
    for (int b = 0; b < blocks; ++b) {
        if (b % 50 == 0) {
            PlayRequest r;
            r.sfx = game[(b / 50) % 5];
            r.pos = m::vec3(0.05f * float(b % 7), 0.78f, 0.1f);
            m.play(r);
        }
        m.process(out.data(), block);
        maxVoices = std::max(maxVoices, m.activeVoices());
    }
    double cpu = threadCpuSeconds() - c0;
    double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
    if (cpu <= 0.0) cpu = wall;
    float typical = float(cpu / seconds);
    // Stress: 32 simultaneous voices.
    for (int k = 0; k < kMaxVoices; ++k) {
        PlayRequest r;
        r.sfx = Sfx::Capture;
        r.pos = m::vec3(0.02f * float(k), 0.8f, 0.0f);
        m.play(r);
    }
    int stressBlocks = int(0.5f * kFs) / block;
    double s0 = threadCpuSeconds();
    auto sw0 = std::chrono::steady_clock::now();
    for (int b = 0; b < stressBlocks; ++b) m.process(out.data(), block);
    double scpu = threadCpuSeconds() - s0;
    if (scpu <= 0.0) scpu = std::chrono::duration<double>(std::chrono::steady_clock::now() - sw0).count();
    float stress = float(scpu / (stressBlocks * block / kFs));
    std::fprintf(stderr, "  mixer CPU: typical %.3f %% of one core (ambience + speech + game sounds, max %d voices), 32 voices %.2f %%\n",
                 typical * 100.0f, maxVoices, stress * 100.0f);
    CHECK_EQ(m.activeSpeech(), 1);  // the speech sounded through both phases
    CHECK(typical < 0.02f);
    CHECK(stress < 0.10f);
}

// WASAPI: a mix format the mixer cannot run at is handed to the engine as float32 stereo at a
// rate the mixer can run at (the engine converts), never at the unusable device rate.
TEST(audio_backend_fallback_rate) {
    using namespace audio;
    for (unsigned long rate : {8000ul, 44100ul, 48000ul, 192000ul, 384000ul}) CHECK_EQ(fallbackDeviceRate(rate), int(rate));
    for (unsigned long rate : {0ul, 4000ul, 7999ul, 384001ul, 705600ul, 768000ul}) CHECK_EQ(fallbackDeviceRate(rate), 48000);
}

// WASAPI reopen back-off: it grows over consecutive failures, a stream that played or a device that
// comes back restarts it, and a device that fails right after opening is not reopened in a tight loop.
TEST(audio_backend_reopen_policy) {
    using namespace audio;
    ReopenBackoff b;
    // No device at start-up: the back-off grows to 5 s.
    for (int ms : {250, 500, 1000, 2000, 4000, 5000, 5000}) CHECK_EQ(b.waitMs(false, StreamEnd::Stalled, false), ms);
    // The device came back and played, then stalled: 250 ms, not the 5 s left by the earlier failures.
    CHECK_EQ(b.waitMs(true, StreamEnd::Stalled, true), 250);
    CHECK_EQ(b.waitMs(true, StreamEnd::Stalled, false), 500);  // stalled again at once
    // A working device lost: at once, back-off reset.
    CHECK_EQ(b.waitMs(true, StreamEnd::Lost, true), 0);
    CHECK_EQ(b.failures, 0);
    // A device that opens but fails at once backs off like a failed open...
    for (int ms : {250, 500, 1000}) CHECK_EQ(b.waitMs(true, StreamEnd::Lost, false), ms);
    // ...while a default-device change reopens at once, even right after an open.
    CHECK_EQ(b.waitMs(true, StreamEnd::Changed, false), 0);
    CHECK_EQ(b.failures, 0);
    // No device for a while, then it comes back but flaps right after opening (a waking HDMI sink):
    // the back-off restarts at 250 ms instead of the 5 s left by the failed opens, then grows again.
    for (int ms : {250, 500, 1000, 2000, 4000, 5000, 5000}) CHECK_EQ(b.waitMs(false, StreamEnd::Stalled, false), ms);
    for (int ms : {250, 500, 1000}) CHECK_EQ(b.waitMs(true, StreamEnd::Lost, false), ms);
    // Failed opens go on with that back-off; a stream that stalls right after the device came back
    // restarts it too.
    for (int ms : {2000, 4000}) CHECK_EQ(b.waitMs(false, StreamEnd::Stalled, false), ms);
    CHECK_EQ(b.waitMs(true, StreamEnd::Stalled, false), 250);
}

// WASAPI underruns: every dry buffer counts (and raises the latency) once the first 4 audio events
// of the stream have passed, the first glitch included; a new stream has its own grace.
TEST(audio_backend_underrun_detection) {
    using namespace audio;
    // Padding (frames still queued in the device) seen by each audio event of a stream.
    auto underruns = [](std::initializer_list<unsigned> paddings) {
        UnderrunDetector d;
        int n = 0;
        for (unsigned p : paddings) n += d.onEvent(p) ? 1 : 0;
        return n;
    };
    CHECK_EQ(underruns({0, 0, 0, 0}), 0);                         // a stream that settles
    CHECK_EQ(underruns({480, 480, 480, 480, 480, 0, 480}), 1);    // the first glitch after it
    CHECK_EQ(underruns({480, 480, 480, 480, 0, 0, 480, 0}), 3);   // and every one after that
    CHECK_EQ(underruns({0, 480, 480, 480, 0}), 1);                // a dry start, then a glitch
}

TEST(audio_backend_choice) {
    using audio::BackendChoice;
    using audio::chooseBackend;
    CHECK(chooseBackend(nullptr, nullptr) == BackendChoice::Device);
    CHECK(chooseBackend("", "") == BackendChoice::Device);
    CHECK(chooseBackend("alsa", nullptr) == BackendChoice::Device);
    CHECK(chooseBackend("nul", nullptr) == BackendChoice::Device);
    CHECK(chooseBackend("nullx", nullptr) == BackendChoice::Device);
    CHECK(chooseBackend("null", nullptr) == BackendChoice::Null);
    CHECK(chooseBackend(nullptr, "/tmp/out.wav") == BackendChoice::Null);   // a dump is the null backend's
    CHECK(chooseBackend("alsa", "/tmp/out.wav") == BackendChoice::Null);
}

TEST(audio_backend_ahead_of_clock) {
    using audio::aheadOfClock;
    // A real device: the buffer filled at once, then the rate.
    CHECK(!aheadOfClock(960, 0.0, 48000, 960));
    CHECK(!aheadOfClock(48000 + 960, 1.0, 48000, 960));
    CHECK(!aheadOfClock(10 * 48000 + 960, 10.0, 48000, 960));
    // A sink that swallows everything at once.
    CHECK(aheadOfClock(48000, 0.001, 48000, 960));
    CHECK(aheadOfClock(12 * 48000, 10.0, 48000, 960));
}

#ifdef __linux__
namespace {
// A scripted ALSA device: 960-frame buffer, 240-frame periods, room for one period every 5 ms of
// wall time. Failures are injected at given write calls.
struct FakeAlsa {
    std::atomic<int> opens{0}, closes{0}, recovers{0}, writes{0};
    std::atomic<long> frames{0};
    std::atomic<bool> openFails{false};
    std::atomic<int> xrunAtWrite{-1}, lostAtWrite{-1};   // write call numbers
    std::atomic<bool> lost{false};                       // recover fails (the device is gone)
    int format = -1;
};
FakeAlsa* g_fake = nullptr;
audio::snd_pcm* const kPcm = reinterpret_cast<audio::snd_pcm*>(uintptr_t(0x1234));

audio::AlsaApi fakeApi() {
    audio::AlsaApi a{};
    a.open = [](audio::snd_pcm** pcm, const char*, int stream, int mode) {
        CHECK_EQ(stream, audio::alsa::kStreamPlayback);
        CHECK_EQ(mode, audio::alsa::kNonBlock);
        if (g_fake->openFails) return -ENOENT;
        ++g_fake->opens;
        g_fake->lost = false;
        *pcm = kPcm;
        return 0;
    };
    a.close = [](audio::snd_pcm*) { ++g_fake->closes; return 0; };
    a.setParams = [](audio::snd_pcm*, int format, int access, unsigned channels, unsigned rate, int, unsigned) {
        CHECK_EQ(access, audio::alsa::kAccessRwInterleaved);
        CHECK_EQ(channels, 2u);
        CHECK_EQ(rate, 48000u);
        g_fake->format = format;
        return format == audio::alsa::kFormatFloat ? 0 : -EINVAL;
    };
    a.getParams = [](audio::snd_pcm*, unsigned long* buffer, unsigned long* period) {
        *buffer = 960;
        *period = 240;
        return 0;
    };
    a.writei = [](audio::snd_pcm*, const void*, unsigned long n) -> long {
        const int k = g_fake->writes++;
        if (k == g_fake->xrunAtWrite) return -EPIPE;
        if (k == g_fake->lostAtWrite) {
            g_fake->lost = true;
            return -ENODEV;
        }
        g_fake->frames += long(n);
        return long(n);
    };
    a.recover = [](audio::snd_pcm*, int err, int) {
        ++g_fake->recovers;
        return g_fake->lost ? err : 0;
    };
    a.wait = [](audio::snd_pcm*, int) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        return 1;
    };
    a.availUpdate = [](audio::snd_pcm*) -> long { return 240; };
    a.drop = [](audio::snd_pcm*) { return 0; };
    a.strerror = [](int) { return "fake error"; };
    return a;
}

void countFrames(void* user, float* stereo, int frames, int) {
    static_cast<std::atomic<long>*>(user)->fetch_add(frames);
    for (int i = 0; i < 2 * frames; ++i) stereo[i] = 0.0f;
}
}  // namespace

// The ALSA backend against a scripted device: format choice, rendering at the device's pace, an
// xrun recovered in place (counted once the stream settled), a lost device closed and reopened
// after the back-off, a bounded stop, no render after it; a device that does not open gives none.
TEST(audio_alsa_backend_fake_device) {
    FakeAlsa fake;
    g_fake = &fake;
    const audio::AlsaApi api = fakeApi();
    fake.openFails = true;
    CHECK(audio::createAlsaBackend(api, "default") == nullptr);
    fake.openFails = false;
    fake.xrunAtWrite = 150;   // ~0.75 s in: counted
    fake.lostAtWrite = 250;
    std::unique_ptr<audio::Backend> b = audio::createAlsaBackend(api, "default");
    REQUIRE(b != nullptr);
    CHECK_EQ(fake.format, audio::alsa::kFormatFloat);
    CHECK_EQ(b->status.sampleRate.load(), 48000);
    CHECK_EQ(b->status.bufferFrames.load(), 960);
    std::atomic<long> rendered{0};
    CHECK(b->start(countFrames, &rendered));
    // 250 writes of 240 frames at 5 ms, the loss, 250 ms of back-off, the reopen.
    for (int i = 0; i < 400 && fake.opens.load() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    CHECK_EQ(fake.opens.load(), 2);
    CHECK_EQ(b->status.restarts.load(), 1u);
    CHECK_EQ(b->status.underruns.load(), 1u);
    CHECK(fake.recovers.load() >= 2);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    CHECK(b->status.deviceOpen.load());
    const auto t0 = std::chrono::steady_clock::now();
    b->stop();
    const double stopMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    CHECK(stopMs < 300.0);
    const long after = rendered.load();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    CHECK_EQ(rendered.load(), after);   // no render after stop()
    CHECK(fake.frames.load() > 0);
    CHECK(rendered.load() >= fake.frames.load());   // what was rendered is written (or still pending)
    b.reset();
    CHECK_EQ(fake.closes.load(), fake.opens.load());
    g_fake = nullptr;
}

// The real libasound with its "null" PCM (no sound card needed): it swallows data at once, so the
// backend must pace itself at the real rate.
TEST(audio_alsa_backend_null_pcm) {
    const audio::AlsaApi* api = audio::alsaApi();
    if (!api) SKIP("libasound.so.2 not installed");
    CHECK(audio::createAlsaBackend(*api, "scacelith_no_such_pcm") == nullptr);
    std::unique_ptr<audio::Backend> b = audio::createAlsaBackend(*api, "null");
    if (!b) SKIP("ALSA has no \"null\" PCM here");
    CHECK_EQ(b->status.sampleRate.load(), 48000);
    std::atomic<long> rendered{0};
    CHECK(b->start(countFrames, &rendered));
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    b->stop();
    const double seconds = double(rendered.load()) / 48000.0;
    std::fprintf(stderr, "  ALSA null PCM: %.3f s rendered in 0.5 s, buffer %d frames\n", seconds, b->status.bufferFrames.load());
    CHECK(seconds > 0.3 && seconds < 0.75);
}
#endif

TEST(audio_live_engine_init_shutdown) {
    audio::setMasterVolume(0.9f);
    audio::setAmbienceEnabled(true);
    bool ok = audio::init();
    audio::Stats st = audio::stats();
    std::fprintf(stderr, "  init() = %s, device %s, %d Hz, buffer %d frames\n", ok ? "true" : "false",
                 st.deviceOpen ? "open" : "none", st.sampleRate, st.bufferFrames);
    CHECK(st.running);
#ifndef _WIN32
    CHECK(ok);  // null backend always runs
#endif
    audio::setListener(m::vec3(0, 1.23f, 0.62f), m::vec3(0, -0.6f, -0.8f), m::vec3(0, 1, 0));
    std::this_thread::sleep_for(std::chrono::milliseconds(400));  // bank synthesis
    audio::debugTakeOutputPeak();
    for (int i = 0; i < 6; ++i) {
        audio::play(audio::Sfx::PiecePlace, m::vec3(0.02f * float(i), 0.78f, 0.0f), 1.0f, 1.0f);
        audio::playUI(audio::Sfx::UIClick);
        audio::play(audio::Sfx(99), m::vec3(0, 0, 0));                  // invalid: ignored
        audio::play(audio::Sfx::PiecePlace, m::vec3(NAN, 0, 0));        // invalid: ignored
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    float peak = audio::debugTakeOutputPeak();
    st = audio::stats();
    std::fprintf(stderr, "  live: output peak %.1f dBFS, cpu %.3f %%, voices %d, underruns %u\n", db(peak), st.cpuLoad * 100.0f,
                 st.activeVoices, st.underruns);
    std::fprintf(stderr, "  bank variants re-synthesised after playing: %u\n", audio::debugBankRefreshCount());
    if (ok) {
        CHECK(peak > 0.001f);
        CHECK(peak <= kMinus1dB);
        CHECK(audio::debugBankRefreshCount() > 0u);
    }
    audio::setAmbienceEnabled(false);
    audio::setEffectsVolume(0.5f);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    audio::shutdown();
    CHECK(!audio::stats().running);
    // Calls after shutdown are harmless; init/shutdown can be repeated.
    audio::play(audio::Sfx::PiecePlace, m::vec3(0, 0.78f, 0));
    bool ok2 = audio::init();
    CHECK_EQ(ok2, ok);
    audio::playUI(audio::Sfx::UIHover);
    audio::shutdown();
    audio::shutdown();
    audio::setAmbienceEnabled(true);
    audio::setEffectsVolume(1.0f);
}

// A 26 s "filmed scene" through the full chain: ambience, seated shifts, handshake, the game
// start tone, moves with pickups/places/servos/clock presses, a capture, game end, menu clicks.
TEST(audio_scene_demo_wav) {
    using namespace audio;
    Mixer m(2024u);
    m.prepare(kFs);
    m.setVolumes(0.9f, 1.0f, 0.7f);  // default settings
    m.setAmbienceEnabled(true, true);
    m.setListener(whiteSeatListener());
    installAllSounds(m, 5u);
    auto sq = [](int f, int r) { return m::vec3((float(f) - 3.5f) * 0.055f, 0.782f, (3.5f - float(r)) * 0.055f); };
    const m::vec3 clock(0.405f, 0.825f, 0.0f), ownArm(0.22f, 1.05f, 0.45f), oppArm(-0.2f, 1.1f, -0.5f);
    const m::vec3 oppChair(0.0f, 0.46f, -0.66f), ownChair(0.0f, 0.46f, 0.66f), hands(0.0f, 1.0f, 0.0f);
    struct Ev { float t; Sfx s; m::vec3 p; float gain, pitch; bool ui; };
    const Ev evs[] = {
        {0.8f, Sfx::ChairCreak, oppChair, 0.7f, 1.0f, false},
        {1.4f, Sfx::ServoShort, oppArm, 1.0f, 1.0f, false},
        {1.5f, Sfx::ServoShort, ownArm, 1.0f, 1.05f, false},
        {2.0f, Sfx::Handshake, hands, 1.0f, 1.0f, false},
        {3.6f, Sfx::GameStart, {}, 1.0f, 1.0f, true},
        {5.0f, Sfx::ServoShort, ownArm, 1.0f, 1.0f, false},
        {5.4f, Sfx::PiecePickup, sq(4, 1), 1.0f, 1.05f, false},   // e2 pawn
        {6.3f, Sfx::PiecePlace, sq(4, 3), 1.0f, 1.05f, false},    // e4
        {7.1f, Sfx::ClockPress, clock, 1.0f, 1.0f, false},
        {9.2f, Sfx::ServoShort, oppArm, 1.0f, 0.95f, false},
        {9.6f, Sfx::PiecePickup, sq(4, 6), 1.0f, 1.05f, false},   // e7
        {10.5f, Sfx::PiecePlace, sq(4, 4), 1.0f, 1.05f, false},   // e5
        {11.3f, Sfx::ClockPress, clock, 1.0f, 0.97f, false},
        {12.2f, Sfx::ChairCreak, ownChair, 0.5f, 1.1f, false},
        {13.4f, Sfx::PiecePickup, sq(5, 2), 1.0f, 1.0f, false},   // Nf3
        {14.2f, Sfx::CaptureClick, sq(4, 4), 1.0f, 1.0f, false},  // Nxe5: the e5 pawn is taken
        {15.1f, Sfx::TablePlace, m::vec3(-0.30f, 0.76f, 0.30f), 1.0f, 1.05f, false},
        {15.9f, Sfx::PiecePlace, sq(4, 4), 1.0f, 0.97f, false},
        {16.6f, Sfx::ClockPress, clock, 1.0f, 1.0f, false},
        {18.6f, Sfx::PiecePickup, sq(3, 6), 1.0f, 1.05f, false},  // d7
        {19.4f, Sfx::PiecePlace, sq(3, 5), 1.0f, 1.05f, false},   // d6
        {20.1f, Sfx::ClockPress, clock, 1.0f, 1.03f, false},
        {21.5f, Sfx::GameEnd, {}, 1.0f, 1.0f, true},
        {24.6f, Sfx::UIHover, {}, 1.0f, 1.0f, true},
        {25.0f, Sfx::UIClick, {}, 1.0f, 1.0f, true},
    };
    const float seconds = 26.0f;
    std::vector<float> out(size_t(seconds * kFs) * 2);
    size_t next = 0, frame = 0, total = out.size() / 2;
    while (frame < total) {
        while (next < sizeof(evs) / sizeof(evs[0]) && size_t(evs[next].t * kFs) <= frame) {
            PlayRequest r;
            r.sfx = evs[next].s;
            r.pos = evs[next].p;
            r.gain = evs[next].gain;
            r.pitch = evs[next].pitch;
            r.spatial = !evs[next].ui;
            r.bus = evs[next].ui ? Bus::UI : Bus::Effects;
            CHECK(m.play(r));
            ++next;
        }
        int n = int(std::min<size_t>(240, total - frame));
        m.process(out.data() + 2 * frame, n);
        frame += size_t(n);
    }
    Analysis a = analyzeStereo(out);
    std::fprintf(stderr, "  scene demo: peak %.2f dBFS, rms %.2f dBFS, limiter min gain %.2f dB\n", db(a.peak), db(a.rms),
                 db(m.takeLimiterMinGain()));
    CHECK(a.finite);
    CHECK_EQ(a.denormals, 0);
    CHECK(a.peak <= kMinus1dB);
    std::string path = outDir() + "/scene_demo_26s.wav";
    CHECK(writeWav16(path.c_str(), out.data(), out.size() / 2, 2, 48000));
}

// ---- Speech (coach voice) ------------------------------------------------------------------------
// The speech voices are tested offline on a Mixer (deterministic), then through the live engine
// (null backend on Linux). Test speech follows the TTS contract: mono 44.1 kHz, -20 dBFS RMS over
// the voiced frames, peak <= -1 dBFS, 10 ms fades.

namespace {

constexpr int kSrcRate = 44100;

// Speech-shaped signal: syllables of a glottal pulse train (Rosenberg-like flow derivative, with
// intonation) through three formant resonators, short gaps between syllables and longer ones
// between words; normalised like the TTS output.
std::vector<float> speechLike(float seconds, int rate, uint32_t seed) {
    using namespace audio::dsp;
    Rng rng(seed);
    const float fs = float(rate);
    const size_t n = size_t(seconds * fs);
    std::vector<float> x(n, 0.0f);
    Svf f1, f2, f3;
    float phase = 0.0f, prevGlot = 0.0f;
    size_t i = size_t(0.03f * fs);
    while (i < n) {
        const size_t len = size_t(rng.range(0.12f, 0.26f) * fs);
        const float F1 = rng.range(350.0f, 800.0f), F2 = rng.range(900.0f, 2200.0f), F3 = rng.range(2400.0f, 3100.0f);
        f1.set(F1, F1 / 90.0f, fs);
        f2.set(F2, F2 / 110.0f, fs);
        f3.set(F3, F3 / 160.0f, fs);
        const float f0a = rng.range(95.0f, 135.0f), f0b = f0a * rng.range(0.85f, 1.12f);
        for (size_t k = 0; k < len && i + k < n; ++k) {
            const float t = float(k) / float(len);
            const float env = 0.5f - 0.5f * std::cos(kTau * t);
            phase += (f0a + (f0b - f0a) * t) / fs;
            if (phase >= 1.0f) phase -= 1.0f;
            const float sp = std::sin(kPi * std::min(phase / 0.45f, 1.0f));
            const float glot = phase < 0.45f ? sp * sp : 0.0f;
            const float src = (glot - prevGlot) * 20.0f + 0.01f * rng.bi();
            prevGlot = glot;
            x[i + k] = (f1.bpNorm(src) + 0.5f * f2.bpNorm(src) + 0.25f * f3.bpNorm(src)) * env;
        }
        i += len + size_t((rng.chance(0.3f) ? rng.range(0.12f, 0.3f) : rng.range(0.02f, 0.06f)) * fs);
    }
    const size_t win = size_t(0.01f * fs);
    double mx = 0.0, sum = 0.0;
    size_t cnt = 0;
    std::vector<double> fr;
    for (size_t a = 0; a + win <= n; a += win) {
        double e = 0.0;
        for (size_t k = 0; k < win; ++k) e += double(x[a + k]) * x[a + k];
        fr.push_back(e / double(win));
        mx = std::max(mx, fr.back());
    }
    for (double e : fr)
        if (e > mx * 1e-4) { sum += e; ++cnt; }
    float g = 0.1f / float(std::sqrt(sum / double(std::max<size_t>(cnt, 1))));
    float peak = 0.0f;
    for (float& v : x) { v *= g; peak = std::max(peak, std::fabs(v)); }
    if (peak > kMinus1dB)
        for (float& v : x) v *= kMinus1dB / peak;
    const size_t fade = std::min(n / 2, size_t(0.01f * fs));
    for (size_t k = 0; k < fade; ++k) {
        const float w = 0.5f - 0.5f * std::cos(kPi * float(k) / float(fade));
        x[k] *= w;
        x[n - 1 - k] *= w;
    }
    return x;
}

// Voiced 10 ms frames of a source (within 40 dB of the loudest frame).
std::vector<bool> voicedFrames(const std::vector<float>& x, int rate) {
    const size_t win = size_t(rate / 100);
    std::vector<double> fr;
    double mx = 0.0;
    for (size_t a = 0; a + win <= x.size(); a += win) {
        double e = 0.0;
        for (size_t k = 0; k < win; ++k) e += double(x[a + k]) * x[a + k];
        fr.push_back(e / double(win));
        mx = std::max(mx, fr.back());
    }
    std::vector<bool> v(fr.size());
    for (size_t i = 0; i < fr.size(); ++i) v[i] = fr[i] > mx * 1e-4;
    return v;
}

// RMS (dBFS, both channels) of a 48 kHz stereo render over the 10 ms frames voiced in its source.
float activeLevelDb(const std::vector<float>& out, const std::vector<bool>& voiced) {
    double e = 0.0;
    size_t cnt = 0;
    for (size_t j = 0; j < voiced.size() && (j + 1) * 960 <= out.size(); ++j) {
        if (!voiced[j]) continue;
        for (size_t k = j * 960; k < (j + 1) * 960; ++k) e += double(out[k]) * out[k];
        cnt += 960;
    }
    return db(float(std::sqrt(e / double(std::max<size_t>(cnt, 1)))));
}

std::vector<float> sineWave(float hz, size_t n, int rate, float amp) {
    std::vector<float> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = amp * float(std::sin(6.283185307179586 * hz * double(i) / rate));
    return v;
}

audio::SoundBuffer* speechChunk(const std::vector<float>& v, size_t from, size_t to) {
    audio::SoundBuffer* b = new audio::SoundBuffer();
    b->samples.assign(v.begin() + long(from), v.begin() + long(std::min(to, v.size())));
    b->sfx = -1;
    return b;
}

void setupSpeechMixer(audio::Mixer& m, float fs, bool room, bool ambience) {
    m.prepare(fs);
    m.setVolumes(1.0f, 1.0f, 1.0f);
    m.setVoiceVolume(1.0f);
    m.setAmbienceEnabled(ambience, true);
    m.setRoomEnabled(room);
    m.setListener(audio::whiteSeatListener());
}

audio::SpeechParams coachParams(m::vec3 facing = m::vec3(0.0f)) {
    audio::VoiceParams vp;
    vp.position = audio::coachMouthDefault();
    vp.facing = facing;
    vp.sampleRate = kSrcRate;
    return audio::speechParams(vp);
}

// Renders 'frames' more frames (blocks of 'block') and appends them to 'out'.
void renderInto(audio::Mixer& m, std::vector<float>& out, size_t frames, int block = 480) {
    size_t at = out.size();
    out.resize(at + frames * 2);
    for (size_t done = 0; done < frames;) {
        int n = int(std::min<size_t>(size_t(block), frames - done));
        m.process(out.data() + at + 2 * done, n);
        done += size_t(n);
    }
}

// Frees the mixer's retired buffers; returns how many were speech chunks.
int freeRetired(audio::Mixer& m) {
    int n = 0;
    while (audio::SoundBuffer* b = m.peekRetired()) {
        n += b->sfx < 0 ? 1 : 0;
        delete b;
        m.dropRetired();
    }
    return n;
}

double channelEnergy(const std::vector<float>& b, size_t fromFrame, size_t toFrame, int ch) {
    double e = 0.0;
    for (size_t i = fromFrame; i < toFrame && 2 * i + 1 < b.size(); ++i) e += double(b[2 * i + size_t(ch)]) * b[2 * i + size_t(ch)];
    return e;
}

// RMS level (dBFS, both channels) of frames [fromFrame, toFrame).
float rangeDb(const std::vector<float>& b, size_t fromFrame, size_t toFrame) {
    const double e = channelEnergy(b, fromFrame, toFrame, 0) + channelEnergy(b, fromFrame, toFrame, 1);
    return db(float(std::sqrt(e / double(2 * std::max<size_t>(1, toFrame - fromFrame)))));
}

// Residual of a least-squares fit of a sine at 'hz' (plus DC) over frames [from, to) of one
// channel, relative to the fitted RMS: ~0 for a clean tone at exactly that frequency.
double sineFitResidual(const std::vector<float>& b, int ch, float hz, float fs, size_t from, size_t to) {
    double ss = 0, sc = 0, cc = 0, s1 = 0, c1 = 0, n = 0, ys = 0, yc = 0, y1 = 0;
    for (size_t i = from; i < to; ++i) {
        double t = 6.283185307179586 * hz * double(i) / fs, s = std::sin(t), c = std::cos(t), y = b[2 * i + size_t(ch)];
        ss += s * s; sc += s * c; cc += c * c; s1 += s; c1 += c; n += 1; ys += y * s; yc += y * c; y1 += y;
    }
    auto det3 = [](double a, double b_, double c, double d, double e, double f, double g, double h, double k) {
        return a * (e * k - f * h) - b_ * (d * k - f * g) + c * (d * h - e * g);
    };
    double D = det3(ss, sc, s1, sc, cc, c1, s1, c1, n);
    double A = det3(ys, sc, s1, yc, cc, c1, y1, c1, n) / D;
    double B = det3(ss, ys, s1, sc, yc, c1, s1, y1, n) / D;
    double C = det3(ss, sc, ys, sc, cc, yc, s1, c1, y1) / D;
    double e = 0;
    for (size_t i = from; i < to; ++i) {
        double t = 6.283185307179586 * hz * double(i) / fs;
        double r = b[2 * i + size_t(ch)] - (A * std::sin(t) + B * std::cos(t) + C);
        e += r * r;
    }
    return std::sqrt(e / n) / std::max(1e-12, std::sqrt(0.5 * (A * A + B * B)));
}

}  // namespace

// Speech plays at its own source rate: 1 s at 44.1 kHz lasts 1 s at 48 kHz and at 44.1 kHz, at the
// right pitch, cleanly interpolated (and untouched at 44.1 kHz: step 1).
TEST(audio_voice_resample_duration) {
    using namespace audio;
    const int N = kSrcRate;
    const std::vector<float> src = sineWave(440.0f, size_t(N), kSrcRate, 0.25f);
    for (float fs : {48000.0f, 44100.0f}) {
        Mixer m(3u);
        setupSpeechMixer(m, fs, false, false);
        CHECK(m.speechOpen(0, coachParams()));
        CHECK(m.speechAppend(0, speechChunk(src, 0, src.size())));
        m.speechClose(0);
        std::vector<float> out;
        size_t frames = 0;
        while (m.speechInfo(0).state != VoiceState::Finished && frames < size_t(3 * fs)) {
            renderInto(m, out, 1, 1);
            ++frames;
        }
        const size_t expect = size_t(std::ceil(double(N) * fs / kSrcRate)) + 1;  // + the frame that ends it
        const double res = sineFitResidual(out, 0, 440.0f, fs, size_t(0.1f * fs), size_t(0.9f * fs));
        std::fprintf(stderr, "  %.0f Hz: finished after %zu frames (expected %zu), played %lld / %d, 440 Hz fit residual %.2e\n", fs,
                     frames, expect, (long long)m.speechInfo(0).played, N, res);
        CHECK(frames + 3 >= expect && frames <= expect + 3);
        CHECK_EQ(m.speechInfo(0).played, int64_t(N));
        CHECK(res < (fs == 44100.0f ? 1e-5 : 1e-3));
        CHECK_EQ(m.activeSpeech(), 0);
        CHECK_EQ(freeRetired(m), 1);
    }
}

// Phrase joins: a stream split into chunks (odd lengths, 1- and 2-sample chunks included) renders
// exactly like the whole buffer; neither has clicks.
TEST(audio_voice_seamless_join) {
    using namespace audio;
    // Faded ends (the TTS contract): the last sample's timing may differ by one output frame between
    // the renders (the read position is accumulated from a different origin), which must not show.
    std::vector<float> tone = sineWave(300.0f, size_t(1.2f * kSrcRate), kSrcRate, 0.3f);
    for (size_t k = 0; k < 441; ++k) {
        const float w = 0.5f - 0.5f * std::cos(3.14159265f * float(k) / 441.0f);
        tone[k] *= w;
        tone[tone.size() - 1 - k] *= w;
    }
    const std::vector<float> talk = speechLike(3.0f, kSrcRate, 11u);
    const std::vector<float>* sources[] = {&tone, &talk};
    for (const std::vector<float>* src : sources) {
        std::vector<float> whole, split;
        const size_t frames = src->size() * 48000 / kSrcRate + 4800;
        {
            Mixer m(5u);
            setupSpeechMixer(m, kFs, true, false);
            m.speechOpen(0, coachParams());
            m.speechAppend(0, speechChunk(*src, 0, src->size()));
            m.speechClose(0);
            renderInto(m, whole, frames);
        }
        const size_t cuts[] = {0, 10007, 10008, 10010, 23333, 23334, src->size()};
        {
            Mixer m(5u);
            setupSpeechMixer(m, kFs, true, false);
            m.speechOpen(0, coachParams());
            for (size_t c = 0; c + 1 < sizeof(cuts) / sizeof(cuts[0]); ++c)
                CHECK(m.speechAppend(0, speechChunk(*src, cuts[c], cuts[c + 1])));
            m.speechClose(0);
            renderInto(m, split, frames);
            CHECK_EQ(m.speechInfo(0).state, VoiceState::Finished);
            CHECK_EQ(m.speechInfo(0).chunksDone, 6u);
            CHECK_EQ(freeRetired(m), 6);
        }
        float maxDiff = 0.0f;
        for (size_t i = 0; i < whole.size(); ++i) maxDiff = std::max(maxDiff, std::fabs(whole[i] - split[i]));
        const float cw = clickRatio(whole), cs = clickRatio(split);
        std::fprintf(stderr, "  %s: split vs whole max diff %.1e, click ratio %.3f (whole %.3f)\n",
                     src == &tone ? "tone" : "speech", maxDiff, cs, cw);
        CHECK(maxDiff < 1e-6f);
        CHECK(cs < (src == &tone ? 0.02f : 1.0f));
    }
}

// Between phrases the voice starves (silent, clock = everything queued) and resumes on the next
// append; a non-silent phrase end (worst case) is declicked.
TEST(audio_voice_starve_resume) {
    using namespace audio;
    for (int worst = 0; worst < 2; ++worst) {
        // worst: a tone cut mid-wave on both sides of the gap; otherwise TTS-like phrases.
        const std::vector<float> a = worst ? sineWave(300.0f, 13337, kSrcRate, 0.3f) : speechLike(0.6f, kSrcRate, 21u);
        const std::vector<float> b = worst ? sineWave(300.0f, 13337, kSrcRate, 0.3f) : speechLike(0.5f, kSrcRate, 22u);
        Mixer m(6u);
        setupSpeechMixer(m, kFs, false, false);
        m.speechOpen(0, coachParams());
        CHECK_EQ(m.speechInfo(0).state, VoiceState::Starved);  // open, nothing yet
        m.speechAppend(0, speechChunk(a, 0, a.size()));
        std::vector<float> out;
        renderInto(m, out, size_t(0.1f * kFs));
        CHECK_EQ(m.speechInfo(0).state, VoiceState::Playing);
        renderInto(m, out, size_t(0.9f * kFs));
        CHECK_EQ(m.speechInfo(0).state, VoiceState::Starved);
        CHECK_EQ(m.speechInfo(0).played, int64_t(a.size()));
        const size_t gapFrom = out.size() / 2 - size_t(0.2f * kFs), gapTo = out.size() / 2;
        const double gapE = channelEnergy(out, gapFrom, gapTo, 0) + channelEnergy(out, gapFrom, gapTo, 1);
        CHECK(gapE < 1e-12);  // silent while starved (dry render)
        m.speechAppend(0, speechChunk(b, 0, b.size()));
        renderInto(m, out, size_t(0.1f * kFs));
        CHECK_EQ(m.speechInfo(0).state, VoiceState::Playing);
        CHECK(m.speechInfo(0).played > int64_t(a.size()));
        m.speechClose(0);
        renderInto(m, out, size_t(0.8f * kFs));
        CHECK_EQ(m.speechInfo(0).state, VoiceState::Finished);
        CHECK_EQ(m.speechInfo(0).played, int64_t(a.size() + b.size()));
        CHECK_EQ(freeRetired(m), 2);
        const float clicks = clickRatio(out);
        std::fprintf(stderr, "  starve/resume (%s): click ratio %.3f\n", worst ? "tone cut mid-wave" : "speech phrases", clicks);
        CHECK(clicks < (worst ? 0.35f : 1.0f));
        CHECK(analyzeStereo(out).peak <= kMinus1dB);
    }
}

// The designed level: TTS-level speech at the coach's mouth, heard from White's seat.
TEST(audio_voice_level_from_coach) {
    using namespace audio;
    const std::vector<float> src = speechLike(4.0f, kSrcRate, 7u);
    const std::vector<bool> voiced = voicedFrames(src, kSrcRate);
    const ListenerPose lis = whiteSeatListener();
    const m::vec3 mouth = coachMouthDefault();
    const m::vec3 toPlayer = m::normalize(lis.pos - mouth);
    OfflineStats st;
    std::vector<float> wet = renderVoiceOffline(src, kSrcRate, 6.0f, mouth, lis, false, &st, toPlayer, true);
    std::vector<float> dry = renderVoiceOffline(src, kSrcRate, 6.0f, mouth, lis, false, nullptr, toPlayer, false);
    Analysis a = analyzeStereo(wet);
    const float level = activeLevelDb(wet, voiced), dryLevel = activeLevelDb(dry, voiced);
    const double eW = channelEnergy(wet, 0, wet.size() / 2, 0) + channelEnergy(wet, 0, wet.size() / 2, 1);
    const double eD = channelEnergy(dry, 0, dry.size() / 2, 0) + channelEnergy(dry, 0, dry.size() / 2, 1);
    const double drr = 10.0 * std::log10(eD / std::max(1e-30, eW - eD));
    const double lr = 10.0 * std::log10(channelEnergy(wet, 0, wet.size() / 2, 0) / channelEnergy(wet, 0, wet.size() / 2, 1));
    double se = 0.0;
    size_t sc = 0;
    for (size_t j = 0; j < voiced.size(); ++j)
        if (voiced[j]) {
            for (size_t k = j * 441; k < (j + 1) * 441; ++k) se += double(src[k]) * src[k];
            sc += 441;
        }
    const float srcLevel = db(float(std::sqrt(se / double(sc))));
    const Analysis place = analyzeStereo(renderSfxOffline(Sfx::PiecePlace, 1.5f, 1u));
    std::fprintf(stderr,
                 "  coach voice at White's seat: source %.1f dBFS RMS -> %.1f dBFS RMS (dry %.1f), peak %.1f dBFS, DRR %+.1f dB, "
                 "L/R %+.2f dB, limiter %.2f dB; piece_place peak %.1f dBFS\n",
                 srcLevel, level, dryLevel, db(a.peak), drr, lr, db(st.limiterMinGain), db(place.peak));
    CHECK(a.finite);
    CHECK_EQ(a.denormals, 0);
    CHECK(level > -29.0f && level < -23.0f);
    CHECK(a.peak <= kMinus1dB);
    CHECK(db(a.peak) > db(place.peak) - 6.0f && db(a.peak) < db(place.peak) + 8.0f);  // level with the board
    CHECK(st.limiterMinGain > 0.9f);
    CHECK(std::fabs(lr) < 0.5);
    CHECK(drr > 0.0 && drr < 6.0);
    CHECK(std::fabs(a.dcL) < 2e-4f && std::fabs(a.dcR) < 2e-4f);
    CHECK(writeWav16((outDir() + "/coach_voice.wav").c_str(), wet.data(), wet.size() / 2, 2, 48000));
}

// Talker directivity: facing the player = omnidirectional level; turned to the board a little
// softer; aside and away softer and darker.
TEST(audio_voice_directivity) {
    using namespace audio;
    const std::vector<float> src = speechLike(2.0f, kSrcRate, 9u);
    const ListenerPose lis = whiteSeatListener();
    const m::vec3 mouth = coachMouthDefault();
    const m::vec3 toPlayer = m::normalize(lis.pos - mouth);
    const m::vec3 toBoard = m::normalize(m::vec3(0.0f, 0.78f, 0.0f) - mouth);
    struct R {
        const char* name;
        m::vec3 facing;
        double level;
        float centroid;
    };
    R rs[] = {{"omni", m::vec3(0.0f), 0, 0}, {"player", toPlayer, 0, 0}, {"board", toBoard, 0, 0},
              {"aside", m::vec3(1, 0, 0), 0, 0}, {"away", toPlayer * -1.0f, 0, 0}};
    for (R& r : rs) {
        std::vector<float> out = renderVoiceOffline(src, kSrcRate, 2.2f, mouth, lis, false, nullptr, r.facing, false);
        r.level = 10.0 * std::log10(channelEnergy(out, 0, out.size() / 2, 0) + channelEnergy(out, 0, out.size() / 2, 1));
        std::vector<float> mono(out.size() / 2);
        for (size_t i = 0; i < mono.size(); ++i) mono[i] = 0.5f * (out[2 * i] + out[2 * i + 1]);
        r.centroid = spectralCentroid(mono);
        std::fprintf(stderr, "  facing %-6s: %+.2f dB, centroid %.0f Hz\n", r.name, r.level - rs[0].level, r.centroid);
    }
    CHECK(std::fabs(rs[1].level - rs[0].level) < 0.05);
    CHECK(rs[2].level < rs[1].level && rs[2].level > rs[1].level - 1.0);
    CHECK(rs[3].level < rs[1].level - 1.5 && rs[3].level > rs[1].level - 4.0);
    CHECK(rs[4].level < rs[1].level - 4.5);
    CHECK(rs[4].centroid < rs[1].centroid * 0.9f);
    CHECK(rs[3].centroid < rs[1].centroid);
}

// Ducking: the ambience drops ~6 dB under speech (attack 0.15 s), holds 0.5 s after it, then
// recovers (release 0.7 s). Effects are not ducked. A zero-gain voice isolates the duck: the
// renders with and without it differ only by the ducking.
TEST(audio_voice_ducks_ambience) {
    using namespace audio;
    const std::vector<float> talk = sineWave(200.0f, size_t(2.0f * kSrcRate), kSrcRate, 0.3f);
    auto render = [&](bool voice, bool ambience, std::vector<float>* duckCurve) {
        Mixer m(77u);
        setupSpeechMixer(m, kFs, true, ambience);
        installAllSounds(m, 3u);
        std::vector<float> out;
        renderInto(m, out, size_t(1.0f * kFs));
        if (voice) {
            SpeechParams p = coachParams();
            p.gain = 0.0f;
            m.speechOpen(0, p);
            m.speechAppend(0, speechChunk(talk, 0, talk.size()));
            m.speechClose(0);
        }
        for (int b = 0; b < int(6.0f * kFs) / 480; ++b) {
            if (b == 100 && !ambience) {  // a piece placed while the coach talks
                PlayRequest r;
                r.sfx = Sfx::PiecePlace;
                r.pos = defaultPosition(Sfx::PiecePlace);
                m.play(r);
            }
            renderInto(m, out, 480);
            if (duckCurve) duckCurve->push_back(m.duckGain());
        }
        return out;
    };
    std::vector<float> curve;
    const std::vector<float> ducked = render(true, true, &curve), plain = render(false, true, nullptr);
    auto ratioDb = [&](float t0, float t1) {
        size_t a = size_t(t0 * kFs), b = size_t(t1 * kFs);
        double e1 = channelEnergy(ducked, a, b, 0) + channelEnergy(ducked, a, b, 1);
        double e0 = channelEnergy(plain, a, b, 0) + channelEnergy(plain, a, b, 1);
        return 10.0 * std::log10(e1 / e0);
    };
    // The voice starts at 1.0 s and ends at ~3.0 s (render time); the curve has one value per 10 ms.
    const double before = ratioDb(0.5f, 1.0f), mid = ratioDb(1.8f, 2.9f), hold = ratioDb(3.1f, 3.45f), after = ratioDb(6.0f, 7.0f);
    const float at300 = curve[30];
    float lo = 1.0f;
    for (float g : curve) lo = std::min(lo, g);
    std::fprintf(stderr,
                 "  ducking: before %+.2f dB, speaking %+.2f dB, hold %+.2f dB, 3 s after %+.2f dB; gain after 0.3 s %.3f, min %.3f\n",
                 before, mid, hold, after, at300, lo);
    CHECK(std::fabs(before) < 0.01);
    CHECK(mid < -5.0 && mid > -7.0);
    CHECK(hold < -4.5);
    CHECK(after > -0.5);
    CHECK(at300 < 0.62f && at300 > 0.52f);
    CHECK(std::fabs(lo - 0.5012f) < 0.01f);
    // Effects: the same piece placement with and without speech, no ambience: identical.
    const std::vector<float> fxV = render(true, false, nullptr), fx0 = render(false, false, nullptr);
    float diff = 0.0f, peak = 0.0f;
    for (size_t i = 0; i < fxV.size(); ++i) {
        diff = std::max(diff, std::fabs(fxV[i] - fx0[i]));
        peak = std::max(peak, std::fabs(fx0[i]));
    }
    std::fprintf(stderr, "  effects under speech: max difference %.1e (piece peak %.1f dBFS)\n", diff, db(peak));
    CHECK(peak > 0.01f);
    CHECK(diff < 1e-7f);
}

// Pause holds the position with 15 ms fades; stop fades out (or cuts, declicked) and ends Stopped.
TEST(audio_voice_pause_resume_stop_fades) {
    using namespace audio;
    const std::vector<float> tone = sineWave(300.0f, size_t(3.0f * kSrcRate), kSrcRate, 0.3f);
    {
        Mixer m(8u);
        setupSpeechMixer(m, kFs, false, false);
        m.speechOpen(0, coachParams());
        m.speechAppend(0, speechChunk(tone, 0, tone.size()));
        m.speechClose(0);
        std::vector<float> out;
        renderInto(m, out, size_t(0.5f * kFs), 64);
        m.speechPause(0, true);
        CHECK_EQ(m.speechInfo(0).state, VoiceState::Paused);
        renderInto(m, out, size_t(0.05f * kFs), 64);
        const int64_t p1 = m.speechInfo(0).played;
        renderInto(m, out, size_t(0.3f * kFs), 64);
        const int64_t p2 = m.speechInfo(0).played;
        const size_t q0 = out.size() / 2 - size_t(0.25f * kFs), q1 = out.size() / 2;
        const float pausedDb = rangeDb(out, q0, q1);
        const double fadeSec = double(p1) / kSrcRate - 0.5;
        m.speechPause(0, false);
        renderInto(m, out, size_t(0.3f * kFs), 64);
        const int64_t p3 = m.speechInfo(0).played;
        CHECK_EQ(m.speechInfo(0).state, VoiceState::Playing);
        const float pauseClicks = clickRatio(out);
        std::fprintf(stderr,
                     "  pause: clock ran %.1f ms into the pause, frozen %s, paused output %.0f dBFS, resumed +%.3f s, click ratio %.3f\n",
                     fadeSec * 1000.0, p1 == p2 ? "yes" : "NO", pausedDb, double(p3 - p2) / kSrcRate, pauseClicks);
        CHECK(fadeSec > 0.010 && fadeSec < 0.020);
        CHECK_EQ(p1, p2);
        CHECK(pausedDb < -80.0f);  // silent but for the master DC blocker settling
        CHECK(p3 > p2 + int64_t(0.25f * kSrcRate));
        CHECK(pauseClicks < 0.02f);
        // Stop with a 60 ms fade.
        m.speechStop(0, 0.06f);
        renderInto(m, out, size_t(0.03f * kFs), 64);
        CHECK_EQ(m.speechInfo(0).state, VoiceState::Playing);  // still fading
        renderInto(m, out, size_t(0.04f * kFs), 64);
        CHECK_EQ(m.speechInfo(0).state, VoiceState::Stopped);
        const int64_t ps = m.speechInfo(0).played;
        renderInto(m, out, size_t(0.2f * kFs), 64);
        const size_t s0 = out.size() / 2 - size_t(0.15f * kFs), s1 = out.size() / 2;
        CHECK(rangeDb(out, s0, s1) < -80.0f);
        CHECK_EQ(m.speechInfo(0).played, ps);
        CHECK_EQ(m.activeSpeech(), 0);
        const float stopClicks = clickRatio(out);
        std::fprintf(stderr, "  stop (60 ms fade): click ratio %.3f\n", stopClicks);
        CHECK(stopClicks < 0.02f);
        CHECK_EQ(freeRetired(m), 1);
    }
    {  // Immediate stop mid-wave: declicked by the tail.
        Mixer m(8u);
        setupSpeechMixer(m, kFs, false, false);
        m.speechOpen(0, coachParams());
        m.speechAppend(0, speechChunk(tone, 0, tone.size()));
        std::vector<float> out;
        renderInto(m, out, size_t(0.3f * kFs) + 37, 64);
        m.speechStop(0, 0.0f);
        CHECK_EQ(m.speechInfo(0).state, VoiceState::Stopped);
        renderInto(m, out, size_t(0.1f * kFs), 64);
        const float clicks = clickRatio(out);
        std::fprintf(stderr, "  stop (immediate, mid-wave): click ratio %.3f\n", clicks);
        CHECK(clicks < 0.35f);
        CHECK_EQ(freeRetired(m), 1);
    }
}

// The speech pool is separate: 40 effects at once never steal or cut the coach.
TEST(audio_voice_not_stolen_by_effects) {
    using namespace audio;
    const std::vector<float> talk = speechLike(1.5f, kSrcRate, 31u);
    Mixer m(10u);
    setupSpeechMixer(m, kFs, true, false);
    installAllSounds(m, 4u);
    m.speechOpen(1, coachParams());
    m.speechAppend(1, speechChunk(talk, 0, talk.size()));
    m.speechClose(1);
    std::vector<float> out;
    renderInto(m, out, size_t(0.2f * kFs));
    for (int k = 0; k < 40; ++k) {
        PlayRequest r;
        r.sfx = Sfx::Capture;
        r.pos = m::vec3(0.01f * float(k), 0.8f, 0.0f);
        CHECK(m.play(r));
    }
    CHECK_EQ(m.activeVoices(), kMaxVoices);
    CHECK_EQ(m.activeSpeech(), 1);
    renderInto(m, out, size_t(2.0f * kFs));
    CHECK_EQ(m.speechInfo(1).state, VoiceState::Finished);
    CHECK_EQ(m.speechInfo(1).played, int64_t(talk.size()));
    CHECK(analyzeStereo(out).peak <= kMinus1dB);
}

// The effect pool is bounded (kMaxVoices); a play with every voice busy reuses the *oldest* one.
// That choice is safe, and this pins it: the pool must cut the oldest effect, not an arbitrary one,
// and never a speech voice -- which lives in its own pool and is what the coach talks through.
TEST(audio_voice_pool_full_reuses_the_oldest_effect) {
    using namespace audio;
    const m::vec3 right(0.5f, 1.23f, 0.62f), left(-0.5f, 1.23f, 0.62f);
    // One audible effect, hard right; the other 31 fill the pool silently (gain 0), so whatever the
    // pool cuts shows in the right channel alone. Every play happens before a frame is rendered, so
    // all the voices share one start clock and the tie-break makes voice 0 the oldest.
    auto fillPool = [&](Mixer& m, bool withCoach) {
        setupSpeechMixer(m, kFs, false, false);   // dry: no room or ambience to blur the measurement
        installAllSounds(m, 9u);
        if (withCoach) {
            // The coach, off to the left and far enough that its right-channel bleed is negligible.
            VoiceParams vp;
            vp.position = m::vec3(-6.0f, 1.2f, 0.0f);
            vp.facing = m::vec3(0.0f);
            vp.sampleRate = kSrcRate;
            const std::vector<float> talk = speechLike(3.0f, kSrcRate, 41u);
            m.speechOpen(0, speechParams(vp));
            m.speechAppend(0, speechChunk(talk, 0, talk.size()));
            m.speechClose(0);
        }
        PlayRequest first;
        first.sfx = Sfx::Capture;   // a bright attack, and the window below sits inside it
        first.pos = right;
        first.gain = 1.5f;
        CHECK(m.play(first));       // voice 0, the only audible one
        for (int k = 1; k < kMaxVoices; ++k) {
            PlayRequest r;
            r.sfx = Sfx::Capture;
            r.pos = left;
            r.gain = 0.0f;          // silent, but it still holds a voice
            CHECK(m.play(r));
        }
        CHECK_EQ(m.activeVoices(), kMaxVoices);
    };

    // Reference: the pool with nothing stolen. voice 0's attack is measured over [0, W).
    constexpr int W = 1920;   // 40 ms
    Mixer ref(61u);
    fillPool(ref, false);
    std::vector<float> refOut;
    renderInto(ref, refOut, W);
    const double rightRef = channelEnergy(refOut, 0, W, 1);
    CHECK(rightRef > 0.0);

    // Steal: the same, plus a 33rd play with every voice busy. The pool must reuse the oldest -- the
    // only audible voice -- so the right channel over the same [0, W) drops to the coach's bleed.
    Mixer m(61u);
    fillPool(m, true);
    PlayRequest extra;
    extra.sfx = Sfx::Capture;
    extra.pos = left;
    extra.gain = 0.0f;
    CHECK(m.play(extra));
    CHECK_EQ(m.activeVoices(), kMaxVoices);
    std::vector<float> out;
    renderInto(m, out, W);
    const double rightAfter = channelEnergy(out, 0, W, 1);
    std::fprintf(stderr, "  pool full: right channel %.4g with the pool intact, %.4g after the 33rd play\n", rightRef, rightAfter);

    CHECK(rightAfter < rightRef * 0.5);   // the oldest (right-panned) voice is the one that went

    // And the coach was never a candidate: 33 effect plays left the speech voice open and moving.
    CHECK_EQ(m.activeSpeech(), 1);
    CHECK_EQ(m.speechInfo(0).state, VoiceState::Playing);
    CHECK(m.speechInfo(0).played > 0);
}

// The ambience bird pool is kBirds (3) slots; a phrase that arrives with all three busy is
// skipped, never written over a sounding bird. Audited as safe; this pins the skip and the reuse.
TEST(audio_ambience_bird_pool_skips_when_full) {
    using namespace audio;
    Ambience amb;
    amb.prepare(kFs, 3u);
    CHECK_EQ(amb.activeBirds(), 0);

    // Fill the pool, one slot per window; every phrase is accepted into a free slot.
    for (int w = 0; w < 3; ++w) {
        CHECK(amb.startBirdForTest(w, 0));   // species 0: the shortest phrases
        CHECK_EQ(amb.birdWindow(w), w);
    }
    CHECK_EQ(amb.activeBirds(), 3);

    // A fourth phrase with every slot busy is skipped: no slot is taken, and the three sounding
    // birds keep the windows they had.
    CHECK(!amb.startBirdForTest(2, 0));
    CHECK_EQ(amb.activeBirds(), 3);
    CHECK_EQ(amb.birdWindow(0), 0);
    CHECK_EQ(amb.birdWindow(1), 1);
    CHECK_EQ(amb.birdWindow(2), 2);

    // Rendering lets the phrases end, and the freed slot is reused: the pool is not wedged.
    const Basis lis = makeBasis(ListenerPose{});
    bool freed = false;
    for (int step = 0; step < 300 && !freed; ++step) {   // up to 3 s, in 10 ms blocks
        float l[480] = {}, r[480] = {}, rm[480] = {};
        amb.process(l, r, rm, 480, lis, 1.0f);
        freed = amb.activeBirds() < 3;
    }
    CHECK(freed);
    CHECK(amb.startBirdForTest(0, 0));
}

// The emitter follows the pose: at the coach's right, then its left, with ramped gains.
TEST(audio_voice_follows_pose) {
    using namespace audio;
    const std::vector<float> tone = sineWave(300.0f, size_t(1.5f * kSrcRate), kSrcRate, 0.3f);
    Mixer m(12u);
    setupSpeechMixer(m, kFs, false, false);
    SpeechParams p = coachParams();
    p.pos = m::vec3(0.6f, 1.17f, -0.3f);
    m.speechOpen(0, p);
    m.speechAppend(0, speechChunk(tone, 0, tone.size()));
    m.speechClose(0);
    std::vector<float> out;
    renderInto(m, out, size_t(0.5f * kFs));
    m.speechPose(0, m::vec3(-0.6f, 1.17f, -0.3f), m::vec3(0.0f));
    renderInto(m, out, size_t(0.6f * kFs));
    const size_t a0 = size_t(0.1f * kFs), a1 = size_t(0.5f * kFs), b0 = size_t(0.6f * kFs), b1 = size_t(1.0f * kFs);
    const double first = 10.0 * std::log10(channelEnergy(out, a0, a1, 1) / channelEnergy(out, a0, a1, 0));
    const double second = 10.0 * std::log10(channelEnergy(out, b0, b1, 1) / channelEnergy(out, b0, b1, 0));
    const float clicks = clickRatio(out);
    std::fprintf(stderr, "  pose: R/L %+.1f dB at the right, %+.1f dB after moving left, click ratio %.3f\n", first, second, clicks);
    CHECK(first > 4.0);
    CHECK(second < -4.0);
    CHECK(clicks < 0.05f);
}

// Voice bus volume, chunk ownership (every chunk comes back through the retired list, whatever
// ends the voice) and refusals.
TEST(audio_voice_volume_and_chunk_ownership) {
    using namespace audio;
    const std::vector<float> tone = sineWave(300.0f, size_t(0.5f * kSrcRate), kSrcRate, 0.3f);
    auto energyAt = [&](float vol) {
        Mixer m(13u);
        setupSpeechMixer(m, kFs, false, false);
        m.setVoiceVolume(vol);
        m.speechOpen(0, coachParams());
        m.speechAppend(0, speechChunk(tone, 0, tone.size()));
        m.speechClose(0);
        std::vector<float> out;
        renderInto(m, out, size_t(0.6f * kFs));
        return channelEnergy(out, 0, out.size() / 2, 0) + channelEnergy(out, 0, out.size() / 2, 1);
    };
    const double e1 = energyAt(1.0f), eHalf = energyAt(0.5f), e0 = energyAt(0.0f), e2 = energyAt(2.0f), eBig = energyAt(9.0f);
    std::fprintf(stderr, "  voice volume 0.5: %+.2f dB, 2: %+.2f dB (9 clamps to 2: %+.2f dB), 0: %s\n", 10.0 * std::log10(eHalf / e1),
                 10.0 * std::log10(e2 / e1), 10.0 * std::log10(eBig / e1), e0 == 0.0 ? "silent" : "NOT SILENT");
    CHECK(std::fabs(10.0 * std::log10(eHalf / e1) + 6.02) < 0.05);
    CHECK(std::fabs(10.0 * std::log10(e2 / e1) - 6.02) < 0.05);
    CHECK(std::fabs(10.0 * std::log10(eBig / e2)) < 0.01);
    CHECK(e0 == 0.0);

    Mixer m(14u);
    setupSpeechMixer(m, kFs, false, false);
    std::vector<float> out;
    // Played to the end: 5 chunks back (+ 1 refused after the close).
    m.speechOpen(0, coachParams());
    for (int k = 0; k < 5; ++k) CHECK(m.speechAppend(0, speechChunk(tone, size_t(k) * 1000, size_t(k + 1) * 1000)));
    m.speechClose(0);
    CHECK(!m.speechAppend(0, speechChunk(tone, 0, 100)));
    renderInto(m, out, size_t(0.2f * kFs));
    CHECK_EQ(m.speechInfo(0).state, VoiceState::Finished);
    CHECK_EQ(freeRetired(m), 6);
    // Stopped with audio queued: every chunk back at once.
    m.speechOpen(1, coachParams());
    for (int k = 0; k < 7; ++k) m.speechAppend(1, speechChunk(tone, 0, tone.size()));
    renderInto(m, out, size_t(0.05f * kFs));
    m.speechStop(1, 0.0f);
    CHECK_EQ(m.speechInfo(1).state, VoiceState::Stopped);
    CHECK_EQ(m.speechInfo(1).chunksDone, 7u);
    CHECK_EQ(freeRetired(m), 7);
    // Reopened while talking: the old voice's chunks come back; the FIFO capacity is enforced.
    m.speechOpen(0, coachParams());
    for (int k = 0; k < 3; ++k) m.speechAppend(0, speechChunk(tone, 0, tone.size()));
    renderInto(m, out, size_t(0.05f * kFs));
    m.speechOpen(0, coachParams());
    CHECK_EQ(freeRetired(m), 3);
    int accepted = 0;
    for (int k = 0; k < kSpeechChunks + 5; ++k) accepted += m.speechAppend(0, speechChunk(tone, 0, 10)) ? 1 : 0;
    CHECK_EQ(accepted, kSpeechChunks);
    CHECK_EQ(freeRetired(m), 5);
    m.speechStop(0, 0.0f);
    CHECK_EQ(freeRetired(m), kSpeechChunks);
    // Invalid slots are refused without leaking.
    CHECK(!m.speechOpen(2, coachParams()));
    CHECK(!m.speechAppend(-1, speechChunk(tone, 0, 10)));
    CHECK_EQ(freeRetired(m), 1);
    renderInto(m, out, size_t(0.05f * kFs));
    CHECK(analyzeStereo(out).finite);
}

namespace {
// Polls voiceStatus() every 5 ms until 'done' holds or 'seconds' pass; returns the last status.
template <class Pred>
audio::VoiceStatus waitVoice(audio::VoiceId id, float seconds, Pred done, bool* sawPlaying = nullptr, int* maxSpeech = nullptr) {
    auto t0 = std::chrono::steady_clock::now();
    audio::VoiceStatus s = audio::voiceStatus(id);
    while (!done(s) && std::chrono::duration<float>(std::chrono::steady_clock::now() - t0).count() < seconds) {
        if (sawPlaying && s.state == audio::VoiceState::Playing) *sawPlaying = true;
        if (maxSpeech) *maxSpeech = std::max(*maxSpeech, audio::stats().activeSpeech);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        s = audio::voiceStatus(id);
    }
    return s;
}
bool isState(const audio::VoiceStatus& s, audio::VoiceState st) { return s.state == st; }
}  // namespace

// A coaching moment at the default settings, for listening and as a level regression: the coach
// speaks three phrases over the ambience (ducked), plays a demonstration move in the gap between
// two phrases (the voice starves meanwhile) and a capture lands in the middle of a sentence.
TEST(audio_voice_scene_demo_wav) {
    using namespace audio;
    Mixer m(2025u);
    m.prepare(kFs);
    m.setVolumes(0.9f, 1.0f, 0.7f);
    m.setVoiceVolume(1.0f);
    m.setAmbienceEnabled(true, true);
    m.setListener(whiteSeatListener());
    installAllSounds(m, 6u);
    auto sq = [](int f, int r) { return m::vec3((float(f) - 3.5f) * 0.055f, 0.782f, (3.5f - float(r)) * 0.055f); };
    const std::vector<float> p1 = speechLike(1.8f, kSrcRate, 61u), p2 = speechLike(2.2f, kSrcRate, 62u),
                             p3 = speechLike(1.5f, kSrcRate, 63u);
    const m::vec3 toPlayer = m::normalize(whiteSeatListener().pos - coachMouthDefault());
    const m::vec3 toBoard = m::normalize(m::vec3(0.0f, 0.78f, 0.0f) - coachMouthDefault());
    std::vector<float> out;
    renderInto(m, out, size_t(1.0f * kFs));
    m.speechOpen(0, coachParams(toPlayer));
    m.speechAppend(0, speechChunk(p1, 0, p1.size()));
    renderInto(m, out, size_t(2.2f * kFs));
    m.speechPose(0, coachMouthDefault(), toBoard);  // looks at the board for the demonstration
    PlayRequest r;
    r.sfx = Sfx::PiecePickup;
    r.pos = sq(6, 7);  // Ng8
    r.pitch = 0.98f;
    m.play(r);
    renderInto(m, out, size_t(0.8f * kFs));
    r.sfx = Sfx::PiecePlace;
    r.pos = sq(5, 5);  // -f6
    m.play(r);
    renderInto(m, out, size_t(0.4f * kFs));
    m.speechAppend(0, speechChunk(p2, 0, p2.size()));
    renderInto(m, out, size_t(1.0f * kFs));
    r.sfx = Sfx::CaptureClick;  // mid-sentence
    r.pos = sq(4, 4);
    r.pitch = 1.0f;
    m.play(r);
    renderInto(m, out, size_t(0.9f * kFs));
    r.sfx = Sfx::TablePlace;
    r.pos = m::vec3(-0.30f, 0.76f, -0.30f);
    m.play(r);
    renderInto(m, out, size_t(0.6f * kFs));
    m.speechPose(0, coachMouthDefault(), toPlayer);
    m.speechAppend(0, speechChunk(p3, 0, p3.size()));
    m.speechClose(0);
    renderInto(m, out, size_t(3.5f * kFs));
    CHECK_EQ(m.speechInfo(0).state, VoiceState::Finished);
    CHECK_EQ(m.speechInfo(0).played, int64_t(p1.size() + p2.size() + p3.size()));
    Analysis a = analyzeStereo(out);
    const float lim = m.takeLimiterMinGain();
    std::fprintf(stderr, "  coach scene: peak %.2f dBFS, rms %.2f dBFS, limiter min gain %.2f dB, ambience duck %.2f at the end\n",
                 db(a.peak), db(a.rms), db(lim), m.duckGain());
    CHECK(a.finite);
    CHECK_EQ(a.denormals, 0);
    CHECK(a.peak <= kMinus1dB);
    CHECK(lim > 0.9f);
    CHECK(writeWav16((outDir() + "/coach_scene.wav").c_str(), out.data(), out.size() / 2, 2, 48000));
}

// The live engine (null backend on Linux, real time): the public voice API end to end, the output
// WAV dump, and no leak whatever ends the voices.
TEST(audio_voice_live_engine) {
    using namespace audio;
    using namespace std::chrono_literals;
    auto finished = [](const VoiceStatus& s) { return isState(s, VoiceState::Finished); };
    VoiceParams vp;
    vp.position = coachMouthDefault();
    vp.facing = m::normalize(whiteSeatListener().pos - vp.position);
    vp.sampleRate = kSrcRate;
    // Before init(): nothing opens and refused audio stays with the caller.
    CHECK(!openVoice(vp));
    std::vector<float> keep = speechLike(0.2f, kSrcRate, 3u);
    const size_t keepN = keep.size();
    CHECK(appendVoice(VoiceId{0xfffffffeu}, std::move(keep)) < 0.0);
    CHECK_EQ(keep.size(), keepN);
    CHECK_EQ(voiceStatus(VoiceId{0xfffffffeu}).state, VoiceState::None);
    CHECK_EQ(voiceStatus(VoiceId{}).state, VoiceState::None);

#ifndef _WIN32
    const std::string dump = outDir() + "/live_voice_dump.wav";
    std::remove(dump.c_str());
    setenv("SCACELITH_AUDIO_DUMP", dump.c_str(), 1);
#endif
    setAmbienceEnabled(false);
    setVoiceVolume(0.0f);  // set before init(): applies from the first block
    const bool ok = init();
#ifndef _WIN32
    unsetenv("SCACELITH_AUDIO_DUMP");
#endif
    if (!ok) {  // Windows without a device: voices stay Pending; nothing else to check here
        shutdown();
        setAmbienceEnabled(true);
        setVoiceVolume(1.0f);
        return;
    }
    const ListenerPose lis = whiteSeatListener();
    setListener(lis.pos, lis.fwd, lis.up);
    std::this_thread::sleep_for(30ms);
    debugTakeOutputPeak();

    // Voice volume 0: the whole voice plays, silently.
    VoiceId v0 = playVoice(speechLike(0.3f, kSrcRate, 40u), vp);
    CHECK(bool(v0));
    CHECK_EQ(waitVoice(v0, 2.0f, finished).state, VoiceState::Finished);
    const float peak0 = debugTakeOutputPeak();
    setVoiceVolume(1.0f);
    std::this_thread::sleep_for(250ms);
    debugTakeOutputPeak();

    // A two-phrase utterance: phrase starts on the speech clock, states, activeSpeech, duration.
    std::vector<float> a = speechLike(0.5f, kSrcRate, 41u), b = speechLike(0.3f, kSrcRate, 42u);
    const size_t na = a.size(), nb = b.size();
    const double total = double(na + nb) / kSrcRate;
    VoiceId v1 = openVoice(vp);
    CHECK(bool(v1));
    const VoiceState s0 = voiceStatus(v1).state;
    CHECK(s0 == VoiceState::Pending || s0 == VoiceState::Starved);
    const auto t1 = std::chrono::steady_clock::now();
    CHECK_EQ(appendVoice(v1, std::move(a)), 0.0);
    CHECK(std::fabs(appendVoice(v1, std::move(b)) - double(na) / kSrcRate) < 1e-12);
    closeVoice(v1);
    CHECK(appendVoice(v1, std::vector<float>(100, 0.0f)) < 0.0);  // closed
    bool sawPlaying = false;
    int maxSpeech = 0;
    VoiceStatus st = waitVoice(v1, 3.0f, finished, &sawPlaying, &maxSpeech);
    const double took = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    const float peak1 = debugTakeOutputPeak();
    std::fprintf(stderr, "  live: %.3f s utterance finished after %.3f s, played %.3f s, peak %.1f dBFS (volume 0: %.1f dBFS)\n",
                 total, took, st.played, db(peak1), db(peak0));
    CHECK_EQ(st.state, VoiceState::Finished);
    CHECK(sawPlaying);
    CHECK_EQ(maxSpeech, 1);
    CHECK(st.closed);
    CHECK(std::fabs(st.played - total) < 1e-9 && std::fabs(st.queued - total) < 1e-9);
    CHECK(took > total - 0.05 && took < total + 0.4);
    CHECK(peak1 > 0.02f && peak1 <= kMinus1dB);
    CHECK(peak0 < 1e-4f);
    std::this_thread::sleep_for(30ms);
    CHECK_EQ(stats().activeSpeech, 0);

    // Starved between phrases, then resumed; the finished voice's slot is reused (v1 goes stale).
    VoiceId v2 = openVoice(vp);
    CHECK(bool(v2) && v2 != v1);
    CHECK_EQ(voiceStatus(v1).state, VoiceState::None);
    CHECK_EQ(appendVoice(v2, speechLike(0.15f, kSrcRate, 43u)), 0.0);
    st = waitVoice(v2, 1.0f, [](const VoiceStatus& s) { return isState(s, VoiceState::Starved) && s.played > 0.0; });
    CHECK_EQ(st.state, VoiceState::Starved);
    CHECK(std::fabs(st.played - st.queued) < 1e-9);
    CHECK(std::fabs(appendVoice(v2, speechLike(0.6f, kSrcRate, 44u)) - st.queued) < 1e-12);
    st = waitVoice(v2, 0.5f, [](const VoiceStatus& s) { return isState(s, VoiceState::Playing); });
    CHECK_EQ(st.state, VoiceState::Playing);
    std::this_thread::sleep_for(100ms);
    setVoicePaused(v2, true);
    std::this_thread::sleep_for(60ms);
    const VoiceStatus p1 = voiceStatus(v2);
    std::this_thread::sleep_for(100ms);
    const VoiceStatus p2 = voiceStatus(v2);
    CHECK_EQ(p1.state, VoiceState::Paused);
    CHECK_EQ(p1.played, p2.played);
    setVoicePaused(v2, false);
    closeVoice(v2);
    st = waitVoice(v2, 2.0f, finished);
    CHECK_EQ(st.state, VoiceState::Finished);
    CHECK(std::fabs(st.played - st.queued) < 1e-9);

    // Stopped mid-phrase and replaced at once: one successor takes the free slot, the next one the
    // slot being stopped (the stopped handle goes stale); a third open finds no slot.
    VoiceId v3 = playVoice(speechLike(2.0f, kSrcRate, 45u), vp);
    st = waitVoice(v3, 1.0f, [](const VoiceStatus& s) { return s.played > 0.2; });
    setVoicePose(v3, vp.position + m::vec3(0.1f, 0.0f, 0.0f), m::vec3(0.0f, 0.0f, 1.0f));
    stopVoice(v3, 0.06f);
    VoiceId v4 = openVoice(vp), v5 = openVoice(vp);
    CHECK(bool(v4) && bool(v5));
    CHECK_EQ(voiceStatus(v3).state, VoiceState::None);
    CHECK(!openVoice(vp));
    CHECK_EQ(appendVoice(v5, speechLike(0.2f, kSrcRate, 46u)), 0.0);
    closeVoice(v5);
    stopVoice(v4, 0.0f);
    CHECK_EQ(waitVoice(v5, 1.5f, finished).state, VoiceState::Finished);
    CHECK_EQ(waitVoice(v4, 0.5f, [](const VoiceStatus& s) { return isState(s, VoiceState::Stopped); }).state, VoiceState::Stopped);

    // Appends from another thread while this thread stops the voice.
    VoiceId v6 = openVoice(vp);
    std::atomic<int> accepted{0}, refused{0};
    std::thread producer([&] {
        for (int k = 0; k < 400 && refused.load() < 20; ++k) {
            if (appendVoice(v6, sineWave(200.0f, 441, kSrcRate, 0.1f)) < 0.0) ++refused;
            else ++accepted;
            std::this_thread::sleep_for(1ms);
        }
    });
    std::this_thread::sleep_for(80ms);
    stopVoice(v6, 0.02f);
    producer.join();
    st = waitVoice(v6, 0.5f, [](const VoiceStatus& s) { return isState(s, VoiceState::Stopped); });
    std::fprintf(stderr, "  live: second-thread appends: %d accepted before the stop, %d refused after\n", accepted.load(), refused.load());
    CHECK(accepted.load() > 0 && refused.load() > 0);
    CHECK_EQ(st.state, VoiceState::Stopped);
    CHECK(st.played < st.queued + 1e-9);

    // Shutdown with a voice talking: it ends Stopped, later calls refuse cleanly, nothing leaks.
    VoiceId v7 = openVoice(vp);
    CHECK(appendVoice(v7, speechLike(1.0f, kSrcRate, 47u)) == 0.0);
    std::this_thread::sleep_for(50ms);
    shutdown();
    CHECK_EQ(voiceStatus(v7).state, VoiceState::Stopped);
    std::vector<float> late = speechLike(0.1f, kSrcRate, 48u);
    const size_t lateN = late.size();
    CHECK(appendVoice(v7, std::move(late)) < 0.0);
    CHECK_EQ(late.size(), lateN);
    CHECK(!openVoice(vp));
    stopVoice(v7);
    setVoicePose(v7, vp.position);
    CHECK_EQ(debugSpeechChunksAlive(), 0);
    setAmbienceEnabled(true);

#ifndef _WIN32
    // The dump holds the session's output: a valid 48 kHz stereo WAV with the speech in it.
    FILE* f = std::fopen(dump.c_str(), "rb");
    CHECK(f != nullptr);
    if (f) {
        std::vector<uint8_t> bytes;
        uint8_t chunkBuf[65536];
        size_t n;
        while ((n = std::fread(chunkBuf, 1, sizeof(chunkBuf), f)) > 0) bytes.insert(bytes.end(), chunkBuf, chunkBuf + n);
        std::fclose(f);
        auto u32 = [&](size_t at) {
            return uint32_t(bytes[at]) | uint32_t(bytes[at + 1]) << 8 | uint32_t(bytes[at + 2]) << 16 | uint32_t(bytes[at + 3]) << 24;
        };
        CHECK(bytes.size() > 44 && std::memcmp(bytes.data(), "RIFF", 4) == 0 && std::memcmp(bytes.data() + 8, "WAVE", 4) == 0);
        if (bytes.size() > 44) {
            CHECK_EQ(size_t(u32(40)), bytes.size() - 44);
            CHECK_EQ(u32(24), 48000u);
            int peak = 0;
            for (size_t i = 44; i + 1 < bytes.size(); i += 2)
                peak = std::max(peak, std::abs(int(int16_t(uint16_t(bytes[i] | bytes[i + 1] << 8)))));
            const double secs = double(bytes.size() - 44) / (48000.0 * 4.0);
            std::fprintf(stderr, "  live: output dump %.2f s, peak %.1f dBFS (%s)\n", secs, db(float(peak) / 32768.0f), dump.c_str());
            CHECK(secs > 2.0);
            CHECK(peak > 600);
        }
    }
#endif
}

// A non-finite sample in the coach's PCM (a TTS numeric blow-up) is silenced on the way in: it
// must not latch NaN into the hall reverb and the DC blockers, which would mute every later sound.
TEST(audio_voice_non_finite_samples) {
    using namespace audio;
    using namespace std::chrono_literals;
    setAmbienceEnabled(false);
    if (!init()) {  // Windows without a device: nothing plays
        shutdown();
        setAmbienceEnabled(true);
        return;
    }
    const ListenerPose lis = whiteSeatListener();
    setListener(lis.pos, lis.fwd, lis.up);
    std::this_thread::sleep_for(400ms);  // bank synthesis
    VoiceParams vp;
    vp.position = coachMouthDefault();
    vp.sampleRate = kSrcRate;
    std::vector<float> pcm = speechLike(0.3f, kSrcRate, 50u);
    pcm[1000] = NAN;
    pcm[2000] = INFINITY;
    pcm[3000] = -INFINITY;
    VoiceId v = playVoice(std::move(pcm), vp);
    CHECK(bool(v));
    CHECK_EQ(waitVoice(v, 2.0f, [](const VoiceStatus& s) { return isState(s, VoiceState::Finished); }).state,
             VoiceState::Finished);
    std::this_thread::sleep_for(100ms);
    debugTakeOutputPeak();
    play(Sfx::PiecePlace, m::vec3(0.0f, 0.78f, 0.0f));
    std::this_thread::sleep_for(300ms);
    const float peak = debugTakeOutputPeak();  // NaN output would leave it at 0
    std::fprintf(stderr, "  live: piece placed after a non-finite voice: peak %.1f dBFS\n", db(peak));
    CHECK(std::isfinite(peak) && peak > 0.001f && peak <= kMinus1dB);
    shutdown();
    CHECK_EQ(debugSpeechChunksAlive(), 0);
    setAmbienceEnabled(true);
}
