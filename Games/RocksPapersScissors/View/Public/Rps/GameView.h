#pragma once
#include <atomic>

#include "Lur/Hud/Dropdown.h"
#include "Lur/Hud/TextField.h"
#include "Lur/Render/Renderer.h"
#include "Lur/Text/Font.h"
#include "Rps/Snapshot.h"

namespace Rps {

// The RPS presentation layer — draws one Snapshot to an IRenderer. Talks only to the
// renderer interface (no Vulkan), so it builds on the host too and is shared by the
// desktop and (later) the phone mains. Mirrors chess's BoardView in spirit.
//
// This is the BRING-UP renderer: one DrawMesh per unit, positions interpolated on the
// CPU (Prev->Pos by alpha). It is deliberately non-instanced — proving the whole
// window -> renderer -> SimRunner -> snapshot pipeline end-to-end on screen first. The
// design's one-instanced-draw + shader mix(prev,curr,alpha) replaces this inner loop
// later (a renderer extension), the same brute-force-then-optimise discipline the
// spatial grid used.
class GameView {
public:
    void CreateResources(Lur::Render::IRenderer* Renderer);

    // Draw the field + units + HUD for this snapshot. CameraY is the world-Y at the
    // bottom of the screen (the swipe scroll position); Alpha in [0,1] interpolates
    // Prev->Pos. Owns the full BeginFrame..EndFrame. Non-const: fills the instance
    // scratch buffer each frame (units draw as ONE instanced call, interpolated in the
    // vertex shader).
    // FlipY mirrors the field vertically for the top player (team 1) so BOTH players see
    // their own camp at the bottom (§9's per-player view flip). View-only, per-device.
    void Render(Lur::Render::IRenderer* Renderer, const Snapshot& Snap, float Alpha,
                float CameraY, float WidthPx, float HeightPx, bool FlipY, float DtSec);

    // World units visible vertically at this width — for the caller's camera clamp.
    static float VisibleWorldHeight(float WidthPx, float HeightPx);

    // Link status for the opponent selector's dot (view-only; call when it changes).
    void SetLinked(bool InLinked);

    // OS safe-area insets in pixels: the HUD's top block (dropdown + panel) starts
    // below TopPx (status bar / notch) and the production plates sit above BottomPx
    // (Android navigation bar / iOS home indicator). View-only, per-device.
    void SetInsets(float TopPx, float BottomPx) { TopInsetPx = TopPx; BottomInsetPx = BottomPx; }

    // Heights in WORLD units of the HUD blocks at this width. The mains extend the
    // camera range by these (MinCam = -Bottom, MaxCam += Top) so BOTH camps scroll
    // clear of the chrome: yours above the plates, the enemy's below the top panel.
    float BottomHudWorldUnits(float WidthPx) const;
    float TopHudWorldUnits(float WidthPx) const;

    // Route a tap at the HUD (call before treating it as a camera drag/tap).
    // Returns a unit type 0..3 when a production plate was pressed, -2 when the HUD
    // consumed the tap (the opponent selector), or -1 when the tap is the world's.
    int OnTap(float XPx, float YPx);
#if !LUR_SHIPPING
    // #113 dev overlay: a tap (input thread) is stashed and consumed on the render thread,
    // where the CVar-row rects are known — so hit-test + the CVar nudge are race-free with
    // the ValueString read. Tapping a row cycles that gameplay CVar (double, wrap to
    // default) LOCALLY; single-device it just updates the browser (no live match to react).
    void DevTap(float XPx, float YPx);
#endif

private:
    Lur::Render::MeshHandle Quad = 0;  // one white unit quad; materials supply colour

    // Field backdrop (#85, locked palette): a unit-rect mesh with the multi-stop
    // vertical gradient baked as vertex colours, scaled to the framebuffer each frame
    // (SCREENSPACE: it never scrolls). VLineMesh is the same idea for the vertical
    // grid lines (2-stop grid gradient); horizontal lines pick a flat colour from
    // GridLut by their screen Y, so the grid's colour is screen-anchored while the
    // lines themselves scroll with the world.
    Lur::Render::MeshHandle FieldGradMesh = 0;
    Lur::Render::MeshHandle VLineMesh = 0;
    static constexpr int GridShades = 17;
    Lur::Render::MaterialHandle GridLut[GridShades] = {};
    Lur::Render::MaterialHandle WhiteMat = 0;

    // The cooked glyph atlas (#85): 8 white silhouettes side by side, RG8
    // shade+coverage. Everything on the field — units, mines, camps, HUD icons —
    // is one of these masks under a tint (the locked "alpha-cutout silhouette" rule).
    enum EGlyph { GlyphMiner = 0, GlyphRock, GlyphPaper, GlyphScissors,
                  GlyphGold, GlyphMine, GlyphSwords, GlyphCamp, GlyphPointer,
                  GlyphOreLoad, GlyphCount };  // OreLoad = the heap on a full cart
    Lur::Render::TextureHandle IconAtlas = 0;
    Lur::Render::MaterialHandle AtlasMat = 0;        // white tint: per-instance colour is the fill
    Lur::Render::MeshHandle GlyphMesh[GlyphCount] = {};  // unit quads with per-glyph atlas UVs

