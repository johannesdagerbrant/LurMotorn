#pragma once
// Lur::DevGui::Console — the dev console, whole, as an engine facility (#201 / epic #39 Phase 5).
//
// One tool, ONE UI, now in ONE place. It lists every registered CVar as a collapsible tree of rows,
// edits numbers through a numpad and colours through an HSV picker, toggles bools in place, runs
// DevCommands from a button strip, and reports tooltips and command output through a toaster. Opened
// by the phone's two-finger triple-tap or the desktop console key; identical either way.
//
// ---- Why this class holds an IRenderer when no other DevGui type may ----
// Every other type here (Numpad, ColorPicker, Slider, FlatList, CategoryTree) is deliberately
// render-agnostic: it computes geometry and answers hit-tests, and the host draws it. That rule
// exists so the rect drawn and the rect hit-tested are literally the same call — it is what stops a
// button existing somewhere you cannot press.
//
// The Console is not a widget, it is the whole tool, and it keeps that rule INTERNALLY: it is the
// single host that draws those widgets' rects and tests those widgets' rects. Holding the renderer
// does not weaken the invariant; it puts the one legitimate host in the engine instead of pasting it
// into each game. The widgets stay geometry-only because they are the pieces a DIFFERENT tool may
// reuse.
//
// Before this, the console's ~650 lines of painting lived in Rps::GameView and chess had a 120-line
// reimplementation of the subset it needed: two consumers of one tool, already drifting. Chess gains
// the numpad, the picker, dev commands, the toaster and the keyboard cursor by deleting code.
//
// ---- Threading ----
// Input arrives on the input/UI thread (Tap/Key/Scroll) and is consumed inside Draw on the render
// thread, where every rect is known. That is not an optimisation: the hit-test MUST run where the
// layout is, or it tests coordinates against last frame's geometry. Handover is by atomics — one
// pending tap and a small key ring, so a dev tool drops a duplicate keystroke rather than lock.
//
// CVar values are read straight from the registry. ValueString/RawValue are guard-free and the
// global value is only ever mutated HERE, never by a sim thread, so these reads are race-free.
//
// ---- Not shipped: the CLASS ITSELF does not exist in a shipping build ----
// Not merely its definitions. A shipping CVar has zero members, returns its constexpr default and is
// not in any registry — Lur::Core::ICVar is itself `#if !LUR_SHIPPING`. So there is nothing for a
// console to enumerate and no type to enumerate it with, and guarding only the .cpp would leave a
// class declaration that cannot compile.
//
// Consequence for a consumer: hold the member and call it under the same `#if !LUR_SHIPPING`, as both
// games do. This is LUR_INTERNAL, not LUR_AGENT — it observes and tunes a session the player is
// driving, and its off switch is the console key itself.
#if !LUR_SHIPPING

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_set>

#include "Lur/Core/CVar.h"
#include "Lur/DevGui/Numpad.h"
#include "Lur/Hud/TextField.h"
#include "Lur/Render/Renderer.h"
#include "Lur/Text/Font.h"

namespace Lur::DevGui {

class Console {
public:
    // Called after an edit lands, so the app can persist it (and, for an AffectsGameplay CVar on a
    // phone, sync it to the peer). EVERY write path goes through it — the numpad's Enter, a bool
    // tap, a picker drag, a keyboard scrub, a row reset — so no edit can be persisted by one path
    // and forgotten by another. That drift is exactly what a single hook prevents.
    using CommitFn = void (*)(void* Ctx, Lur::Core::ICVar& Cv);

    // Builds the console's own materials, gradient meshes and font. It does NOT borrow the game's: a
    // tool that only works inside a game that happens to have an Inter atlas is not an engine
    // facility. The cost is one extra atlas upload in a dev build, which does not ship.
    void CreateResources(Lur::Render::IRenderer* Renderer);

    void SetCommitHook(CommitFn Fn, void* Ctx) { CommitFn_ = Fn; CommitCtx_ = Ctx; }

    // Closing dismisses the open editors. Leaving a numpad bound to a row you can no longer see is
    // how it reappears mid-edit over an unrelated CVar the next time the console opens.
    void SetOpen(bool On);
    bool IsOpen() const { return Open_; }

    // ---- Input (call from wherever the game routes touches and keys) ----
    // A tap/click at framebuffer pixels. Queued; resolved in Draw. The CALLER decides whether the
    // console gets it at all — ask IsOpen() first and keep the tap for the game if it is closed.
    void Tap(float XPx, float YPx);

    // A virtual-key press. Returns true when the console CLAIMED it, which the caller must respect
    // or the key also reaches the game. Claimed only while open — closed, every key is the game's.
    bool Key(uint32_t Vk);

    // Wheel / drag scroll for the row list. Accumulated and folded in at draw time; discarded while
    // closed, so a wheel spin with the console shut does not lurch it on reopen.
    void Scroll(float DeltaY);

