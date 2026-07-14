#include "Lur/Text/FontRegistry.h"

#include <cstring>

namespace Lur::Text {

FontHandle FontRegistry::Register(const char* Name, const CookedFont& Cooked) {
    if (const FontHandle Existing = Find(Name)) return Existing;
    if (N >= MaxFonts) return 0;
    Entry& E = Entries[N];
    std::strncpy(E.Name, Name, MaxNameLen);
    E.Name[MaxNameLen] = '\0';
    E.Font.Init(Cooked);
    ++N;
    return static_cast<FontHandle>(N);   // 1-based; 0 = invalid
}

FontHandle FontRegistry::Find(const char* Name) const {
    for (int i = 0; i < N; ++i) {
        if (std::strncmp(Entries[i].Name, Name, MaxNameLen) == 0)
            return static_cast<FontHandle>(i + 1);
    }
    return 0;
}

bool FontRegistry::IsValid(FontHandle Handle) const {
    return Handle >= 1 && static_cast<int>(Handle) <= N;
}

const Font& FontRegistry::Get(FontHandle Handle) const {
    return Entries[Handle - 1].Font;
}

Font& FontRegistry::GetMutable(FontHandle Handle) {
    return Entries[Handle - 1].Font;
}

void FontRegistry::UploadAll(Render::IRenderer& Renderer) {
    for (int i = 0; i < N; ++i) Entries[i].Font.UploadAtlas(Renderer);
}

} // namespace Lur::Text