    // Atlas-tinted materials for the DrawMesh path (mines / camps).
    Lur::Render::MaterialHandle CampMat[2] = {};
    Lur::Render::MaterialHandle MineMat = 0;
    // Flat-colour materials (BaseColor 0 = white, Tint = the colour).
    Lur::Render::MaterialHandle HealthBg = 0;
    Lur::Render::MaterialHandle HealthFg = 0;
    Lur::Render::MaterialHandle GoldBarFg = 0;   // mine reserve bar (#84) — gold accent
#if !LUR_SHIPPING
    Lur::Render::MaterialHandle DevPanelMat = 0;   // #113 dev overlay: charcoal translucent
    Lur::Render::MaterialHandle DevAccentMat = 0;  //   + cyan accent (DevTheme, bring-up)
    std::atomic<float> DevTapX_{-1.0e9f};          // input-thread tap -> render-thread nudge
    std::atomic<float> DevTapY_{-1.0e9f};
    std::atomic<bool>  DevTapPending_{false};
#endif

    Lur::Render::Color TeamTint[2] = {};              // locked BASE team colours
    // Per-(team,type) tint: a unique shade of the team hue per unit type (playtest
    // 2026-07-20) — reinforces type by colour on top of the glyph shape. The colour is
    // the per-instance fill for units; the materials tint the HUD production buttons.
    Lur::Render::Color TeamTypeTint[2][UnitCount] = {};
    Lur::Render::MaterialHandle TypeTintMat[2][UnitCount] = {};      // button glyph (affordable)
    Lur::Render::MaterialHandle TypeTintMatDim[2][UnitCount] = {};   // button glyph (unaffordable)
    Lur::Render::InstanceData Instances[MaxUnits];    // per-frame scratch (one instanced draw)

    Lur::Text::Font Font;
    Lur::Hud::TextField Text;

    // ---- HUD (#85, locked layout): opponent dropdown above the status panel
    // (gold | population | clock), four production plates along the bottom. ----
    Lur::Hud::Dropdown Selector;          // engine widget — same one chess uses
    float TopInsetPx = 0.0f;              // OS safe areas (status bar / nav bar)
    float BottomInsetPx = 0.0f;
    bool Linked = false;
    bool SelectorDirty = true;            // rebuild items when link state changes
    Lur::Text::Font ClockFont;            // DSEG7: monospaced digits for the match clock
    Lur::Hud::TextField ClockText;
    float PlateRect[4][4] = {};           // per-type plate {x,y,w,h}, cached for OnTap
    Lur::Render::MaterialHandle PanelMat = 0;
    Lur::Render::MaterialHandle PanelEdge = 0;
    Lur::Render::MaterialHandle PlateBg = 0;
    Lur::Render::MaterialHandle BarBg = 0;
    Lur::Render::MaterialHandle GoldFlat = 0;
    Lur::Render::MaterialHandle PlateIconMat = 0;     // plate glyph fill (#C9D3DA)
    Lur::Render::MaterialHandle PlateIconDim = 0;     // unaffordable: dimmed
    Lur::Render::MaterialHandle GoldIconMat = 0;      // gold glyph (costs, counter)
    Lur::Render::MaterialHandle MiniWinMat = 0;       // minimap camera window fill
    Lur::Render::MaterialHandle MiniWinEdge = 0;      // minimap camera window edge lines

    void RefreshSelector();

    // ---- View-side juice (#85 playtest feedback) — all per-device, never sim ----
    // "+N" floats above a miner banking its carry (world-anchored, rise + fade).
    struct GoldFloat { float Wx = 0, Wy = 0, Age = 0; int32_t Value = 0; bool Active = false; };
    static constexpr int MaxFloats = 24;
    GoldFloat Floats[MaxFloats];
    int32_t LastCarry[MaxUnits] = {};     // deposit edge detection (carry >0 -> 0)
    // Held facing per slot: soldiers orient to their MOVE direction, but below a low
    // speed we STOP updating (keep the last angle) so a nearly-stopped unit doesn't jitter
    // its heading on noise. Persists across frames; 0 = upright until the unit first moves.
    float LastFaceX[MaxUnits] = {};
    float LastFaceY[MaxUnits] = {};
    // Gold counter animation: the shown value rolls toward the real one and pops on gain.
    float DisplayedGold = -1.0f;
    float GoldPulse = 0.0f;
    // First-scroll hint: pointing finger + up/down arrows bobbing mid-screen from the
    // moment one of YOUR units leaves the screen until the first camera pan.
    enum class EHint : uint8_t { Idle, Active, Fading, Done };
    EHint Hint = EHint::Idle;
    float HintAge = 0.0f, HintFade = 0.0f, HintCamY = 0.0f;
    static constexpr int HintAlphaSteps = 6;   // materials are immutable: fade = LUT
    Lur::Render::MaterialHandle HintPointer[HintAlphaSteps] = {};
    Lur::Render::MaterialHandle HintArrow[HintAlphaSteps] = {};
    Lur::Render::MeshHandle ArrowUp = 0, ArrowDown = 0;

    bool Ready = false;
};

}  // namespace Rps
