#pragma once
#include <cstdint>

#include "Lur/Render/Renderer.h"   // Render::TextureHandle
#include "Lur/Text/CookedFont.h"

namespace Lur::Text {

// A runtime font: a lightweight view over a cooked MSDF asset (CookedFont) plus the
// uploaded atlas texture handle. Built once from a CookedFont; the glyph lookup binary-
// searches the (codepoint-sorted) cooked table, so Font itself owns no storage and is
// cheap to copy. Metrics are em-normalised and y-DOWN (see CookedFont.h).
//
// The atlas TextureHandle is 0 until UploadAtlas() runs against a live renderer — layout
// needs only the metrics, so a Font is fully usable (and host-testable) without a GPU.
class Font {
public:
    Font() = default;
    explicit Font(const CookedFont& Cooked) { Init(Cooked); }

    void Init(const CookedFont& Cooked);
    bool IsValid() const { return Source != nullptr; }

    // Glyph lookup by Unicode code point; nullptr if the font lacks it.
    const CookedGlyph* Find(uint32_t Codepoint) const;
    // Advance width in em (0 if the glyph is absent).
    float Advance(uint32_t Codepoint) const;

    float LineHeight()    const { return Source ? Source->LineHeight : 0.0f; }
    float Ascender()      const { return Source ? Source->Ascender : 0.0f; }   // y-down: < 0
    float Descender()     const { return Source ? Source->Descender : 0.0f; }
    float DistanceRange() const { return Source ? Source->DistanceRange : 0.0f; }

    const CookedFont& Cooked() const { return *Source; }

    // The MSDF atlas texture (0 until uploaded). UploadAtlas expands the cooked RGB
    // bytes to RGBA and calls Renderer->LoadTexture once; safe to call again (re-uploads).
    Render::TextureHandle Atlas() const { return AtlasTex; }
    void UploadAtlas(Render::IRenderer& Renderer);
    void SetAtlas(Render::TextureHandle Handle) { AtlasTex = Handle; }

private:
    const CookedFont*     Source   = nullptr;
    Render::TextureHandle AtlasTex = 0;
};

} // namespace Lur::Text