    // Draws the console and consumes this frame's input. A no-op while closed, including the
    // BeginDevGui pass — a closed console costs the frame nothing and paints nothing.
    //
    // No DtSec parameter on purpose: the toaster's auto-expiry reads a wall clock the Console owns.
    // A dev tool that touches no sim state may do that, and it means a consumer whose Render has no
    // delta (chess's does not) cannot accidentally pass 0 and leave the toaster up forever.
    void Draw(Lur::Render::IRenderer* Renderer, float WidthPx, float HeightPx);

    // ---- Observation ----
    // The interaction model, readable from outside: which row the editors are bound to, which editor
    // is up, what the toaster says. Tests drive taps and assert on these rather than on pixels, and
    // these are the same fields the paint reads — so a passing test cannot describe a console that
    // draws something else.
    const Lur::Core::ICVar* SelectedCvar() const { return SelectedCvar_; }
    const Lur::Core::ICVar* HighlightedCvar() const { return HiCvar_; }
    const std::string& HighlightedCategory() const { return HiCat_; }
    bool NumpadOpen() const { return NumpadOpen_; }
    bool PickerOpen() const { return PickerOpen_; }
    const std::string& ToastText() const { return ToastText_; }
    const std::string& NumpadBuffer() const { return Numpad_.Buffer(); }
    float ScrollOffset() const { return ScrollY_; }
    bool IsFolded(const std::string& Path) const { return Folded(Path); }

    // Where a CVar's row is on screen right now, in framebuffer pixels. False when it is folded away
    // or scrolled out of the clip band — in which case it is also neither drawn nor tappable, which
    // is the same answer.
    //
    // Shares the layout and the flattened row list with Draw, so this reports the row you can
    // actually see. It reads the CURRENT scroll without folding in a pending Scroll() delta; that is
    // applied at draw time, where it can be clamped against this frame's content height.
    bool RowRect(const Lur::Core::ICVar* Cv, float WidthPx, float HeightPx, float& OutX,
                 float& OutY, float& OutW, float& OutH) const;

    // The colour picker's metrics for this frame, in framebuffer pixels: EXACTLY the values Draw
    // feeds to Lur::DevGui::ColorPicker. Ask ColorPicker::HueRect/SquareRect/AlphaRect with these and
    // you get the same rects the console painted and hit-tested — which is the only way to point at a
    // strip from outside without re-deriving nine constants that would then drift.
    // False when the picker is not open.
    struct PickerGeom {
        float X = 0.0f, Y = 0.0f, W = 0.0f;
        float SwatchH = 0.0f, SquareH = 0.0f, StripH = 0.0f, ReadoutH = 0.0f;
        float Gap = 0.0f, KnobW = 0.0f, PanelH = 0.0f;
        float CancelX = 0.0f, CancelY = 0.0f, CancelS = 0.0f;
    };
    bool PickerGeometry(float WidthPx, float HeightPx, PickerGeom& Out) const;

    // The picker's WORKING hue/saturation/value while it is open. Observable because it is the state
    // the correctness rule is about: it must survive our own writes and be re-derived only on an
    // external edit. A picker that re-derives every frame snaps the hue handle to red the instant a
    // drag reaches the black or white edge of the square, and the CVar's RGB cannot show that.
    float PickerHue() const { return PickH_; }
    float PickerSat() const { return PickS_; }
    float PickerVal() const { return PickV_; }

    // The console's metric scale for a framebuffer width (1.0 at 360 px, linear above). The console
    // sizes itself rather than taking the game's HUD scale: it is the same tool on a phone and in a
    // desktop window, and it must not change size because a game picked a different baseline.
    static float Scale(float WidthPx);

private:
    // Everything derived from the framebuffer size, computed once per query and shared by the paint,
    // the hit-test and RowRect. Three separate answers to "where does the row band start" is how a
    // console ends up editing a different row from the one you touched.
    struct Layout {
        float HS = 1.0f;
        float X0 = 0.0f, Y0 = 0.0f, PW = 0.0f, PH = 0.0f;    // the panel
        float TitleH = 0.0f, LineH = 0.0f, CatH = 0.0f, IndentW = 0.0f;
        float CmdStripY = 0.0f, CmdStripH = 0.0f;            // the DevCommand button strip
        float ViewTop = 0.0f, ViewBot = 0.0f, ViewH = 0.0f;  // the scrollable content band
        float RowPad = 0.0f, ValX = 0.0f, ValW = 0.0f, ResetX = 0.0f, ResetS = 0.0f;
    };
    static Layout Lay(float WidthPx, float HeightPx);
    // The picker's metrics from the layout plus the screen Y of the row it hangs off. Static and
    // pure, so the paint, the hit-test and PickerGeometry() cannot answer differently.
    static PickerGeom PickerLayout(const Layout& L, float AnchorRowY, float WidthPx,
                                   float HeightPx);
    // Screen Y of a CVar's row this frame, or Fallback when it is not in the flattened list. Defined
    // out of line so FlatList stays out of this header.
    float AnchorRowYFor(const Lur::Core::ICVar* Cv, const Layout& L, float Fallback) const;

