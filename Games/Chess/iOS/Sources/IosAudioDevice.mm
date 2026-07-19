// iOS audio backend: a RemoteIO Audio Unit output implementing the engine's IAudioDevice.
// AudioToolbox's RemoteIO is Apple's lowest-latency callback path (the twin of MoltenVK for
// audio) — a per-OS seam file, like IosBleTransport.mm, that defines CreateAudioDevice().
//
// The mixer produces MONO 16-bit @ 48 kHz; iOS hardware output is interleaved stereo, so the
// render callback renders one mono buffer and duplicates it into L/R. An AVAudioSession set
// to Playback with a short IO buffer keeps the latency down.
#import <AudioToolbox/AudioToolbox.h>
#import <AVFAudio/AVFAudio.h>
#include <cstdint>
#include <cstring>

#include "Lur/Audio/AudioDevice.h"

namespace Lur::Audio {
namespace {

class RemoteIoDevice : public IAudioDevice {
public:
    ~RemoteIoDevice() override { Stop(); }

    bool Start(RenderCallback Cb, void* User) override {
        if (Unit != nullptr) return true;
        Callback = Cb;
        UserPtr = User;

        // Session: playback, 48 kHz, small IO buffer for low latency.
        AVAudioSession* Session = [AVAudioSession sharedInstance];
        [Session setCategory:AVAudioSessionCategoryPlayback error:nil];
        [Session setPreferredSampleRate:48000 error:nil];
        [Session setPreferredIOBufferDuration:0.005 error:nil];
        [Session setActive:YES error:nil];

        AudioComponentDescription Desc = {};
        Desc.componentType = kAudioUnitType_Output;
        Desc.componentSubType = kAudioUnitSubType_RemoteIO;
        Desc.componentManufacturer = kAudioUnitManufacturer_Apple;
        AudioComponent Comp = AudioComponentFindNext(nullptr, &Desc);
        if (Comp == nullptr || AudioComponentInstanceNew(Comp, &Unit) != noErr) { Unit = nullptr; return false; }

        // Interleaved stereo 16-bit PCM at 48 kHz on element 0's input scope (our data).
        AudioStreamBasicDescription Fmt = {};
        Fmt.mSampleRate = 48000;
        Fmt.mFormatID = kAudioFormatLinearPCM;
        Fmt.mFormatFlags = kAudioFormatFlagIsSignedInteger | kAudioFormatFlagIsPacked;
        Fmt.mChannelsPerFrame = 2;
        Fmt.mBitsPerChannel = 16;
        Fmt.mBytesPerFrame = 4;      // 2 ch * 2 bytes
        Fmt.mFramesPerPacket = 1;
        Fmt.mBytesPerPacket = 4;
        AudioUnitSetProperty(Unit, kAudioUnitProperty_StreamFormat,
                             kAudioUnitScope_Input, 0, &Fmt, sizeof(Fmt));

        AURenderCallbackStruct Rc = {};
        Rc.inputProc = &RemoteIoDevice::RenderProc;
        Rc.inputProcRefCon = this;
        AudioUnitSetProperty(Unit, kAudioUnitProperty_SetRenderCallback,
                             kAudioUnitScope_Input, 0, &Rc, sizeof(Rc));

        if (AudioUnitInitialize(Unit) != noErr || AudioOutputUnitStart(Unit) != noErr) { Stop(); return false; }
        return true;
    }

    void Stop() override {
        if (Unit == nullptr) return;
        AudioOutputUnitStop(Unit);
        AudioUnitUninitialize(Unit);
        AudioComponentInstanceDispose(Unit);
        Unit = nullptr;
    }

    uint32_t OutputRate() const override { return 48000; }

private:
    static constexpr uint32_t MaxFrames = 4096;

    static OSStatus RenderProc(void* RefCon, AudioUnitRenderActionFlags* /*Flags*/,
                               const AudioTimeStamp* /*Ts*/, UInt32 /*Bus*/,
                               UInt32 NumFrames, AudioBufferList* IoData) {
        auto* Self = static_cast<RemoteIoDevice*>(RefCon);
        auto* Out = static_cast<int16_t*>(IoData->mBuffers[0].mData);
        uint32_t Frames = NumFrames;
        if (Frames > MaxFrames) Frames = MaxFrames;

        if (Self->Callback != nullptr) Self->Callback(Self->UserPtr, Self->Mono, Frames);
        else std::memset(Self->Mono, 0, Frames * sizeof(int16_t));

        for (uint32_t i = 0; i < Frames; ++i) {   // mono -> interleaved stereo
            Out[2 * i] = Self->Mono[i];
            Out[2 * i + 1] = Self->Mono[i];
        }
        return noErr;
    }

    AudioUnit Unit = nullptr;
    RenderCallback Callback = nullptr;
    void* UserPtr = nullptr;
    int16_t Mono[MaxFrames] = {};   // audio-thread scratch (single-threaded callback)
};

} // namespace

IAudioDevice* CreateAudioDevice() { return new RemoteIoDevice(); }

} // namespace Lur::Audio
