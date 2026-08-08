#pragma once
#include <cstdint>
#include <vector>

#include "Chess/View/MoveSound.h"
#include "Lur/Audio/Mixer.h"

namespace Chess {

// The chess sound effects, decoded from their cooked LSF1 blobs into a mixer once at load,
// then triggered by EVENT (EMoveSound), not by clip. It owns NO audio device — the app owns
// the device and points its render callback at the same Mixer; SfxLibrary just registers the
// clips and fires them.
//
// The mapping from event to sound is the interesting part (issue #78). An event owns a
// GROUP of interchangeable clips plus a variation policy, and the two kinds of event want
// opposite treatment:
//
//   * FREQUENT events (Move, Capture) fire hundreds of times a match. Played verbatim they
//     become a machine-gun of one identical click, which is what the single Move clip used
//     to sound like at blitz speed. Each gets 3 clips, a no-immediate-repeat pick, and per-
//     trigger pitch + gain jitter — so no two triggers are the same in either clip OR tone,
//     and a handful of committed samples buys a lot of perceived variety.
//   * RARE events (Check, Checkmate) fire once or twice. They are ALERTS, so they are played
//     verbatim, every time: consistency is what makes them recognisable, and varying them
//     would only blur the signal.
//
// Variation is COSMETIC. It runs off a local RNG that must never be the sim's — the two
// peers are free to hear different variants of the same move, and nothing here may touch
// the wire or determinism.
class SfxLibrary {
public:
    // Decode every cooked clip and register it with the mixer. Call ONCE, before the audio
    // device starts pulling frames (Mixer::Add must not race the audio thread).
    void Load(Lur::Audio::Mixer& Mixer);

    bool Loaded() const { return !Ids.empty(); }

    // Trigger the sound for an event (wait-free via Mixer::Play). No-op if not loaded.
    // Non-const: picking a variant advances the RNG and the no-repeat memory.
    void Play(Lur::Audio::Mixer& Mixer, EMoveSound Which);

    // How many clips back an event — 3 for the varied ones, 1 for the alerts. Nothing
    // hard-codes 3, so adding a 4th Move variant is a cook-marker edit plus one number.
    static int VariantCount(EMoveSound Which);

    // Pick the next clip for an event, returning an index into that event's group
    // (0..VariantCount-1) and advancing the no-repeat memory. Play() calls this; it is
    // public because the SELECTION POLICY — never the same clip twice running — is a
    // behaviour of this class worth pinning down in a test, not an implementation detail.
    int PickVariant(EMoveSound Which);

private:

    std::vector<Lur::Audio::SoundId> Ids;   // indexed by cooked clip order (SfxClips.h)
    std::uint32_t Rng = 0x9E3779B9u;        // cosmetic only — NEVER the sim's RNG
    int LastPick[4] = {-1, -1, -1, -1};     // per EMoveSound, the variant played last
};

} // namespace Chess
