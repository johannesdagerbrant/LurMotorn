#pragma once
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "Lur/Hud/TextField.h"
#include "Lur/Render/Renderer.h"

namespace Lur::Text { class Font; }

namespace Lur::Hud {

// How an item's left marker is drawn. Dot = a filled status circle (LeadFill),
// optionally ringed (Ring/RingColor). Split = a two-tone disc for a "this device /
// both sides" entry. None = no marker.
enum class ELeadStyle : uint8_t { None, Dot, Split };

// One row in the dropdown. Chess-agnostic: the game maps its meaning onto these
// generic fields (a labelled row with a coloured status dot + optional ring).
struct DropdownItem {
    std::string        Label;                 // primary line (e.g. a GUID)
    std::string        Sublabel;              // secondary line (e.g. "moved 2m ago")
    ELeadStyle         Lead = ELeadStyle::Dot;
    Lur::Render::Color LeadFill{};            // status-dot fill
    bool               Ring = false;          // draw the "attention" ring
    Lur::Render::Color RingColor{};           // ring colour
    bool               Header = false;        // a non-selectable section label
};

// A tap-to-open dropdown/selector for the GUI layer (draw it inside BeginGui()).
// Mirrors the other Hud widgets: default-constructed member, two-phase
// CreateResources, per-frame Draw, hit-tested in the same pixel rects it draws.
//
// Colour rides on mesh vertices (the sprite shader does vertexColour x Tint), so the
// whole widget draws with ONE white material plus the text atlas — it never competes
// for the renderer's material pool. Per-colour dot/ring meshes are built lazily and
// cached, so opening the menu allocates nothing after the first time each colour is
// seen.
class Dropdown {
public:
    void CreateResources(Lur::Render::IRenderer* Renderer, const Lur::Text::Font* Font);

    // Replace the item list (call on open / when state changes, NOT every frame).
    // Resolves each item's colours to cached meshes up front.
    void SetItems(const DropdownItem* Items, int Count);

    void SetSelected(int Index);
    int  Selected() const { return SelectedIdx; }

    // Draw the "Title" caption, the collapsed pill showing the current selection, and
    // — when open — the list below it. (X,Y) is the widget's top-left; W its width;
    // PillH the pill height (all other metrics derive from it). Caches the drawn rects
    // for OnTap.
    void Draw(Lur::Render::IRenderer* Renderer, const char* Title,
              float X, float Y, float W, float PillH);

    // Route a tap. Returns true if consumed: a tap on the pill (toggles open), or —
    // while open — a tap on any row (selects + closes) or elsewhere (closes). The
    // caller must early-out when this returns true so the tap doesn't reach the game.
    bool OnTap(float XPx, float YPx);

    bool IsOpen() const { return Open; }

    // One-shot: true exactly once after a row selection, so the game can react
    // (e.g. switch the active opponent) without a callback.
    bool TookSelection();

private:
    Lur::Render::MeshHandle MeshFor(Lur::Render::Color C, bool RingShape);
    void DrawBox(Lur::Render::IRenderer* R, Lur::Render::MeshHandle M,
                 float X, float Y, float W, float H) const;
    void DrawRowContent(Lur::Render::IRenderer* R, int ItemIndex,
                        float X, float Y, float W, float RowH, bool Selected);

    Lur::Render::IRenderer* Rend = nullptr;
    TextField               Text;                       // owns glyph layout + atlas

    // Shared white material; colour comes from the mesh vertices.
    Lur::Render::MaterialHandle White = 0;
    // Fixed chrome meshes (unit-space, baked colour), scaled per draw.
    Lur::Render::MeshHandle QuadPanel = 0, QuadLine = 0, QuadAccent = 0;
    Lur::Render::MeshHandle SplitDisc = 0, ChevronDown = 0, ChevronUp = 0;

    // Per-colour dot/ring mesh caches (key = packed RGBA8, bit 31 = ring vs disc).
    std::unordered_map<uint32_t, Lur::Render::MeshHandle> ColorMesh;

    std::vector<DropdownItem> Items;
    int  SelectedIdx = 0;
    bool Open = false;
    bool SelectionLatch = false;

    // Layout cached by the last Draw, for hit-testing in OnTap.
    struct RectPx { float X0, Y0, X1, Y1; };
    RectPx              PillRect{};
    std::vector<RectPx> RowRects;      // parallel to RowItem
    std::vector<int>    RowItem;       // item index for each drawn selectable row
};

} // namespace Lur::Hud
