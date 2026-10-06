// Real-time hall ambience (no loop, no music): room/air tone (pink + brown noise, slowly
// breathing level and colour), wind gusts on the three windows (band-passed noise following a
// gust process, faint whistle on strong gusts), birds outside heard through the glass
// (procedural chirps, trills, warbles and the odd dove, spatialised at the windows), and rare
// faint creaks somewhere in the hall (played by the mixer as distant ChairCreak voices).
#pragma once
#include "dsp.h"
#include "spatial.h"
#include <cstdint>

namespace audio {

class Ambience {
public:
    struct Event { m::vec3 pos; float gain, pitch; };

    void prepare(float sampleRate, uint32_t seed);
    // Adds the ambience (scaled by 'gain') into L/R and its hall send into 'room'.
    void process(float* L, float* R, float* room, int n, const Basis& lis, float gain);
    // True once when a distant creak should be triggered by the mixer.
    bool popCreak(Event& e);

    // Diagnostics for the tests. The bird pool is kBirds slots; a phrase that arrives with all of
    // them busy is skipped rather than written over a sounding bird, and these expose that state.
    int activeBirds() const;
    int birdWindow(int slot) const;                      // -1 when the slot is free
    bool startBirdForTest(int windowHint, int species);  // false when the phrase was skipped

private:
    static constexpr int kWindows = 3;
    static constexpr int kBirds = 3;
    static constexpr int kMaxSyl = 32;
    static constexpr int kGustHist = 256;  // control-rate history for per-window gust lag

    struct Syl { float start, dur, f0, f1, amp, vibRate, vibDepth, h2, h3, breath; };
    struct Bird {
        bool active = false;
        int window = 0;
        m::vec3 pos;
        Syl syl[kMaxSyl];
        int count = 0, cur = 0;
        float t = 0.0f, phase = 0.0f, amp = 1.0f;
        dsp::OnePole glass1, glass2;
        dsp::FastNoise noise;
        float gL = 0, gR = 0, lp = 0, lpZ = 0;
    };
    struct Window {
        m::vec3 pos;
        dsp::FastNoise noise;
        dsp::Brown brown;
        dsp::Svf bp, lp;
        int lag = 0;
        float fcOffset = 1.0f;
        float gL = 0, gR = 0, lp1 = 0, z = 0;
        float ampPrev = 0.0f;
    };

    void startPhrase(int windowHint, int species);
    void controlUpdate(const Basis& lis, float blockSec);

    float fs_ = 48000.0f;
    double t_ = 0.0;
    dsp::Rng rng_;
    // air tone
    dsp::FastNoise nL_, nR_, nC_;
    dsp::Pink pL_, pR_, pC_;
    dsp::Brown brown_;
    dsp::Svf lpL_, lpR_;
    dsp::OnePole hpL_, hpR_;
    dsp::SlowRandom airLvl_, airCut_, airHiss_;
    float airGain_ = 0, airGainPrev_ = 0, hissGain_ = 0;
    dsp::Svf hissL_, hissR_;
    // wind
    dsp::SlowRandom gustBase_, gustFast_, gustMood_;
    float gustHist_[kGustHist] = {};
    uint32_t gustW_ = 0;
    Window win_[kWindows];
    dsp::Svf whistle_;
    float whistleAmp_ = 0, whistleAmpPrev_ = 0;
    // birds
    Bird birds_[kBirds];
    double nextBird_ = 3.0;
    int repeatBird_ = -1, repeatSpecies_ = 0;
    double repeatAt_ = 0.0;
    // creaks
    double nextCreak_ = 40.0;
    bool creakPending_ = false;
    Event creak_{};
};

}  // namespace audio
