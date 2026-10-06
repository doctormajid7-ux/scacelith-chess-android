#include "ambience.h"
#include "../game/layout.h"
#include <cmath>

namespace audio {
using namespace dsp;

namespace {
// Levels at ambience gain 1 (calibrated with tests/audio_tests.cpp: whole ambience ~ -45 dBFS
// RMS with slow +-3 dB breathing, birds peaking around -28 dBFS).
constexpr float kLevel = 1.41f;  // overall ambience trim
constexpr float kAir = 0.0145f * kLevel;
constexpr float kBrownMix = 0.55f;
constexpr float kHiss = 0.00028f * kLevel;
constexpr float kWind = 0.020f * kLevel;
constexpr float kWhistle = 0.0035f * kLevel;
constexpr float kBird = 0.018f * kLevel;
constexpr float kWindSend = 0.5f;
constexpr float kBirdSend = 0.9f;

// Ambience emitters keep their designed level; only a mild distance law and the pan are used.
inline void emitterGains(const SpatialParams& sp, float& gL, float& gR) {
    float k = 7.0f / std::max(sp.dist, 3.0f) / std::max(sp.distGain, 1e-6f);
    gL = sp.gL * k;
    gR = sp.gR * k;
}
}  // namespace

void Ambience::prepare(float sampleRate, uint32_t seed) {
    fs_ = sampleRate;
    t_ = 0.0;
    rng_.reseed(seed, 777u);
    nL_ = FastNoise(rng_.next());
    nR_ = FastNoise(rng_.next());
    nC_ = FastNoise(rng_.next());
    pL_ = pR_ = pC_ = Pink();
    brown_ = Brown();
    brown_.leak = 1.0f - kTau * 18.0f / fs_;
    hpL_.setCutoff(22.0f, fs_);
    hpR_.setCutoff(22.0f, fs_);
    hpL_.reset();
    hpR_.reset();
    lpL_.reset();
    lpR_.reset();
    airLvl_.init(rng_, -2.5f, 2.5f, 6.0f, 18.0f);
    airCut_.init(rng_, 480.0f, 1100.0f, 8.0f, 20.0f);
    airHiss_.init(rng_, 0.5f, 1.0f, 10.0f, 25.0f);
    hissL_.set(2400.0f, 0.7f, fs_);
    hissR_.set(2500.0f, 0.7f, fs_);
    gustBase_.init(rng_, 0.05f, 0.45f, 8.0f, 20.0f);
    gustFast_.init(rng_, 0.0f, 1.0f, 1.2f, 4.0f);
    gustMood_.init(rng_, 0.35f, 1.0f, 30.0f, 90.0f);
    for (float& g : gustHist_) g = 0.0f;
    gustW_ = 0;
    const float midY = 0.5f * (layout::WINDOW_SILL_Y + layout::WINDOW_TOP_Y);
    for (int i = 0; i < kWindows; ++i) {
        Window& w = win_[i];
        w.pos = m::vec3(layout::HALL_MIN_X - 0.3f, midY, float(i - 1) * layout::WINDOW_SPACING);
        w.noise = FastNoise(rng_.next());
        w.brown = Brown();
        w.brown.leak = 1.0f - kTau * 40.0f / fs_;
        w.bp.reset();
        w.lp.reset();
        w.lp.set(1500.0f, 0.7f, fs_);
        w.lag = int(rng_.range(0.0f, 0.6f) * fs_ / 256.0f);
        w.fcOffset = rng_.range(0.85f, 1.15f);
        w.z = w.ampPrev = 0.0f;
    }
    whistle_.reset();
    whistle_.set(800.0f, 28.0f, fs_);
    for (Bird& b : birds_) b.active = false;
    nextBird_ = rng_.range(2.0f, 6.0f);
    repeatBird_ = -1;
    nextCreak_ = rng_.range(20.0f, 50.0f);
    creakPending_ = false;
    airGain_ = airGainPrev_ = 0.0f;
}

bool Ambience::popCreak(Event& e) {
    if (!creakPending_) return false;
    creakPending_ = false;
    e = creak_;
    return true;
}

int Ambience::activeBirds() const {
    int n = 0;
    for (const Bird& b : birds_) n += b.active ? 1 : 0;
    return n;
}

int Ambience::birdWindow(int slot) const {
    return slot >= 0 && slot < kBirds && birds_[slot].active ? birds_[slot].window : -1;
}

bool Ambience::startBirdForTest(int windowHint, int species) {
    const int before = activeBirds();
    startPhrase(windowHint, species);   // picks the first free slot, or returns when there is none
    return activeBirds() > before;
}

void Ambience::startPhrase(int windowHint, int species) {
    int slot = -1;
    for (int i = 0; i < kBirds; ++i)
        if (!birds_[i].active) { slot = i; break; }
    if (slot < 0) return;
    Rng& r = rng_;
    if (species < 0) {
        float u = r.uni();
        species = u < 0.34f ? 0 : u < 0.56f ? 1 : u < 0.8f ? 2 : u < 0.92f ? 4 : 3;
    }
    Bird& b = birds_[slot];
    b.window = windowHint >= 0 ? windowHint : r.below(kWindows);
    b.pos = win_[b.window].pos + m::vec3(0.0f, r.range(-1.2f, 1.5f), r.range(-1.0f, 1.0f));
    b.amp = r.logRange(0.25f, 1.0f);
    b.count = 0;
    float t = 0.0f;
    auto add = [&](float dur, float f0, float f1, float amp, float vibRate = 0.0f, float vibDepth = 0.0f, float h2 = 0.06f,
                   float h3 = 0.0f, float breath = 0.0f) {
        if (b.count >= kMaxSyl) return;
        b.syl[b.count++] = {t, dur, f0, f1, amp, vibRate, vibDepth, h2, h3, breath};
    };
    switch (species) {
        case 0: {  // sparrow-like chirps
            int n = 2 + r.below(6);
            float base = r.range(4200.0f, 6000.0f);
            for (int k = 0; k < n; ++k) {
                float f0 = r.jit(base, 0.08f), d = r.range(0.03f, 0.07f);
                add(d, f0, f0 * r.range(0.55f, 0.75f), r.range(0.7f, 1.0f));
                t += d + r.range(0.06f, 0.14f);
            }
        } break;
        case 1: {  // tit "tee-cher" motif
            int reps = 3 + r.below(3);
            float fa = r.range(5100.0f, 5700.0f), fb = r.range(3400.0f, 3800.0f);
            for (int k = 0; k < reps; ++k) {
                add(0.085f, fa, fa * 0.945f, 0.9f);
                t += 0.085f + 0.045f;
                add(0.075f, fb, fb * 0.92f, 1.0f);
                t += 0.075f + r.range(0.1f, 0.14f);
            }
        } break;
        case 2: {  // warbler/robin-like fluty phrase
            int n = 4 + r.below(5);
            for (int k = 0; k < n; ++k) {
                float d = r.range(0.08f, 0.22f), f0 = r.range(1800.0f, 3400.0f);
                add(d, f0, f0 * r.range(0.8f, 1.25f), r.range(0.6f, 1.0f), r.range(6.0f, 12.0f), r.range(0.015f, 0.04f), 0.04f);
                t += d + r.range(0.04f, 0.12f);
            }
        } break;
        case 3: {  // collared dove "coo-COO-coo" (low, passes the glass well: kept quieter)
            float f = r.range(470.0f, 560.0f);
            const float durs[3] = {0.22f, 0.42f, 0.28f}, amps[3] = {0.6f, 1.0f, 0.7f}, gaps[3] = {0.12f, 0.18f, 0.0f};
            for (int k = 0; k < 3; ++k) {
                add(durs[k] * r.range(0.9f, 1.1f), f, f * 0.94f, amps[k], 0.0f, 0.0f, 0.25f, 0.08f, 0.05f);
                t += durs[k] + gaps[k];
            }
            b.amp *= 0.5f;
        } break;
        default: {  // trill
            int n = 8 + r.below(11);
            float rate = r.range(12.0f, 18.0f), f0 = r.range(3000.0f, 4200.0f);
            for (int k = 0; k < n; ++k) {
                float d = r.range(0.022f, 0.03f);
                add(d, f0, f0 * 0.9f, std::pow(std::sin(kPi * (float(k) + 0.5f) / float(n)), 0.5f));
                t += 1.0f / rate;
            }
        } break;
    }
    b.cur = 0;
    b.t = 0.0f;
    b.phase = 0.0f;
    b.noise = FastNoise(r.next());
    b.glass1.setCutoff(r.range(2600.0f, 3400.0f), fs_);
    b.glass2.setCutoff(r.range(2600.0f, 3400.0f), fs_);
    b.glass1.reset();
    b.glass2.reset();
    b.lpZ = 0.0f;
    b.active = b.count > 0;
    if (b.active && repeatBird_ < 0 && r.chance(0.45f)) {
        repeatBird_ = slot;
        repeatSpecies_ = species;
        repeatAt_ = t_ + double(t) + double(r.range(1.2f, 4.0f));
    }
}

void Ambience::controlUpdate(const Basis& lis, float dt) {
    Rng& r = rng_;
    airGainPrev_ = airGain_;
    airGain_ = kAir * dbToGain(airLvl_.step(r, dt));
    float cut = airCut_.step(r, dt);
    lpL_.set(cut, 0.6f, fs_);
    lpR_.set(cut * 1.04f, 0.6f, fs_);
    hissGain_ = kHiss * airHiss_.step(r, dt);

    float mood = gustMood_.step(r, dt), base = gustBase_.step(r, dt), fast = gustFast_.step(r, dt);
    float I = mood * (base + 0.75f * fast * fast * fast);
    gustHist_[gustW_ & (kGustHist - 1)] = I;
    ++gustW_;
    for (Window& w : win_) {
        float Ii = gustHist_[(gustW_ - 1u - uint32_t(w.lag)) & (kGustHist - 1)];
        w.bp.set((150.0f + 380.0f * Ii) * w.fcOffset, 0.9f, fs_);
        SpatialParams sp = computeSpatial(lis, w.pos, fs_, false);
        emitterGains(sp, w.gL, w.gR);
        w.lp1 = sp.lp;
    }
    whistle_.set(650.0f + 350.0f * I, 28.0f, fs_);
    whistleAmpPrev_ = whistleAmp_;
    float ws = clampf((I - 0.6f) / 0.4f, 0.0f, 1.0f);
    whistleAmp_ = kWhistle * ws * ws;

    if (t_ >= nextBird_) {
        startPhrase(-1, -1);
        nextBird_ = t_ + double(clampf(r.expo(7.0f), 1.5f, 30.0f));
    }
    if (repeatBird_ >= 0 && t_ >= repeatAt_) {
        if (!birds_[repeatBird_].active) startPhrase(birds_[repeatBird_].window, repeatSpecies_);
        repeatBird_ = -1;
    }
    for (Bird& b : birds_) {
        if (!b.active) continue;
        SpatialParams sp = computeSpatial(lis, b.pos, fs_, false);
        emitterGains(sp, b.gL, b.gR);
        b.lp = sp.lp;
    }
    if (t_ >= nextCreak_) {
        creakPending_ = true;
        float side = r.chance(0.5f) ? 1.0f : -1.0f;
        creak_.pos = m::vec3(r.range(-6.0f, 6.0f), r.range(0.3f, 3.0f), side * r.range(4.0f, 10.0f));
        creak_.gain = r.range(0.3f, 0.6f);
        creak_.pitch = r.range(0.55f, 0.85f);
        nextCreak_ = t_ + double(r.range(25.0f, 75.0f));
    }
}

void Ambience::process(float* L, float* R, float* room, int n, const Basis& lis, float gain) {
    if (n <= 0) return;
    float dt = float(n) / fs_;
    // Per-window target amplitudes for this block (ramped per sample).
    float windAmp[kWindows], windPrev[kWindows];
    controlUpdate(lis, dt);
    for (int i = 0; i < kWindows; ++i) {
        Window& w = win_[i];
        float Ii = gustHist_[(gustW_ - 1u - uint32_t(w.lag)) & (kGustHist - 1)];
        windPrev[i] = w.ampPrev;
        windAmp[i] = kWind * std::pow(Ii, 1.6f);
        w.ampPrev = windAmp[i];
    }
    t_ += double(dt);
    const float invN = 1.0f / float(n);
    const float birdStep = 1.0f / fs_;
    for (int k = 0; k < n; ++k) {
        float a = float(k + 1) * invN;
        // Room / air tone.
        float ga = (airGainPrev_ + (airGain_ - airGainPrev_) * a) * gain;
        float c = pC_.tick(nC_.next());
        float br = brown_.tick(nC_.next());
        float wl = nL_.next(), wr = nR_.next();
        float l = 0.75f * pL_.tick(wl) + 0.66f * c;
        float r = 0.75f * pR_.tick(wr) + 0.66f * c;
        l = hpL_.hp(lpL_.lp(l + br * kBrownMix));
        r = hpR_.hp(lpR_.lp(r + br * kBrownMix));
        float hl = hissL_.bpNorm(wl) * hissGain_ * gain, hr = hissR_.bpNorm(wr) * hissGain_ * gain;
        float outL = l * ga + hl, outR = r * ga + hr;
        float send = 0.0f;
        // Wind on the windows.
        for (int i = 0; i < kWindows; ++i) {
            Window& w = win_[i];
            float x = w.noise.next();
            float v = w.bp.bpNorm(0.6f * x + w.brown.tick(x));
            v = w.lp.lp(v) * (windPrev[i] + (windAmp[i] - windPrev[i]) * a) * gain;
            w.z = v + w.lp1 * (w.z - v);
            outL += w.z * w.gL;
            outR += w.z * w.gR;
            send += w.z;
            if (i == 1) {
                float wh = whistle_.bpNorm(x) * (whistleAmpPrev_ + (whistleAmp_ - whistleAmpPrev_) * a) * gain;
                outL += wh * w.gL;
                outR += wh * w.gR;
                send += wh;
            }
        }
        send *= kWindSend;
        // Birds outside, through the glass.
        for (Bird& b : birds_) {
            if (!b.active) continue;
            b.t += birdStep;
            const Syl* s = &b.syl[b.cur];
            if (b.t >= s->start + s->dur) {
                if (++b.cur >= b.count) {
                    b.active = false;
                    continue;
                }
                s = &b.syl[b.cur];
                b.phase = 0.0f;
            }
            float v = 0.0f;
            if (b.t >= s->start) {
                float x = (b.t - s->start) / s->dur;
                float f = s->f0 + (s->f1 - s->f0) * x;
                if (s->vibDepth > 0.0f) f *= 1.0f + s->vibDepth * fastSin(kTau * s->vibRate * (b.t - s->start));
                b.phase += kTau * f / fs_;
                if (b.phase > kTau) b.phase -= kTau;
                // Syllable envelope: fast attack, longer decay (peak at ~35 %).
                float e = std::max(0.0f, fastSin(kPi * x * (1.6f - 0.6f * x)));
                e = e * std::sqrt(e);
                v = fastSin(b.phase) + s->h2 * fastSin(2.0f * b.phase);
                if (s->h3 > 0.0f) v += s->h3 * fastSin(3.0f * b.phase);
                if (s->breath > 0.0f) v += s->breath * b.noise.next();
                v *= e * s->amp;
            }
            v = b.glass2.lp(b.glass1.lp(v)) * b.amp * kBird * gain;
            b.lpZ = v + b.lp * (b.lpZ - v);
            outL += b.lpZ * b.gL;
            outR += b.lpZ * b.gR;
            send += b.lpZ * kBirdSend;
        }
        L[k] += outL;
        R[k] += outR;
        room[k] += send;
    }
}

}  // namespace audio
