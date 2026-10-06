#include "tts.h"
#include "core/log.h"
#include "model.h"
#include "model_store.h"
#include "text.h"
#include "threads.h"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <system_error>

namespace tts {
namespace {

constexpr int kDefaultVoice = 7;              // M3 in voice.bin (F1..F5, M1..M5), chosen by listening
constexpr int kSilenceSamples = kSampleRate * 3 / 10;   // 0.3 s between chunks (official helper)
constexpr float kMinChunkSeconds = 0.1f;      // shortest chunk (sherpa-onnx kMinDuration)
constexpr int64_t kMaxLatentFrames = 10000;   // longest chunk (sherpa-onnx kMaxLatentLen)
constexpr float kTargetRms = 0.1f;            // -20 dBFS
constexpr float kPeakLimit = 0.891251f;       // -1 dBFS
constexpr int kFadeSamples = kSampleRate / 100;         // 10 ms
constexpr size_t kLoudnessBlock = kSampleRate / 50;     // 20 ms

using Clock = std::chrono::steady_clock;
double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

// Seeded standard normal noise (splitmix64 uniforms, Box-Muller): the same seed gives the same
// noise on every run of a build.
class Gaussian {
public:
    explicit Gaussian(uint32_t seed) : state_(0x9E3779B97F4A7C15ull ^ seed) {}
    float next() {
        if (hasSpare_) {
            hasSpare_ = false;
            return spare_;
        }
        double u1 = (double(bits() >> 11) + 1.0) * (1.0 / 9007199254740992.0);   // (0, 1]
        double u2 = double(bits() >> 11) * (1.0 / 9007199254740992.0);           // [0, 1)
        double r = std::sqrt(-2.0 * std::log(u1));
        double a = 6.283185307179586 * u2;
        spare_ = float(r * std::sin(a));
        hasSpare_ = true;
        return float(r * std::cos(a));
    }

private:
    uint64_t bits() {
        uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    uint64_t state_;
    float spare_ = 0.0f;
    bool hasSpare_ = false;
};

uint32_t textSeed(const std::string& text, const std::string& lang, int voice) {
    uint32_t h = 2166136261u;
    auto mix = [&h](unsigned char c) {
        h ^= c;
        h *= 16777619u;
    };
    for (char c : lang) mix(static_cast<unsigned char>(c));
    mix(0);
    for (char c : text) mix(static_cast<unsigned char>(c));
    mix(static_cast<unsigned char>(voice));
    return h ? h : 1u;
}

}  // namespace

// Loudness: gain to about -20 dBFS RMS over the active part (20 ms blocks within 40 dB of the
// loudest block), capped so the peak stays at -1 dBFS; then 10 ms raised-cosine fades. A
// non-finite sample (a damaged model) becomes silence first: it would skip the measures and reach
// the mixer.
void finishPcm(std::vector<float>& pcm) {
    if (pcm.empty()) return;
    for (float& v : pcm)
        if (!std::isfinite(v)) v = 0.0f;
    std::vector<double> ms;
    double loudest = 0.0;
    for (size_t b = 0; b < pcm.size(); b += kLoudnessBlock) {
        size_t e = std::min(pcm.size(), b + kLoudnessBlock);
        double s = 0.0;
        for (size_t i = b; i < e; ++i) s += double(pcm[i]) * pcm[i];
        ms.push_back(s / double(e - b));
        loudest = std::max(loudest, ms.back());
    }
    double sum = 0.0;
    size_t n = 0;
    for (double m : ms)
        if (m >= loudest * 1e-4) {
            sum += m;
            ++n;
        }
    float peak = 0.0f;
    for (float v : pcm) peak = std::max(peak, std::fabs(v));
    if (n && sum > 0.0 && peak > 0.0f) {
        float gain = kTargetRms / float(std::sqrt(sum / double(n)));
        gain = std::min(gain, kPeakLimit / peak);
        for (float& v : pcm) v *= gain;
    }
    size_t fade = std::min<size_t>(kFadeSamples, pcm.size() / 2);
    for (size_t i = 0; i < fade; ++i) {
        float w = 0.5f * (1.0f - std::cos(3.14159265f * float(i) / float(fade)));
        pcm[i] *= w;
        pcm[pcm.size() - 1 - i] *= w;
    }
}

namespace {

std::string modelTag(const std::string& lang) {
    std::string l = lang.substr(0, lang.find_first_of("-_"));
    for (char& c : l) c = char(std::tolower(static_cast<unsigned char>(c)));
    if (text::modelLanguage(l)) return l;
    LOGW("tts: language '%s' not supported by the model, speaking English", lang.c_str());
    return "en";
}

}  // namespace

bool languageSupported(const std::string& uiCode) {
    for (const char* l : {"en", "fr", "de", "es", "ru", "uk", "ar", "ja"})
        if (uiCode == l) return true;
    return false;
}

int defaultVoice() { return kDefaultVoice; }

bool setArchCap(const char* arch) { return kern::setArchCap(arch); }

const char* activeArch() { return kern::active().name; }

// The model folder and its quick check belong to the model store (model_store.h).
void setModelDirectory(const std::string& dir) { setModelFolder(dir); }
std::string modelDirectory() { return modelFolder(); }
bool modelFilesPresent() { return modelStatus() == ModelStatus::Ready; }

// ------------------------------------------------------------------------------------------------
// Synthesizer
// ------------------------------------------------------------------------------------------------
Synthesizer::Synthesizer() = default;
Synthesizer::~Synthesizer() = default;

bool Synthesizer::loaded() const { return engine_ && engine_->loaded(); }
int Synthesizer::sampleRate() const { return kSampleRate; }
int Synthesizer::voiceCount() const { return loaded() ? engine_->voiceCount() : 0; }
std::string Synthesizer::voiceName(int i) const { return loaded() ? engine_->voiceName(i) : std::string(); }

bool Synthesizer::loadFrom(const std::string& dir, std::string* error) {
#ifdef SCACELITH_TTS_NO_KERNELS
    // This build has no compute kernels (the Android port: they are x86 SIMD, and the shared
    // bodies in src/tts/kernels_impl.h use SSE2 throughout). Loading a model would only produce
    // silence, so the load fails here and the coach speaks through its subtitles, as it does when
    // the model has not been downloaded yet.
    (void)dir;
    const std::string msg = "this build has no speech kernels";
    LOGW("tts: %s", msg.c_str());
    if (error) *error = msg;
    return false;
#else
    auto t0 = Clock::now();
    auto e = std::make_unique<Engine>();
    std::string err;
    if (!e->loadDirectory(dir, kern::active(), &err)) {
        if (error) *error = err;
        return false;
    }
    engine_ = std::move(e);
    LOGI("tts: models loaded from %s (%.0f MB, %s kernels at load, %.0f ms)", dir.c_str(),
         engine_->modelBytes() / 1048576.0, kern::active().name, since(t0) * 1000.0);
    return true;
#endif
}

bool Synthesizer::load(std::string* error) {
    std::string err;
    if (loadFrom(modelFolder(), &err)) return true;
    LOGW("tts: speech unavailable (%s)", err.c_str());
    if (error) *error = err;
    return false;
}

ThreadPool* Synthesizer::pool(int threads) {
    threads = std::max(1, std::min(threads, 16));
    if (threads == 1) return nullptr;
    if (!pool_ || poolThreads_ != threads) {
        pool_ = std::make_unique<ThreadPool>(threads);
        poolThreads_ = threads;
    }
    return pool_.get();
}

std::vector<float> Synthesizer::synthesize(const std::string& textIn, const std::string& lang, const Options& o,
                                           const std::atomic<bool>* cancel) {
    FpGuard fp;
    // The activations of this synthesis are reused through the buffer cache, then released.
    struct TrimAtEnd {
        ~TrimAtEnd() { trimBufferCache(); }
    } trim;
    auto t0 = Clock::now();
    stats_ = Stats();
    std::vector<float> out;
    if (!loaded()) return out;
    const Engine& eng = *engine_;
    std::string tag = modelTag(lang);
    int voice = o.voice >= 0 && o.voice < eng.voiceCount() ? o.voice
                                                            : (kDefaultVoice < eng.voiceCount() ? kDefaultVoice : 0);
    int steps = std::max(1, std::min(o.steps, 32));
    float speed = std::max(0.5f, std::min(o.speed, 2.0f));
    Gaussian rng(o.seed ? o.seed : textSeed(textIn, tag, voice));
    ExecContext ctx;
    ctx.k = &kern::active();
    ctx.pool = pool(o.threads);
    std::string err;
    auto cancelled = [cancel] { return cancel && cancel->load(std::memory_order_relaxed); };

    for (const std::string& piece : text::chunk(textIn, text::chunkLength(tag))) {
        if (cancelled()) return {};
        int dropped = 0;
        std::vector<int64_t> ids = text::indices(text::preprocess(piece, tag), eng.indexer(), &dropped);
        stats_.droppedCharacters += dropped;
        if (ids.empty()) continue;
        auto t = Clock::now();
        float seconds = 0.0f;
        if (!eng.duration(ids, voice, ctx, &seconds, &err, cancel)) break;
        stats_.duration += since(t);
        seconds = std::max(seconds / speed, kMinChunkSeconds);
        if (!std::isfinite(seconds)) {
            err = "duration predictor: non-finite duration";
            break;
        }
        int64_t wavLen = int64_t(std::min(double(seconds) * kSampleRate, double(kMaxLatentFrames) * kFrameSamples));
        int64_t frames = std::min<int64_t>((wavLen + kFrameSamples - 1) / kFrameSamples, kMaxLatentFrames);
        wavLen = std::min<int64_t>(wavLen, frames * kFrameSamples);
        Tensor noise = Tensor::alloc(DType::F32, {1, kLatentChannels, frames});
        for (int64_t i = 0; i < noise.count(); ++i) noise.mut<float>()[i] = rng.next();

        t = Clock::now();
        Tensor emb;
        if (!eng.encode(ids, voice, ctx, &emb, &err, cancel)) break;
        stats_.textEncoder += since(t);
        t = Clock::now();
        Tensor latent;
        if (!eng.denoise(emb, voice, noise, steps, ctx, &latent, cancel, &err)) break;
        stats_.vectorEstimator += since(t);
        t = Clock::now();
        Tensor wav;
        if (!eng.vocode(latent, ctx, &wav, &err, cancel)) break;
        stats_.vocoder += since(t);
        wavLen = std::min<int64_t>(wavLen, wav.count());
        if (!out.empty()) out.resize(out.size() + kSilenceSamples, 0.0f);
        out.insert(out.end(), wav.as<float>(), wav.as<float>() + wavLen);
        ++stats_.chunks;
    }
    if (!err.empty()) {
        if (!cancelled()) LOGE("tts: synthesis failed: %s", err.c_str());
        return {};
    }
    if (stats_.droppedCharacters) LOGD("tts: %d unsupported characters dropped", stats_.droppedCharacters);
    finishPcm(out);
    stats_.audioSeconds = double(out.size()) / kSampleRate;
    stats_.total = since(t0);
    return out;
}

// ------------------------------------------------------------------------------------------------
// Worker
// ------------------------------------------------------------------------------------------------
Worker::~Worker() { stop(); }

bool Worker::start(const Options& o) {
    stop();
    std::lock_guard<std::mutex> lock(mutex_);
    opts_ = o;
    quit_ = false;
    started_ = true;
    ready_ = false;
    failed_ = false;
    warmUpFailed_ = false;
    cancelRunning_ = false;
    try {
        thread_ = std::thread([this] { run(); });
    } catch (const std::system_error& e) {
        LOGE("tts: cannot start the speech thread (%s)", e.what());
        started_ = false;
        return false;
    }
    return true;
}

void Worker::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!started_) return;
        quit_ = true;
        queue_.clear();
        cancelRunning_ = true;
    }
    wake_.notify_all();
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> lock(mutex_);
    results_.clear();
    running_ = 0;
    started_ = false;
    ready_ = false;
}

