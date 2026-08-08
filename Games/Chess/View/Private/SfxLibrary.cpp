#include "Chess/View/SfxLibrary.h"

#include <cmath>
#include <utility>

#include "Lur/Audio/PcmCodec.h"

// Content reference the cook derives its work from (Cook/Cook.ps1): compress these sounds
// (paths relative to this game's Content/) into LSF1 lossless blobs in the header included
// below. The list is FLAT and its order is the cooked enum's order; the grouping into events
// lives in Groups[] here, not in the cook — see Cook/README.md on why the cooker stays dumb.
// LUR_COOK audio src=Audio/Move1.wav,Audio/Move2.wav,Audio/Move3.wav,Audio/Capture1.wav,Audio/Capture2.wav,Audio/Capture3.wav,Audio/Check.wav,Audio/Checkmate.wav out=View/Private/SfxClips.h ns=ChessSfx clips=Clips index=ESfx
#include "SfxClips.h"  // cooked 16-bit/48k mono SFX, one LSF1 blob per clip

namespace Chess {
namespace {

// One event's clips and how much to vary them. First/Count index the flat cooked list.
//
// PitchPct/GainDb are ZERO for the alerts on purpose: Check and Checkmate must sound the
// same every time they fire, so the player learns them. Only the frequent events jitter.
struct SfxGroup {
    int   First;
    int   Count;
    float PitchPct;   // +/- playback-rate jitter, as a fraction (0.05 = +/-5%, ~a semitone)
    float GainDb;     // +/- level jitter
};

constexpr SfxGroup Groups[4] = {
    /* Move      */ {ChessSfx::Move1,     3, 0.05f, 2.0f},
    /* Capture   */ {ChessSfx::Capture1,  3, 0.05f, 2.0f},
    /* Check     */ {ChessSfx::Check,     1, 0.0f,  0.0f},
    /* Checkmate */ {ChessSfx::Checkmate, 1, 0.0f,  0.0f},
};

const SfxGroup& GroupOf(EMoveSound Which) {
    return Groups[static_cast<int>(Which)];
}

// Keep the event table and the cooked clip order in lockstep. Reordering the cook sources
// without updating Groups[] (or vice-versa) is a compile error, not a silent mix-up where
// a capture plays the checkmate stinger.
static_assert(ChessSfx::Move2 == ChessSfx::Move1 + 1, "cook order drift: Move variants must be contiguous");
static_assert(ChessSfx::Move3 == ChessSfx::Move1 + 2, "cook order drift: Move variants must be contiguous");
static_assert(ChessSfx::Capture2 == ChessSfx::Capture1 + 1, "cook order drift: Capture variants must be contiguous");
static_assert(ChessSfx::Capture3 == ChessSfx::Capture1 + 2, "cook order drift: Capture variants must be contiguous");
static_assert(ChessSfx::SfxCount == 8, "cook clip count changed — update Groups[]");

}  // namespace

void SfxLibrary::Load(Lur::Audio::Mixer& Mixer) {
    Ids.clear();
    Ids.reserve(ChessSfx::SfxCount);
    for (int i = 0; i < ChessSfx::SfxCount; ++i) {
        const ChessSfx::SfxBlob& B = ChessSfx::Clips[i];
        Lur::Audio::Sound S;
        if (Lur::Audio::DecodeLossless(ChessSfx::ClipsData + B.Offset, B.Size, S))
            Ids.push_back(Mixer.Add(std::move(S)));
        else
            Ids.push_back(Lur::Audio::InvalidSound);   // corrupt blob: silently skip that clip
    }
}

int SfxLibrary::VariantCount(EMoveSound Which) {
    return GroupOf(Which).Count;
}

int SfxLibrary::PickVariant(EMoveSound Which) {
    const SfxGroup& G = GroupOf(Which);
    if (G.Count <= 1) return 0;

    Rng = Rng * 1664525u + 1013904223u;                  // Numerical-Recipes LCG
    const int Last = LastPick[static_cast<int>(Which)];
    // Draw from the OTHER variants and skip past the last one, so a repeat is impossible
    // rather than merely unlikely — two identical clicks in a row is exactly the artefact
    // this exists to kill, and uniform random would still do it one time in three.
    int Pick = static_cast<int>((Rng >> 8) % static_cast<std::uint32_t>(G.Count - 1));
    if (Last >= 0 && Pick >= Last) ++Pick;
    LastPick[static_cast<int>(Which)] = Pick;
    return Pick;
}

void SfxLibrary::Play(Lur::Audio::Mixer& Mixer, EMoveSound Which) {
    if (Ids.empty()) return;
    const SfxGroup& G = GroupOf(Which);
    const std::size_t Slot = static_cast<std::size_t>(G.First + PickVariant(Which));
    if (Slot >= Ids.size()) return;

    float Pitch = 1.0f;
    float Gain = 1.0f;
    if (G.PitchPct > 0.0f || G.GainDb > 0.0f) {
        // Two more draws, one per knob, so pitch and level are independent — jittering
        // them together would just sound like one louder-and-higher knob.
        Rng = Rng * 1664525u + 1013904223u;
        const float U1 = static_cast<float>((Rng >> 8) & 0xFFFF) / 32767.5f - 1.0f;  // [-1,1]
        Rng = Rng * 1664525u + 1013904223u;
        const float U2 = static_cast<float>((Rng >> 8) & 0xFFFF) / 32767.5f - 1.0f;
        Pitch = 1.0f + U1 * G.PitchPct;
        Gain = std::pow(10.0f, (U2 * G.GainDb) / 20.0f);
    }
    Mixer.Play(Ids[Slot], Gain, Pitch);
}

} // namespace Chess
