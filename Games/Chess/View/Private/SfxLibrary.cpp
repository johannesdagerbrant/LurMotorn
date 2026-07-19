#include "Chess/View/SfxLibrary.h"

#include <utility>

#include "Lur/Audio/PcmCodec.h"

// Content reference the cook derives its work from (Cook/Cook.ps1): compress these move
// sounds (paths relative to this game's Content/) into LSF1 lossless blobs in the header
// included below. Order == Chess::ESfx (Move, Capture, Check).
// LUR_COOK audio src=Audio/Move.wav,Audio/Capture.wav,Audio/Check.wav out=View/Private/SfxClips.h ns=ChessSfx clips=Clips index=ESfx
#include "SfxClips.h"  // cooked 16-bit/48k mono SFX, one LSF1 blob per ESfx

namespace Chess {

// Keep the game-facing enum and the cooked clip order in lockstep — reordering the cook
// sources without updating ESfx (or vice-versa) is a compile error, not a silent mix-up.
static_assert(static_cast<int>(ESfx::Move) == ChessSfx::Move, "SFX order drift: Move");
static_assert(static_cast<int>(ESfx::Capture) == ChessSfx::Capture, "SFX order drift: Capture");
static_assert(static_cast<int>(ESfx::Check) == ChessSfx::Check, "SFX order drift: Check");

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

void SfxLibrary::Play(Lur::Audio::Mixer& Mixer, ESfx Which) const {
    const std::size_t i = static_cast<std::size_t>(Which);
    if (i < Ids.size()) Mixer.Play(Ids[i]);
}

} // namespace Chess