uint32_t Worker::request(const std::string& text, const std::string& lang, int priority, uint32_t seed, float speed) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!started_ || quit_ || failed_) return 0;
    uint32_t id = nextId_++;
    if (nextId_ == 0) nextId_ = 1;
    queue_.push_back(Job{id, priority, order_++, seed, speed > 0.0f ? speed : 0.0f, text, lang});
    wake_.notify_all();
    return id;
}

bool Worker::done(uint32_t id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return results_.count(id) != 0;
}

bool Worker::take(uint32_t id, std::vector<float>& pcm) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = results_.find(id);
    if (it == results_.end()) return false;
    pcm = std::move(it->second);
    results_.erase(it);
    return true;
}

void Worker::cancel(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (id == 0) {
        queue_.clear();
        results_.clear();
        if (running_) cancelRunning_ = true;
        return;
    }
    queue_.erase(std::remove_if(queue_.begin(), queue_.end(), [id](const Job& j) { return j.id == id; }), queue_.end());
    results_.erase(id);
    if (running_ == id) cancelRunning_ = true;
}

size_t Worker::pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size() + (running_ ? 1 : 0);
}

void Worker::run() {
    lowerThreadPriority();
    Synthesizer synth;
    std::string err;
    bool ok = false;
    // An exception (std::bad_alloc, std::system_error) must not leave the thread: it would end the
    // game. The load then fails, a line ends with no samples (subtitles), as on any other error.
    try {
        ok = synth.load(&err);
        if (ok) {
            // Warm-up: pages the weights in and starts the thread pool before the first real line
            // (the buffer cache is trimmed after every line). Intact files always give it samples:
            // none (unless stop() cancelled it) means damaged files that still load.
            Options w = opts_;
            w.seed = 1;
            if (synth.synthesize("Hello.", "en", w, &cancelRunning_).empty() && !cancelRunning_.load()) {
                LOGE("tts: speech unavailable (the warm-up synthesis failed)");
                warmUpFailed_ = true;
                ok = false;
            }
        }
    } catch (const std::exception& e) {
        LOGE("tts: speech unavailable (%s)", e.what());
        ok = false;
    }
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!ok) {
            failed_ = true;
            for (const Job& j : queue_) results_[j.id] = std::vector<float>();   // done, no samples
            queue_.clear();
            return;
        }
        ready_ = true;
    }
    std::unique_lock<std::mutex> lock(mutex_);
    for (;;) {
        wake_.wait(lock, [this] { return quit_ || !queue_.empty(); });
        if (quit_) break;
        auto best = queue_.begin();
        for (auto it = queue_.begin(); it != queue_.end(); ++it)
            if (it->priority > best->priority || (it->priority == best->priority && it->order < best->order)) best = it;
        Job job = std::move(*best);
        queue_.erase(best);
        running_ = job.id;
        cancelRunning_ = false;
        lock.unlock();
        Options o = opts_;
        o.seed = job.seed;
        if (job.speed > 0.0f) o.speed = job.speed;
        std::vector<float> pcm;
        try {
            pcm = synth.synthesize(job.text, job.lang, o, &cancelRunning_);
        } catch (const std::exception& e) {
            LOGE("tts: synthesis failed: %s", e.what());
            pcm.clear();
        }
        lock.lock();
        if (!cancelRunning_ && !quit_) results_[job.id] = std::move(pcm);
        running_ = 0;
    }
}

}  // namespace tts
