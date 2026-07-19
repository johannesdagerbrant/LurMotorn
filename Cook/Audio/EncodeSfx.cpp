// Offline SFX encoder used by Cook/CookAudio.ps1. Reads a 16-bit PCM WAV and writes the
// compressed LSF1 blob (see Modules/Audio PcmCodec). It compiles the ENGINE's own
// PcmCodec.cpp verbatim, so the bytes it emits are exactly what the app's DecodeLossless
// expects — one codec, no drift. Host-only; never linked into the app.
//
//   encodesfx <in.wav> <out.bin>
//
// The cook builds this with the same MinGW g++ the host build uses, caches the exe, and
// runs it per clip. Because cooked headers are committed, this only runs when audio content
// changes on a dev machine (never on the phones' builds).
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Lur/Audio/PcmCodec.h"

using namespace Lur::Audio;

static uint32_t Rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8)
         | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
static uint16_t Rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

int main(int argc, char** argv) {
    if (argc != 3) { std::fprintf(stderr, "usage: encodesfx <in.wav> <out.bin>\n"); return 2; }

    std::FILE* f = std::fopen(argv[1], "rb");
    if (!f) { std::fprintf(stderr, "encodesfx: cannot open %s\n", argv[1]); return 1; }
    std::fseek(f, 0, SEEK_END);
    const long Size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> D(Size > 0 ? static_cast<size_t>(Size) : 0);
    if (!D.empty() && std::fread(D.data(), 1, D.size(), f) != D.size()) {
        std::fprintf(stderr, "encodesfx: short read %s\n", argv[1]); std::fclose(f); return 1;
    }
    std::fclose(f);

    if (D.size() < 12 || std::memcmp(D.data(), "RIFF", 4) != 0 || std::memcmp(D.data() + 8, "WAVE", 4) != 0) {
        std::fprintf(stderr, "encodesfx: %s is not a WAV\n", argv[1]); return 1;
    }

    uint16_t Fmt = 0, Ch = 0, Bits = 0;
    uint32_t Rate = 0, DataOff = 0, DataLen = 0;
    for (size_t i = 12; i + 8 <= D.size();) {          // RIFF chunk walk
        const uint8_t* C = D.data() + i;
        const uint32_t Len = Rd32(C + 4);
        if (std::memcmp(C, "fmt ", 4) == 0 && i + 8 + 16 <= D.size()) {
            Fmt = Rd16(C + 8); Ch = Rd16(C + 10); Rate = Rd32(C + 12); Bits = Rd16(C + 22);
        } else if (std::memcmp(C, "data", 4) == 0) {
            DataOff = static_cast<uint32_t>(i + 8); DataLen = Len;
        }
        i += 8 + Len + (Len & 1);                       // chunks are word-aligned
    }

    if (Fmt != 1 || Bits != 16 || Ch == 0 || DataOff == 0) {
        std::fprintf(stderr, "encodesfx: %s must be 16-bit PCM WAV (fmt=%u bits=%u ch=%u)\n",
                     argv[1], Fmt, Bits, Ch);
        return 1;
    }
    if (DataOff + DataLen > D.size()) DataLen = static_cast<uint32_t>(D.size()) - DataOff;  // clamp

    const uint32_t Frames = DataLen / 2u / Ch;
    std::vector<int16_t> Mono(Frames);
    for (uint32_t k = 0; k < Frames; ++k) {
        int32_t Acc = 0;
        for (uint16_t c = 0; c < Ch; ++c) {             // downmix (already mono in practice)
            int16_t s;
            std::memcpy(&s, D.data() + DataOff + (static_cast<size_t>(k) * Ch + c) * 2, 2);
            Acc += s;
        }
        Mono[k] = static_cast<int16_t>(Acc / Ch);
    }

    const std::vector<uint8_t> Blob = EncodeLossless(Mono.data(), Frames, Rate);

    std::FILE* o = std::fopen(argv[2], "wb");
    if (!o) { std::fprintf(stderr, "encodesfx: cannot write %s\n", argv[2]); return 1; }
    if (!Blob.empty()) std::fwrite(Blob.data(), 1, Blob.size(), o);
    std::fclose(o);
    return 0;
}
