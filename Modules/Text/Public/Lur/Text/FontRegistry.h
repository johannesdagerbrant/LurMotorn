#pragma once
#include <cstdint>

#include "Lur/Text/Font.h"

namespace Lur::Text {

// Stable, lightweight handle into a FontRegistry (0 = invalid).
using FontHandle = uint32_t;

// Owns the engine's loaded fonts and hands out FontHandles. Widgets select a font by
// handle (resolved once from a name), never by owning a Font — mirroring the
// Render handle model. Fixed capacity, no heap allocation (hot-path friendly).
//
// Registering only builds the runtime Font (metrics + glyph lookup). The MSDF atlas is
// uploaded lazily via UploadAll() once a renderer exists (render side, #26); until then
// layout works fine against the metrics alone.
class FontRegistry {
public:
    static constexpr int MaxFonts   = 8;
    static constexpr int MaxNameLen = 31;   // + NUL

    // Register a cooked font under a short name. Returns its handle, or re-returns the
    // existing handle if the name is already registered (idempotent). 0 if full.
    FontHandle Register(const char* Name, const CookedFont& Cooked);

    FontHandle  Find(const char* Name) const;   // 0 if absent
    bool        IsValid(FontHandle Handle) const;
    const Font& Get(FontHandle Handle) const;    // Handle must be valid
    Font&       GetMutable(FontHandle Handle);   // for atlas upload
    int         Count() const { return N; }

    // Upload every registered font's atlas to the renderer (call once, after Init).
    void UploadAll(Render::IRenderer& Renderer);

private:
    struct Entry {
        char Name[MaxNameLen + 1] = {};
        Font Font;
    };
    Entry Entries[MaxFonts];
    int   N = 0;
};

} // namespace Lur::Text