    void Blit(Lur::Render::IRenderer* R, Lur::Render::MaterialHandle Mat, float Cx, float Cy,
              float Wpx, float Hpx) const;
    void Commit(Lur::Core::ICVar& Cv) {
        if (CommitFn_) CommitFn_(CommitCtx_, Cv);
    }
    bool Folded(const std::string& Path) const {
        return CollapsedCats_.find(Path) != CollapsedCats_.end();
    }
    // Bind an editor to a row: the picker for a colour, the numpad for anything else. One place, so
    // the tap path and the keyboard path cannot disagree about which editor a type gets.
    void OpenEditorFor(Lur::Core::ICVar* Cv);

    // ---- Input handover: written on the input thread, drained in Draw ----
    std::atomic<float> TapX_{-1.0e9f};
    std::atomic<float> TapY_{-1.0e9f};
    std::atomic<bool>  TapPending_{false};
    std::atomic<float> ScrollAccum_{0.0f};
    static constexpr int KeyCap = 32;
    std::atomic<uint32_t> Keys_[KeyCap] = {};
    std::atomic<int>      KeyCount_{0};

    // ---- Model ----
    bool Open_ = false;
    // The OPEN EDITOR's target. Distinct from the keyboard highlight below: arrows move the
    // highlight, Enter promotes it to the editor, and the two differ while you arrow away from an
    // open numpad. Both deserve to be visible when they do.
    Lur::Core::ICVar* SelectedCvar_ = nullptr;
    // The keyboard cursor, held as a KEY (a CVar pointer, or a category PATH) and never an index:
    // the row list is rebuilt every frame and its indices shift the instant a category folds.
    Lur::Core::ICVar* HiCvar_ = nullptr;
    std::string       HiCat_;
    std::unordered_set<std::string> CollapsedCats_;   // folded headers, keyed by FULL path
    float ScrollY_ = 0.0f;

    Numpad Numpad_;
    bool   NumpadOpen_ = false;

    bool  PickerOpen_ = false;
    // The picker's working HSV, authoritative while it is open, because RGB->HSV cannot recover a
    // hue at S==0 or V==0 — re-deriving every frame snaps the handle to red the moment you drag into
    // the white or black edge of the square. PickWrote_ is how we tell OUR write from an external
    // one (a numpad edit, a row reset) and re-derive only for the latter.
    float PickH_ = 0.0f, PickS_ = 0.0f, PickV_ = 0.0f, PickA_ = 1.0f;
    Lur::Core::ICVar* PickBound_ = nullptr;
    float PickWrote_[4] = {0, 0, 0, 0};

    Lur::Core::ICVar* ToastCvar_ = nullptr;   // the row it is anchored to (null = a command result)
    std::string       ToastText_;             // "" = no toaster
    uint64_t          ToastShownMs_ = 0;      // wall clock at open; see Draw's note on DtSec

    CommitFn CommitFn_ = nullptr;
    void*    CommitCtx_ = nullptr;

    // ---- Resources ----
    Lur::Render::MaterialHandle PanelMat_ = 0;    // charcoal translucent surface
    Lur::Render::MaterialHandle KeyMat_ = 0;      // raised control face
    Lur::Render::MaterialHandle AccentMat_ = 0;   // selection marker / scroll thumb / Enter key
    Lur::Render::MaterialHandle WhiteMat_ = 0;    // flat white: SV base, knobs, reticle
    Lur::Render::MaterialHandle SwatchMat_ = 0;   // the picker's own swatch, retinted each frame
    Lur::Render::MaterialHandle HueMat_ = 0;      // retinted with the LIVE hue
    Lur::Render::MaterialHandle AlphaMat_ = 0;    // retinted with the live RGB for the alpha strip
    // A RING of swatch materials for colour rows, retinted per visible row — not one per row per
    // frame: CreateMaterial allocates a descriptor set and grows a vector nothing reclaims, so
    // creating inside a draw loop exhausts the pool.
    static constexpr int RowSwatchCount = 16;
    Lur::Render::MaterialHandle RowSwatchMat_[RowSwatchCount] = {};
    int ColorRowsDrawn_ = 0;

    Lur::Render::MeshHandle QuadMesh_ = 0;
    Lur::Render::MeshHandle SvSatMesh_ = 0;      // white, alpha ramping 0->1 across X (saturation)
    Lur::Render::MeshHandle SvValMesh_ = 0;      // black, alpha ramping 0->1 down Y (value)
    Lur::Render::MeshHandle HueStripMesh_ = 0;   // 6 segments round the wheel, per-vertex coloured

    Lur::Text::Font     Font_;
    Lur::Hud::TextField Text_;
    bool                Ready_ = false;
};

}  // namespace Lur::DevGui

#endif  // !LUR_SHIPPING
