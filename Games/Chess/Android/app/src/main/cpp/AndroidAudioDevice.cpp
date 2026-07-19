// Android audio backend: an AAudio low-latency output stream implementing the engine's
// IAudioDevice. AAudio is a pure NDK C API (API 26+, our minSdk) — no Java, no JNI. This is
// the audio twin of AndroidBleTransport.cpp / AndroidVulkanSurface.cpp: a per-OS seam file
// that defines Lur::Audio::CreateAudioDevice(), compiled only into the app.
//
// We request mono 16-bit @ 48 kHz on the LOW_LATENCY path and hand the callback straight to
// the mixer's RenderCallback. The buffer is sized to two bursts — the smallest that avoids
// underruns — so the software latency floor is a couple of bursts (~5-10 ms).
#include <aaudio/AAudio.h>
#include <cstdint>

#include "Lur/Audio/AudioDevice.h"

namespace Lur::Audio {
namespace {

class AAudioDevice : public IAudioDevice {
public:
    ~AAudioDevice() override { Stop(); }

    bool Start(RenderCallback Cb, void* User) override {
        if (Stream != nullptr) return true;      // already running
        Callback = Cb;
        UserPtr = User;

        AAudioStreamBuilder* Builder = nullptr;
        if (AAudio_createStreamBuilder(&Builder) != AAUDIO_OK) return false;
        AAudioStreamBuilder_setDirection(Builder, AAUDIO_DIRECTION_OUTPUT);
        AAudioStreamBuilder_setFormat(Builder, AAUDIO_FORMAT_PCM_I16);
        AAudioStreamBuilder_setChannelCount(Builder, 1);
        AAudioStreamBuilder_setSampleRate(Builder, 48000);
        AAudioStreamBuilder_setPerformanceMode(Builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
        AAudioStreamBuilder_setSharingMode(Builder, AAUDIO_SHARING_MODE_SHARED);
        AAudioStreamBuilder_setDataCallback(Builder, &AAudioDevice::DataCallback, this);

        const aaudio_result_t Opened = AAudioStreamBuilder_openStream(Builder, &Stream);
        AAudioStreamBuilder_delete(Builder);
        if (Opened != AAUDIO_OK || Stream == nullptr) { Stream = nullptr; return false; }

        Rate = AAudioStream_getSampleRate(Stream);
        const int32_t Burst = AAudioStream_getFramesPerBurst(Stream);
        if (Burst > 0) AAudioStream_setBufferSizeInFrames(Stream, Burst * 2);  // two bursts

        if (AAudioStream_requestStart(Stream) != AAUDIO_OK) { Stop(); return false; }
        return true;
    }

    void Stop() override {
        if (Stream == nullptr) return;
        AAudioStream_requestStop(Stream);
        AAudioStream_close(Stream);
        Stream = nullptr;
    }

    uint32_t OutputRate() const override { return Rate > 0 ? static_cast<uint32_t>(Rate) : 48000; }

private:
    static aaudio_data_callback_result_t DataCallback(
        AAudioStream* /*Stream*/, void* User, void* AudioData, int32_t NumFrames) {
        auto* Self = static_cast<AAudioDevice*>(User);
        auto* Out = static_cast<int16_t*>(AudioData);
        if (Self->Callback != nullptr) {
            Self->Callback(Self->UserPtr, Out, static_cast<uint32_t>(NumFrames));
        } else {
            for (int32_t i = 0; i < NumFrames; ++i) Out[i] = 0;   // silence until wired
        }
        return AAUDIO_CALLBACK_RESULT_CONTINUE;
    }

    AAudioStream* Stream = nullptr;
    RenderCallback Callback = nullptr;
    void* UserPtr = nullptr;
    int32_t Rate = 0;
};

} // namespace

IAudioDevice* CreateAudioDevice() { return new AAudioDevice(); }

} // namespace Lur::Audio
