// Android audio backend: AAudio (the NDK's C API over the platform's low-latency path, the
// successor of OpenSL ES). The counterpart of backend_wasapi.cpp (Windows) and backend_alsa.cpp
// (Linux).
//
// Shape: AAudio owns the audio thread and calls the mixer through its data callback, so this
// backend has no thread of its own -- start() opens and starts the stream, stop() stops and
// closes it (AAudioStream_close waits for the callback in flight to return). When no stream can
// be opened the caller (createBackend) falls back to the null backend, which keeps the mixer
// running at real-time pace so the game's audio state machine is never the one that stalls: the
// game plays its sounds, they just are not heard.
#if !defined(__ANDROID__)
// The desktop CMakeLists globs src/audio/*.cpp into scacelith_core, and this file is Android-only
// (the desktop builds get their audio from backend_alsa.cpp / backend_wasapi.cpp). Its body is
// therefore compiled for Android alone: on any other target this is an empty translation unit.
#else
#include "backend.h"
#include "../core/log.h"

#include <aaudio/AAudio.h>

#include <cstring>
#include <mutex>
#include <vector>

namespace audio {
namespace {

class AaudioBackend : public Backend {
public:
    // Opens the device (createBackend's probe) and starts the stream: its data callback is what
    // pulls audio, so there is no thread of ours to start.
    bool openDevice() {
        if (stream_) return true;
        AAudioStreamBuilder* builder = nullptr;
        if (AAudio_createStreamBuilder(&builder) != AAUDIO_OK || !builder) {
            LOGE("audio: AAudio_createStreamBuilder failed");
            return false;
        }
        AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
        // LOW_LATENCY: the mixer is a game mixer, not a media player, and the chess clock's lever
        // has to be heard when it is pressed, not 200 ms later.
        AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_FLOAT);
        AAudioStreamBuilder_setChannelCount(builder, 2);
        AAudioStreamBuilder_setSampleRate(builder, 48000);   // the device may pick another one
        AAudioStreamBuilder_setDataCallback(builder, &AaudioBackend::dataCallback, this);
        AAudioStreamBuilder_setErrorCallback(builder, &AaudioBackend::errorCallback, this);
        const aaudio_result_t opened = AAudioStreamBuilder_openStream(builder, &stream_);
        AAudioStreamBuilder_delete(builder);
        if (opened != AAUDIO_OK || !stream_) {
            LOGI("audio: no AAudio output stream (%s)", AAudio_convertResultToText(opened));
            stream_ = nullptr;
            return false;
        }
        rate_ = AAudioStream_getSampleRate(stream_);
        channels_ = AAudioStream_getChannelCount(stream_);
        if (rate_ < kMinDeviceRate || rate_ > kMaxDeviceRate) {
            LOGW("audio: unusual device rate %d Hz, resampling", rate_);
            rate_ = 48000;
        }
        if (channels_ < 1) channels_ = 2;
        if (channels_ != 2) scratch_.resize(size_t(2) * 4096);
        const aaudio_result_t started = AAudioStream_requestStart(stream_);
        if (started != AAUDIO_OK) {
            LOGE("audio: AAudioStream_requestStart failed (%s)", AAudio_convertResultToText(started));
            AAudioStream_close(stream_);
            stream_ = nullptr;
            return false;
        }
        status.sampleRate = rate_;
        status.bufferFrames = AAudioStream_getBufferSizeInFrames(stream_);
        status.deviceOpen = true;
        LOGI("audio: AAudio %d Hz, %d ch, %d frames", rate_, channels_, status.bufferFrames.load());
        return true;
    }

    bool start(RenderFn fn, void* user) override {
        fn_ = fn;
        user_ = user;
        return openDevice();
    }

    void stop() override {
        if (!stream_) return;
        AAudioStream_requestStop(stream_);
        AAudioStream_close(stream_);   // returns once the callback in flight has returned
        stream_ = nullptr;
        status.deviceOpen = false;
    }

private:
    static aaudio_data_callback_result_t dataCallback(AAudioStream*, void* user, void* audio, int32_t frames) {
        AaudioBackend* self = static_cast<AaudioBackend*>(user);
        if (!self->fn_ || frames <= 0) return AAUDIO_CALLBACK_RESULT_CONTINUE;
        float* out = static_cast<float*>(audio);
        if (self->channels_ == 2) {
            self->fn_(self->user_, out, frames, self->rate_);
        } else {
            // Mono (a headset in a call, an unusual device): mix stereo into it, taking the mean
            // so nothing clips that would not have clipped in stereo.
            const size_t n = size_t(frames);
            if (self->scratch_.size() < n * 2) self->scratch_.resize(n * 2);
            self->fn_(self->user_, self->scratch_.data(), frames, self->rate_);
            for (size_t i = 0; i < n; ++i) out[i] = 0.5f * (self->scratch_[2 * i] + self->scratch_[2 * i + 1]);
        }
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    static void errorCallback(AAudioStream*, void* user, aaudio_result_t error) {
        AaudioBackend* self = static_cast<AaudioBackend*>(user);
        ++self->status.underruns;
        LOGW("audio: AAudio error %s", AAudio_convertResultToText(error));
    }

    AAudioStream* stream_ = nullptr;
    RenderFn fn_ = nullptr;
    void* user_ = nullptr;
    int rate_ = 48000, channels_ = 2;
    std::vector<float> scratch_;
};

}  // namespace

std::unique_ptr<Backend> createAaudioBackend() { return std::unique_ptr<Backend>(new AaudioBackend()); }

std::unique_ptr<Backend> createBackend() {
    if (chooseBackend(std::getenv("SCACELITH_AUDIO"), std::getenv("SCACELITH_AUDIO_DUMP")) == BackendChoice::Device) {
        // The device is opened here, like the ALSA backend does, so a phone with no usable output
        // falls back to the null backend and the mixer keeps running at real-time pace.
        std::unique_ptr<AaudioBackend> candidate(new AaudioBackend());
        if (candidate->openDevice()) return candidate;
        LOGI("audio: no sound output, running silent");
    }
    return createNullBackend();
}

}  // namespace audio
#endif  // __ANDROID__
