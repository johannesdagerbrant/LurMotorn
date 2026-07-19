#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

#include "Lur/Audio/Sound.h"

namespace Lur::Audio {

// Hand-rolled LOSSLESS codec for mono 16-bit PCM SFX — format tag "LSF1" (Lur SFX v1).
//
// It is our own FLAC-lite: the signal is split into blocks; each block picks the best of
// four FLAC-style fixed linear predictors (orders 0-3) and Rice/Golomb-codes the integer
// residuals. Reconstruction is bit-exact — no quantisation anywhere — so "lossless" means
// the decoded int16 samples equal the source samples exactly.
//
// The split of labour follows the engine's content pipeline: the offline COOK calls
// EncodeLossless to shrink each split clip into a slim byte blob it embeds as a header; the
// APP calls DecodeLossless ONCE at load to expand the blob back to PCM the mixer plays. The
// runtime never touches the codec on the audio thread. One implementation, both directions,
// so encoder and decoder can never drift (the alternative — a PowerShell encoder vs. a C++
// decoder — would silently corrupt on any format tweak).
//
// Blob layout: 12-byte header { 'L','S','F','1', u32 Rate LE, u32 FrameCount LE } followed
// by the MSB-first bitstream of blocks.

// Encode mono 16-bit PCM -> compressed LSF1 blob. Used by the offline cook tool.
std::vector<uint8_t> EncodeLossless(const int16_t* Pcm, uint32_t Frames, uint32_t Rate);

// Decode an LSF1 blob -> Sound (exact PCM). Returns false on a bad/truncated blob (a
// shipped game must not crash on corrupt embedded data). Used at load time in the app.
bool DecodeLossless(const uint8_t* Data, size_t Size, Sound& Out);

} // namespace Lur::Audio
