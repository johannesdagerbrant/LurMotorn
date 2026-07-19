#pragma once
#include <vector>

#include "Lur/Audio/Mixer.h"

namespace Chess {

// The chess sound effects, decoded from their cooked LSF1 blobs into a mixer once at load,
// then triggered by name. It owns NO audio device — the app owns the device and points its
// render callback at the same Mixer; SfxLibrary just registers the clips and fires them. The
// enum here is the game-facing name; the cooked order (SfxClips.h) is asserted to match it.
enum class ESfx { Move, Capture, Check };

class SfxLibrary {
public:
    // Decode every cooked clip and register it with the mixer. Call ONCE, before the audio
    // device starts pulling frames (Mixer::Add must not race the audio thread).
    void Load(Lur::Audio::Mixer& Mixer);

    bool Loaded() const { return !Ids.empty(); }

    // Trigger a clip (wait-free via Mixer::Play). No-op if not loaded or Which is unknown.
    void Play(Lur::Audio::Mixer& Mixer, ESfx Which) const;

private:
    std::vector<Lur::Audio::SoundId> Ids;   // indexed by ESfx / cooked clip order
};

} // namespace Chess
