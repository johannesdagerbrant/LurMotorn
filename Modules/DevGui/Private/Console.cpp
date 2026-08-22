#include "Lur/DevGui/Console.h"

// The whole file is dev-only. Not a runtime toggle: in a shipping build these symbols do not exist,
// so nothing a player holds can reach the console even by accident. Games hold the member and call
// it under the same guard.
#if !LUR_SHIPPING

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Lur/Core/DevCommand.h"       // #116: commands rendered as buttons
#include "Lur/DevGui/CategoryTree.h"   // #121: hierarchical category tree
#include "Lur/DevGui/ColorMath.h"      // RGB <-> HSV
#include "Lur/DevGui/ColorPicker.h"    // #117/#174: the RGBA picker's geometry + hit-test
#include "Lur/DevGui/CvarTree.h"       // registry -> (category, cvar), name-sorted
#include "Lur/DevGui/DevTheme.h"       // #113: the one home for dev-layer colours
#include "Lur/DevGui/FlatList.h"       // fold-aware row list + scroll clamp
#include "Lur/DevGui/Popover.h"        // below-or-above anchored placement
#include "Lur/DevGui/Widgets.h"        // HitRect / Slider
#include "Lur/Math/Mat4.h"
#include "Lur/Render/DevGuiLayer.h"    // BeginDevGuiLayer (shipping-guarded dev pass)
#include "Lur/Render/Mesh2D.h"         // gradient strips
#include "Lur/Render/Sprite2D.h"       // MakeQuad
#include "Lur/Text/BuiltinFonts.h"     // InterFont

namespace Lur::DevGui {
namespace {

using Lur::Math::Mat4;
using Lur::Render::Color;
using Lur::Render::MaterialHandle;

namespace Theme = DevTheme;

uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// The leaf of a dotted name: "rps.sim.cart_speed" -> "cart_speed". Row labels and command buttons
// both want it — the parents are already visible as the tree headers above.
const char* LeafName(const char* Full) {
    const char* Dot = std::strrchr(Full, '.');
    return Dot ? Dot + 1 : Full;
}

constexpr float ToastSeconds = 6.0f;

}  // namespace

float Console::Scale(float WidthPx) {
    // Baseline is the 360 px desktop window, so a 1080-wide phone gets 3x text and controls. Floored
    // at 1 — a window narrower than the baseline should not shrink the tool into illegibility.
    const float S = WidthPx / 360.0f;
    return S < 1.0f ? 1.0f : S;
}

Console::Layout Console::Lay(float WidthPx, float HeightPx) {
    Layout L;
    L.HS = Scale(WidthPx);
    const float Pad = 8.0f * L.HS;
    L.LineH = 20.0f * L.HS;
    L.CatH = 22.0f * L.HS;
    L.TitleH = 26.0f * L.HS;
    L.IndentW = 12.0f * L.HS;   // per depth level
    // A fixed panel viewport: its height is the SCREEN, not the content, so an arbitrary number of
    // CVars fits and the rows scroll inside it.
    L.X0 = 2.0f * Pad;
    L.Y0 = HeightPx * 0.06f;
    L.PW = WidthPx - 4.0f * Pad;
    L.PH = HeightPx * 0.86f;
    // The DevCommand strip sits between the title and the row list; the content band starts BELOW
    // it, or the first rows would be drawn under the buttons and hit-test against them.
    L.CmdStripH = 18.0f * L.HS;
    L.CmdStripY = L.Y0 + L.TitleH + 2.0f * L.HS;
    L.ViewTop = L.CmdStripY + L.CmdStripH + 4.0f * L.HS;
    L.ViewBot = L.Y0 + L.PH - 4.0f * L.HS;
    L.ViewH = L.ViewBot - L.ViewTop;
    // Row columns, right-aligned to a constant edge so values read down a true column.
    L.RowPad = 4.0f * L.HS;
    L.ResetS = L.LineH - 4.0f * L.HS;
    L.ResetX = L.X0 + L.PW - L.RowPad - L.ResetS;
    L.ValW = 90.0f * L.HS;
    L.ValX = L.ResetX - 6.0f * L.HS - L.ValW;
    return L;
}

Console::PickerGeom Console::PickerLayout(const Layout& L, float AnchorRowY, float WidthPx,
                                          float HeightPx) {
    PickerGeom G;
    G.SwatchH = 26.0f * L.HS;
    G.StripH = 18.0f * L.HS;
    G.ReadoutH = 16.0f * L.HS;
    G.Gap = 6.0f * L.HS;
    G.KnobW = 10.0f * L.HS;
    G.W = WidthPx * 0.62f;
    G.SquareH = G.W * 0.72f;
    G.X = (WidthPx - G.W) * 0.5f;
    G.PanelH = ColorPicker::PanelH(G.SwatchH, G.SquareH, G.StripH, G.ReadoutH, G.Gap);
    // Hangs off the selected row, flipped above it when it would not fit below.
    G.Y = PlaceBelowOrAbove(AnchorRowY, L.LineH, G.PanelH + 10.0f * L.HS, G.Gap, HeightPx) +
          5.0f * L.HS;
    // Cancel sits where the numpad's does, so dismissing either editor is the same gesture.
    G.CancelS = G.SwatchH * 0.72f;
    G.CancelX = G.X + G.W + 6.0f * L.HS;
    G.CancelY = G.Y + (G.SwatchH - G.CancelS) * 0.5f;
    return G;
}

void Console::Blit(Lur::Render::IRenderer* R, MaterialHandle Mat, float Cx, float Cy, float Wpx,
                   float Hpx) const {
    // Centres a Wpx x Hpx quad at screen (Cx, Cy) — the whole console is expressed in centres, so
    // the draw call and the DevGui rect helpers agree without every site converting.
    const Mat4 M = Mat4::Translation({Cx - Wpx * 0.5f, Cy - Hpx * 0.5f, 0.0f}) *
                   Mat4::Scale({Wpx, Hpx, 1.0f});
    R->DrawMesh(QuadMesh_, Mat, M);
}

void Console::CreateResources(Lur::Render::IRenderer* Renderer) {
    if (Renderer == nullptr) return;

    const Lur::Render::Quad Q = Lur::Render::MakeQuad({1.0f, 1.0f, 1.0f, 1.0f});
    QuadMesh_ = Renderer->CreateMesh(Q.Vertices, 4, Q.Indices, 6);

    PanelMat_ = Lur::Render::MakeFlatMaterial(Renderer, Theme::Panel);
    AccentMat_ = Lur::Render::MakeFlatMaterial(Renderer, Theme::AccentFill);
    KeyMat_ = Lur::Render::MakeFlatMaterial(Renderer, Theme::KeyFace);
    WhiteMat_ = Lur::Render::MakeFlatMaterial(Renderer, Color{1.0f, 1.0f, 1.0f, 1.0f});
    SwatchMat_ = Lur::Render::MakeFlatMaterial(Renderer, Color{1.0f, 1.0f, 1.0f, 1.0f});
    for (int I = 0; I < RowSwatchCount; ++I)
        RowSwatchMat_[I] = Lur::Render::MakeFlatMaterial(Renderer, Color{1.0f, 1.0f, 1.0f, 1.0f});

    // The SV square is three layers, which is what makes it exactly S(across) x V(down): a white
    // quad, then a white->transparent ramp TINTED with the live hue (saturation), then a black ramp
    // down Y (value). The shader does OutColor = InColor * Tint, so per-vertex alpha plus a retinted
    // material gives the hue ramp with no mesh rebuild. Get the layer order wrong and you get a
    // picker that looks plausible and lies about saturation.
    using Lur::Render::GradStop;
    const GradStop SatStops[2] = {{0.0f, {1.0f, 1.0f, 1.0f, 0.0f}},
                                  {1.0f, {1.0f, 1.0f, 1.0f, 1.0f}}};
    SvSatMesh_ = Lur::Render::MakeGradientStripH(Renderer, SatStops, 2);
    const GradStop ValStops[2] = {{0.0f, {0.0f, 0.0f, 0.0f, 0.0f}},
                                  {1.0f, {0.0f, 0.0f, 0.0f, 1.0f}}};
    SvValMesh_ = Lur::Render::MakeGradientStripV(Renderer, ValStops, 2, 1.0f);
    // Hue strip: six segments round the wheel, per-vertex coloured. SEVEN stops so the last segment
    // closes back on red — a strip that ends at magenta reads as a bug.
    GradStop HueStops[7];
    for (int I = 0; I < 7; ++I) {
        const float H = static_cast<float>(I) / 6.0f;
        float R2 = 0, G2 = 0, B2 = 0;
        ColorMath::HueColor(H, R2, G2, B2);
        HueStops[I] = {H, {R2, G2, B2, 1.0f}};
    }
    HueStripMesh_ = Lur::Render::MakeGradientStripH(Renderer, HueStops, 7);
    HueMat_ = Lur::Render::MakeFlatMaterial(Renderer, Color{1.0f, 0.0f, 0.0f, 1.0f});
    AlphaMat_ = Lur::Render::MakeFlatMaterial(Renderer, Color{1.0f, 1.0f, 1.0f, 1.0f});

    Font_.Init(Lur::Text::InterFont());
    Font_.UploadAtlas(*Renderer);
    Text_.CreateResources(Renderer, &Font_);

    Ready_ = true;
}

void Console::SetOpen(bool On) {
    Open_ = On;
    if (!On) {
        // Dismiss the editors. A numpad still bound to a row you can no longer see reappears
        // mid-edit over an unrelated CVar the next time the console opens.
        Numpad_.Clear();
        NumpadOpen_ = false;
        PickerOpen_ = false;
        ToastText_.clear();
        ToastCvar_ = nullptr;
    }
}

void Console::Tap(float XPx, float YPx) {
    TapX_.store(XPx, std::memory_order_relaxed);
    TapY_.store(YPx, std::memory_order_relaxed);
    TapPending_.store(true, std::memory_order_release);  // consumed on the render thread
}

bool Console::Key(uint32_t Vk) {
    // Claim ONLY while showing. Closed, every key belongs to the game and this must not swallow it —
    // that is the "input flows to the game unchanged when it is closed" half of the contract.
    if (!Open_) return false;
    const int N = KeyCount_.load(std::memory_order_relaxed);
    if (N < KeyCap) {
        Keys_[N].store(Vk, std::memory_order_relaxed);
        KeyCount_.store(N + 1, std::memory_order_release);
    }
    // Claimed even when the ring is full: the console is open, so the game must not see this key
    // regardless of whether we had room to act on it.
    return true;
}

void Console::Scroll(float DeltaY) { ScrollAccum_.fetch_add(DeltaY, std::memory_order_relaxed); }

void Console::OpenEditorFor(Lur::Core::ICVar* Cv) {
    SelectedCvar_ = Cv;
    Numpad_.Clear();
    // A colour gets the picker; everything else gets the numpad. One place, so the tap path and the
    // keyboard path cannot disagree about which editor a type gets.
    NumpadOpen_ = !Cv->IsColor();
    PickerOpen_ = Cv->IsColor();
}

bool Console::RowRect(const Lur::Core::ICVar* Cv, float WidthPx, float HeightPx, float& OutX,
                      float& OutY, float& OutW, float& OutH) const {
    if (!Open_ || Cv == nullptr) return false;
    const Layout L = Lay(WidthPx, HeightPx);
    const auto Root = BuildCategoryTree(GatherCvars('.'), '.');
    std::vector<FlatRow<Lur::Core::ICVar*>> Vis;
    FlattenTree(Root, [this](const std::string& P) { return Folded(P); }, L.LineH, L.CatH, Vis);
    for (const auto& It : Vis) {
        if (It.IsCategory || It.Item != Cv) continue;
        const float Sy = L.ViewTop - ScrollY_ + It.ContentY;
        // Culled rows are neither drawn nor tappable, so "not visible" is the honest answer rather
        // than an off-screen rect a caller might tap into.
        if (Sy + L.LineH <= L.ViewTop || Sy >= L.ViewBot) return false;
        OutX = L.X0 + static_cast<float>(It.Depth) * L.IndentW;
        OutY = Sy;
        OutW = L.X0 + L.PW - OutX;
        OutH = L.LineH;
        return true;
    }
    return false;
}

float Console::AnchorRowYFor(const Lur::Core::ICVar* Cv, const Layout& L, float Fallback) const {
    const auto Root = BuildCategoryTree(GatherCvars('.'), '.');
    std::vector<FlatRow<Lur::Core::ICVar*>> Vis;
    FlattenTree(Root, [this](const std::string& P) { return Folded(P); }, L.LineH, L.CatH, Vis);
    return RowScreenY(Vis, const_cast<Lur::Core::ICVar*>(Cv), L.ViewTop, ScrollY_, Fallback);
}

bool Console::PickerGeometry(float WidthPx, float HeightPx, PickerGeom& Out) const {
    if (!Open_ || !PickerOpen_ || SelectedCvar_ == nullptr || !SelectedCvar_->IsColor()) return false;
    const Layout L = Lay(WidthPx, HeightPx);
    Out = PickerLayout(L, AnchorRowYFor(SelectedCvar_, L, HeightPx * 0.45f), WidthPx, HeightPx);
    return true;
}

void Console::Draw(Lur::Render::IRenderer* Renderer, float WidthPx, float HeightPx) {
    if (!Open_ || !Ready_ || Renderer == nullptr) {
        // Discard input queued while hidden: a tap that arrived before the console opened must not
        // fire into a layout it never saw, and a wheel spin must not lurch the list on reopen.
        TapPending_.store(false, std::memory_order_relaxed);
        ScrollAccum_.store(0.0f, std::memory_order_relaxed);
        KeyCount_.store(0, std::memory_order_relaxed);
        return;
    }

    Lur::Render::BeginDevGuiLayer(Renderer);

    const Layout L = Lay(WidthPx, HeightPx);
    const float HS = L.HS;
    const Color Accent = Theme::Accent;
    const Color Ink = Theme::Ink;
    const Color CatInk = Theme::CatInk;
    const Color DimInk = Theme::DimInk;

    // Split on '.' — the dotted CVar name IS the category hierarchy, so there is no separate
    // category field that can disagree with what a knob is called.
    const auto Root = BuildCategoryTree(GatherCvars('.'), '.');
    const int Count = Root.TotalLeaves;

    // One flattened array (honouring folds) feeds painting, culling, hit-testing AND the anchored
    // popovers. A console where the row you tap is not the row you see is the classic dev-UI bug,
    // and sharing this array is what makes it impossible.
    using VItem = FlatRow<Lur::Core::ICVar*>;
    std::vector<VItem> Vis;
    const float ContentH = FlattenTree(
        Root, [this](const std::string& P) { return Folded(P); }, L.LineH, L.CatH, Vis);

    ScrollY_ += ScrollAccum_.exchange(0.0f, std::memory_order_relaxed);
    const float MaxScroll = ClampScroll(ScrollY_, ContentH, L.ViewH);

    // A CVar's row screen-Y this frame, for anchoring the numpad/picker/toaster. Falls back to
    // mid-viewport when the row is folded or scrolled away.
    auto RowY = [&](Lur::Core::ICVar* Which, float Fallback) {
        return RowScreenY(Vis, Which, L.ViewTop, ScrollY_, Fallback);
    };

    // ---- Panel, title, close box ----
    Blit(Renderer, PanelMat_, L.X0 + L.PW * 0.5f, L.Y0 + L.PH * 0.5f, L.PW, L.PH);
    const float XbtnS = L.TitleH;
    const float XbtnX = L.X0 + L.PW - XbtnS;
    Blit(Renderer, KeyMat_, XbtnX + XbtnS * 0.5f, L.Y0 + XbtnS * 0.5f, XbtnS, XbtnS);
    Text_.Draw(Renderer, "X", XbtnX, L.Y0, XbtnS, XbtnS, 16.0f * HS, Accent,
               Lur::Text::EHAlign::Center, Lur::Text::EVAlign::Middle);
    char T[96];
    std::snprintf(T, sizeof(T), "Console  %d cvars", Count);
    Text_.Draw(Renderer, T, L.X0 + 10.0f * HS, L.Y0 + 3.0f * HS, L.PW - XbtnS - 18.0f * HS,
               L.TitleH, 14.0f * HS, Accent);

    // Commit the numpad buffer to the selected CVar, then dismiss the pad. Shared by the on-screen
    // Enter key and the physical one so the two entry paths cannot drift — they are the same widget
    // reached by two devices.
    auto CommitNumpad = [&]() {
        if (SelectedCvar_ != nullptr && !Numpad_.Buffer().empty() &&
            SelectedCvar_->SetFromString(Numpad_.Buffer().c_str()))
            Commit(*SelectedCvar_);
        Numpad_.Clear();
        NumpadOpen_ = false;
    };

    // ---- The keyboard cursor: a highlight over the flattened row list ----
    auto HighlightIndex = [&]() -> int {
        if (HiCvar_ != nullptr) return FindLeafRow(Vis, HiCvar_);
        return FindCategoryRow(Vis, HiCat_);
    };
    auto SetHighlight = [&](const VItem& It) {
        if (!It.IsCategory) {
            HiCvar_ = It.Item;
            HiCat_.clear();
        } else {
            HiCvar_ = nullptr;
            HiCat_ = It.Node->Path;
        }
        // Follow the cursor with the viewport. Without this, Down walks the highlight off the bottom
        // of the clip band and keeps going invisibly — the list appears frozen and then jumps
        // several rows when you finally press Enter.
        ScrollToReveal(It.ContentY, It.IsCategory ? L.CatH : L.LineH, ScrollY_, L.ViewH, MaxScroll);
    };
    auto MoveHighlight = [&](int Delta) {
        const int I = StepIndex(HighlightIndex(), Delta, static_cast<int>(Vis.size()));
        if (I >= 0) SetHighlight(Vis[static_cast<std::size_t>(I)]);
    };
    // Enter with no editor open: act on whatever is highlighted.
    auto OpenHighlighted = [&]() {
        if (!HiCat_.empty()) {   // a category folds/unfolds — its only meaningful action
            if (Folded(HiCat_)) CollapsedCats_.erase(HiCat_);
            else CollapsedCats_.insert(HiCat_);
            return;
        }
        if (HiCvar_ == nullptr) return;
        if (HiCvar_->IsBool()) {
            // A bool has no editor to open — a numpad for a two-state knob is a keypad to type "1"
            // into. Enter toggles it in place, matching what a TAP on a bool row does.
            HiCvar_->SetFromString(HiCvar_->RawValue() != 0 ? "false" : "true");
            Commit(*HiCvar_);
            return;
        }
        OpenEditorFor(HiCvar_);
    };
    auto ScrubHighlighted = [&](int Steps) {
        // Left/Right move the LIVE value. ICVar::Nudge picks the step from the type and declines on
        // an enum, which leaves the value untouched and uncommitted.
        if (HiCvar_ == nullptr) return;
        if (HiCvar_->Nudge(Steps)) Commit(*HiCvar_);
    };

    // ---- Physical keyboard — desktop in practice, ADDITIVE to the on-screen pad ----
    // Nothing here draws: the console renders identically on both platforms, and a phone simply
    // never queues a key. Drained BEFORE the taps so a keystroke and a click in the same frame
    // resolve in the order a human would expect.
    {
        const int NKeys = KeyCount_.load(std::memory_order_acquire);
        for (int K = 0; K < NKeys; ++K) {
            const uint32_t Vk = Keys_[K].load(std::memory_order_relaxed);
            // Digits come from BOTH the top row (VK_0..VK_9) and the numpad cluster
            // (VK_NUMPAD0..VK_NUMPAD9) — a tuner using the number pad one-handed is the whole
            // point, so accepting only one of the two would miss the common case.
            char Ch = '\0';
            if (Vk >= 0x30 && Vk <= 0x39)      Ch = static_cast<char>('0' + (Vk - 0x30));
            else if (Vk >= 0x60 && Vk <= 0x69) Ch = static_cast<char>('0' + (Vk - 0x60));
            else if (Vk == 0xBE || Vk == 0x6E) Ch = '.';   // VK_OEM_PERIOD / VK_DECIMAL
            else if (Vk == 0xBD || Vk == 0x6D) Ch = '-';   // VK_OEM_MINUS / VK_SUBTRACT

            if (Ch != '\0') {
                // Typing on a highlighted numeric row opens the pad without a separate Enter, so
                // "arrow to the row, then type" flows.
                if (!NumpadOpen_ && !PickerOpen_ && HiCvar_ != nullptr && !HiCvar_->IsBool() &&
                    !HiCvar_->IsColor()) {
                    SelectedCvar_ = HiCvar_;
                    Numpad_.Clear();
                    NumpadOpen_ = true;
                }
                if (NumpadOpen_) Numpad_.Press(Ch);
            } else if (Vk == 0x0D) {            // VK_RETURN (the pad's Enter is the same VK)
                if (NumpadOpen_)      CommitNumpad();
                else if (PickerOpen_) PickerOpen_ = false;   // Enter closes the picker too
                else                  OpenHighlighted();
            } else if (Vk == 0x1B) {            // VK_ESCAPE — dismiss without committing
                Numpad_.Clear();
                NumpadOpen_ = false;
                PickerOpen_ = false;
            } else if (Vk == 0x08) {            // VK_BACK
                if (NumpadOpen_) Numpad_.Backspace();
            } else if (Vk == 0x26 || Vk == 0x28) {           // VK_UP / VK_DOWN
                MoveHighlight(Vk == 0x26 ? -1 : +1);         // MOVE, don't scrub
            } else if (Vk == 0x25 || Vk == 0x27) {           // VK_LEFT / VK_RIGHT
                ScrubHighlighted(Vk == 0x27 ? +1 : -1);      // scrub the value
            }
        }
        if (NKeys > 0) KeyCount_.store(0, std::memory_order_release);
    }

    // Taps are consumed on THIS thread, where every rect is laid out, so hit-test and edits cannot
    // race the ValueString reads above.
    const bool TapPending = TapPending_.load(std::memory_order_acquire);
    const float TapX = TapX_.load(std::memory_order_relaxed);
    const float TapY = TapY_.load(std::memory_order_relaxed);
    bool TapUsed = false;

    // ---- Dev commands as BUTTONS ----
    // Not ergonomics: this is the ONLY way to invoke a DevCommand. Neither platform has text entry —
    // the console edits numbers through a numpad — so a command with no button is a command nobody
    // can run. An evenly-split strip under the title, using the same rect for the draw and the
    // hit-test so a visible button is always pressable.
    {
        int CmdCount = 0;
        Lur::Core::DevCommandRegistry::ForEach([&](Lur::Core::DevCommand*) { ++CmdCount; });
        if (CmdCount > 0) {
            const float CbW = (L.PW - 8.0f * HS) / static_cast<float>(CmdCount);
            int Idx = 0;
            Lur::Core::DevCommandRegistry::ForEach([&](Lur::Core::DevCommand* C) {
                const float Cbx = L.X0 + 4.0f * HS + static_cast<float>(Idx) * CbW;
                const float Cby = L.CmdStripY, Cbh = L.CmdStripH;
                const float Cbw = CbW - 4.0f * HS;
                if (TapPending && !TapUsed && HitRect(Cbx, Cby, Cbw, Cbh, TapX, TapY)) {
                    std::string Out;
                    C->Run(Lur::Core::DevArgs{}, Out);
                    // The console has no scrollback pane, so the result goes to the toaster the "i"
                    // button already uses — one output surface, not a second one invented for
                    // commands.
                    ToastText_ = Out.empty() ? std::string(C->Name()) + ": done" : Out;
                    ToastCvar_ = nullptr;
                    ToastShownMs_ = NowMs();
                    TapUsed = true;
                }
                Blit(Renderer, KeyMat_, Cbx + Cbw * 0.5f, Cby + Cbh * 0.5f, Cbw, Cbh);
                // Leaf name only — the category prefix is redundant on a strip this small, and the
                // full name is what the result line reports.
                Text_.Draw(Renderer, LeafName(C->Name()), Cbx, Cby, Cbw, Cbh, 11.0f * HS, Accent,
                           Lur::Text::EHAlign::Center, Lur::Text::EVAlign::Middle);
                ++Idx;
            });
        }
    }

    // A visible toaster is modal-lite: the next tap ANYWHERE dismisses it (and is consumed), so it
    // cannot also trigger a row underneath. Auto-expires on the wall clock.
    if (!ToastText_.empty() &&
        NowMs() - ToastShownMs_ > static_cast<uint64_t>(ToastSeconds * 1000.0f)) {
        ToastText_.clear();
        ToastCvar_ = nullptr;
    }
    if (TapPending && !TapUsed && !ToastText_.empty()) {
        ToastText_.clear();
        ToastCvar_ = nullptr;
        TapUsed = true;
    }

    // Top-right X closes the console.
    if (TapPending && !TapUsed && HitRect(XbtnX, L.Y0, XbtnS, L.TitleH, TapX, TapY)) {
        SetOpen(false);
        TapPending_.store(false, std::memory_order_release);
        return;
    }

    // ---- Numpad geometry — anchored BELOW the selected row, flipped above when it would not fit --
    const float NumW = WidthPx * 0.62f;
    const float NumX = (WidthPx - NumW) * 0.5f;
    float NumH = NumW;
    if (NumH > HeightPx * 0.42f) NumH = HeightPx * 0.42f;
    const float NumGap = 8.0f * HS;
    const float NumY = PlaceBelowOrAbove(RowY(SelectedCvar_, HeightPx * 0.45f), L.LineH,
                                         NumH + 10.0f * HS, NumGap, HeightPx) + 5.0f * HS;
    // Cancel: a small 4th button just right of the top row (1 2 3 -> x) that closes the numpad
    // WITHOUT committing. Derived from the pad's OWN row count — a hardcoded 4 here silently
    // mis-placed the button the moment the layout grew a row.
    const float NumKeyH =
        (NumH - NumGap * (Numpad::Rows - 1)) / static_cast<float>(Numpad::Rows);
    const float CancelS = NumKeyH * 0.62f;
    const float CancelX = NumX + NumW + 6.0f * HS;
    const float CancelY = NumY + (NumKeyH - CancelS) * 0.5f;
    if (TapPending && !TapUsed && NumpadOpen_ &&
        HitRect(CancelX, CancelY, CancelS, CancelS, TapX, TapY)) {
        Numpad_.Clear();
        NumpadOpen_ = false;
        TapUsed = true;   // discard, no write
    }
    if (TapPending && !TapUsed && NumpadOpen_ &&
        Numpad_.Tap(NumX, NumY, NumW, NumH, NumGap, TapX, TapY)) {
        TapUsed = true;
        if (Numpad_.TakeEnter()) CommitNumpad();
    }

    // ---- Colour picker popover — same anchor and dismissal as the numpad ----
    using Picker = ColorPicker;
    const PickerGeom PG = PickerLayout(L, RowY(SelectedCvar_, HeightPx * 0.45f), WidthPx, HeightPx);

    if (PickerOpen_ && SelectedCvar_ != nullptr && SelectedCvar_->IsColor()) {
        float Ch[4] = {0, 0, 0, 1};
        SelectedCvar_->GetColorChannels(Ch);

        // Re-derive HSV ONLY when the binding changed (the picker just opened on this row) or the
        // value moved from OUTSIDE us (a numpad edit, the row's R button). Deriving every frame
        // would fight the drag: RGB->HSV cannot recover a hue at S==0 or V==0, so sliding into the
        // white or black edge of the square would snap the hue handle back to red.
        const bool Rebind = (PickBound_ != SelectedCvar_);
        const bool External = Ch[0] != PickWrote_[0] || Ch[1] != PickWrote_[1] ||
                              Ch[2] != PickWrote_[2] || Ch[3] != PickWrote_[3];
        if (Rebind || External) {
            ColorMath::RgbToHsv(Ch[0], Ch[1], Ch[2], PickH_, PickS_, PickV_);
            PickA_ = Ch[3];
            PickBound_ = SelectedCvar_;
            for (int I = 0; I < 4; ++I) PickWrote_[I] = Ch[I];
        }

        if (TapPending && !TapUsed &&
            HitRect(PG.CancelX, PG.CancelY, PG.CancelS, PG.CancelS, TapX, TapY)) {
            PickerOpen_ = false;
            TapUsed = true;   // dismiss; edits already committed live
        }
        if (TapPending && !TapUsed) {
            float A = 0.0f, B = 0.0f;
            const Picker::EHit Hit =
                Picker::Hit(PG.X, PG.Y, PG.W, PG.SwatchH, PG.SquareH, PG.StripH, PG.Gap,
                            PG.KnobW, TapX, TapY, A, B);
            if (Hit != Picker::EHit::None) {
                if (Hit == Picker::EHit::SvSquare)      { PickS_ = A; PickV_ = B; }
                else if (Hit == Picker::EHit::HueStrip) { PickH_ = A; }
                else                                    { PickA_ = A; }
                // Write RGBA out and commit LIVE, down the same hook the numpad's Enter uses: a
                // colour you must press Enter to see is a colour you cannot tune. H,S,V stay ours —
                // this is the write side of the working-state rule.
                float R2 = 0, G2 = 0, B2 = 0;
                ColorMath::HsvToRgb(PickH_, PickS_, PickV_, R2, G2, B2);
                const float Out[4] = {R2, G2, B2, PickA_};
                SelectedCvar_->SetColorChannels(Out);
                for (int I = 0; I < 4; ++I) PickWrote_[I] = Out[I];
                Commit(*SelectedCvar_);
                TapUsed = true;
            }
        }
    } else if (!PickerOpen_) {
        PickBound_ = nullptr;   // the next open re-derives, even onto the same row
    }

    ColorRowsDrawn_ = 0;   // rewind the swatch ring for this frame's visible rows

    // ---- Draw and hit-test every visible row, culled to the viewport ----
    for (const VItem& It : Vis) {
        const float RowH = It.IsCategory ? L.CatH : L.LineH;
        const float Sy = L.ViewTop - ScrollY_ + It.ContentY;
        if (Sy + RowH <= L.ViewTop || Sy >= L.ViewBot) continue;   // fully outside the band
        const float IndentX = L.X0 + static_cast<float>(It.Depth) * L.IndentW;
        // Clip taps to the band as well as the draw, or a press on the title or the command strip
        // would also land on whichever row happens to be scrolled under it.
        const bool InBand = TapY >= L.ViewTop && TapY <= L.ViewBot;

        if (It.IsCategory) {   // ---- a category header: tap toggles the fold ----
            const bool Collapsed = Folded(It.Node->Path);
            if (TapPending && !TapUsed && InBand && TapX >= IndentX && TapX <= L.X0 + L.PW &&
                TapY >= Sy && TapY <= Sy + L.CatH) {
                if (Collapsed) CollapsedCats_.erase(It.Node->Path);
                else CollapsedCats_.insert(It.Node->Path);
                // A tap moves the keyboard cursor here too, so switching between mouse and keyboard
                // mid-session resumes from where you were looking.
                HiCvar_ = nullptr;
                HiCat_ = It.Node->Path;
                TapUsed = true;
            }
            Blit(Renderer, KeyMat_, (IndentX + L.X0 + L.PW) * 0.5f, Sy + L.CatH * 0.5f,
                 (L.X0 + L.PW - IndentX) - 6.0f * HS, L.CatH - 3.0f * HS);
            // Categories are highlightable, so they need the same cursor marker CVar rows get —
            // otherwise arrowing onto one looks like the highlight vanished.
            const bool CatHi = !HiCat_.empty() && It.Node->Path == HiCat_;
            if (CatHi)
                Blit(Renderer, AccentMat_, IndentX + 2.0f * HS, Sy + L.CatH * 0.5f, 3.0f * HS,
                     L.CatH - 5.0f * HS);
            char H[96];
            std::snprintf(H, sizeof(H), "[%c] %s  (%d)", Collapsed ? '+' : '-',
                          It.Node->Segment.c_str(), It.Node->TotalLeaves);
            Text_.Draw(Renderer, H, IndentX + 10.0f * HS, Sy, L.X0 + L.PW - IndentX - 14.0f * HS,
                       L.CatH, 13.0f * HS, CatHi ? Accent : CatInk);
            continue;
        }

        // ---- a CVar row: [ i | name ......... | AG | value | R ] ----
        Lur::Core::ICVar* C = It.Item;
        const bool Overridden = C->Overridden();
        const bool HasTip = C->Tooltip()[0] != '\0';
        const float InfoS = L.LineH - 6.0f * HS;
        const float InfoX = IndentX + L.RowPad;
        const float NameX = InfoX + InfoS + 5.0f * HS;
        // "AG" immediately left of the value for an AffectsGameplay CVar. Now that the tree lists
        // dev knobs alongside sim tunables the difference is not cosmetic: an AG edit is latched,
        // hashed and synced to the peer, a dev one is local and persisted only. The name column
        // yields the width, so unmarked rows are unchanged.
        const bool Ag = C->AffectsGameplay();
        const float AgW = Ag ? 18.0f * HS : 0.0f;
        const float NameW = L.ValX - NameX - 6.0f * HS - AgW;

        if (TapPending && !TapUsed && InBand && HasTip &&
            HitRect(InfoX, Sy, InfoS, L.LineH, TapX, TapY)) {   // "i": open the tooltip toaster
            ToastText_ = C->Tooltip();
            ToastCvar_ = C;
            ToastShownMs_ = NowMs();
            TapUsed = true;
        } else if (TapPending && !TapUsed && InBand &&
                   HitRect(L.ResetX, Sy, L.ResetS, L.LineH, TapX, TapY)) {   // reset -> default
            C->Reset();
            Commit(*C);
            if (C == SelectedCvar_) {
                Numpad_.Clear();
                NumpadOpen_ = false;
            }
            TapUsed = true;
        } else if (TapPending && !TapUsed && InBand && TapX >= IndentX && TapX <= L.X0 + L.PW &&
                   TapY >= Sy && TapY <= Sy + L.LineH) {
            HiCvar_ = C;
            HiCat_.clear();   // mouse and keyboard share one cursor
            if (C->IsBool()) {
                // A bool TOGGLES in place — a numpad for a two-state knob is a keypad to type "1"
                // into. One tap flips it and commits down the same hook every other edit uses, so it
                // persists (and, were a bool ever AffectsGameplay, syncs).
                C->SetFromString(C->RawValue() != 0 ? "false" : "true");
                Commit(*C);
                // Never leave the numpad bound to a row that no longer uses it.
                if (C == SelectedCvar_) {
                    Numpad_.Clear();
                    NumpadOpen_ = false;
                    SelectedCvar_ = nullptr;
                }
                PickerOpen_ = false;
            } else {
                OpenEditorFor(C);
            }
            TapUsed = true;
        }

        // The row reads as current when the keyboard cursor is on it OR it is the open editor's
        // target. They are usually the same row; they differ while you arrow away from an open
        // numpad, and BOTH deserve to be visible when they do.
        const bool Selected = (C == HiCvar_) || (C == SelectedCvar_);
        // "i" button — accent when a tooltip exists, greyed and inert otherwise.
        Blit(Renderer, KeyMat_, InfoX + InfoS * 0.5f, Sy + L.LineH * 0.5f, InfoS, InfoS);
        Text_.Draw(Renderer, "i", InfoX, Sy + (L.LineH - InfoS) * 0.5f, InfoS, InfoS, 12.0f * HS,
                   HasTip ? Accent : DimInk, Lur::Text::EHAlign::Center,
                   Lur::Text::EVAlign::Middle);
        if (Selected)
            Blit(Renderer, AccentMat_, IndentX + 2.0f * HS, Sy + L.LineH * 0.5f, 3.0f * HS,
                 L.LineH - 5.0f * HS);
        // Row label = the leaf name; the parents live in the tree headers above it.
        Text_.Draw(Renderer, LeafName(C->Name()), NameX, Sy, NameW, L.LineH, 12.5f * HS,
                   Selected ? Accent : Ink, Lur::Text::EHAlign::Left, Lur::Text::EVAlign::Middle);
        if (Ag)
            Text_.Draw(Renderer, "AG", L.ValX - AgW, Sy, AgW - 3.0f * HS, L.LineH, 9.5f * HS,
                       DimInk, Lur::Text::EHAlign::Right, Lur::Text::EVAlign::Middle);
        Blit(Renderer, KeyMat_, L.ValX + L.ValW * 0.5f, Sy + L.LineH * 0.5f, L.ValW,
             L.LineH - 4.0f * HS);
        if (C->IsColor()) {
            // A SWATCH where a numeric row draws its value: "0.88 0.31 0.22 1" is not a colour
            // anyone can read, which defeats the point of having the type at all.
            float Cc[4] = {0, 0, 0, 1};
            C->GetColorChannels(Cc);
            const MaterialHandle SwMat = RowSwatchMat_[ColorRowsDrawn_ % RowSwatchCount];
            Renderer->SetMaterialTint(SwMat, {Cc[0], Cc[1], Cc[2], Cc[3]});
            ++ColorRowsDrawn_;
            // Inset so the key plate still frames it — the swatch reads as a value IN the column,
            // not as the column itself.
            Blit(Renderer, SwMat, L.ValX + L.ValW * 0.5f, Sy + L.LineH * 0.5f,
                 L.ValW - 10.0f * HS, L.LineH - 9.0f * HS);
        } else {
            char VS[64];
            // A bool reads as a CHECKBOX, centred, so it is obvious at a glance that the row is a
            // toggle and not a number to type into. ASCII on purpose — the MSDF atlas is cooked from
            // the glyphs we ship, so a ballot-box codepoint is not guaranteed to be in it.
            if (C->IsBool())
                std::snprintf(VS, sizeof(VS), "%s", C->RawValue() != 0 ? "[x]" : "[ ]");
            else if (NumpadOpen_ && C == SelectedCvar_)
                std::snprintf(VS, sizeof(VS), "%s_", Numpad_.Buffer().c_str());
            else
                std::snprintf(VS, sizeof(VS), "%s", C->ValueString().c_str());
            Text_.Draw(Renderer, VS, L.ValX + 5.0f * HS, Sy, L.ValW - 10.0f * HS, L.LineH,
                       12.5f * HS, Overridden ? Accent : Ink,
                       C->IsBool() ? Lur::Text::EHAlign::Center : Lur::Text::EHAlign::Right,
                       Lur::Text::EVAlign::Middle);
        }
        Blit(Renderer, KeyMat_, L.ResetX + L.ResetS * 0.5f, Sy + L.LineH * 0.5f, L.ResetS,
             L.ResetS);
        Text_.Draw(Renderer, "R", L.ResetX, Sy + (L.LineH - L.ResetS) * 0.5f, L.ResetS, L.ResetS,
                   12.0f * HS, Overridden ? Accent : DimInk, Lur::Text::EHAlign::Center,
                   Lur::Text::EVAlign::Middle);
    }

    // Scrollbar indicator (right edge) when the content overflows the viewport.
    if (MaxScroll > 0.0f && ContentH > 0.0f) {
        const float TrackW = 3.0f * HS, TrackX = L.X0 + L.PW - TrackW - 1.0f * HS;
        const float ThumbH = L.ViewH * (L.ViewH / ContentH);
        const float ThumbY = L.ViewTop + (L.ViewH - ThumbH) * (ScrollY_ / MaxScroll);
        Blit(Renderer, AccentMat_, TrackX + TrackW * 0.5f, ThumbY + ThumbH * 0.5f, TrackW, ThumbH);
    }

    // ---- The numpad: a backing panel + the key grid (shared KeyRect geometry) ----
    if (NumpadOpen_) {
        using Pad = Numpad;
        Blit(Renderer, PanelMat_, NumX + NumW * 0.5f, NumY + NumH * 0.5f, NumW + 10.0f * HS,
             NumH + 10.0f * HS);
        for (int R = 0; R < Pad::Rows; ++R) {
            // Enter spans its whole row: draw it ONCE across RowRect instead of per cell, or the
            // label would be stamped in each column of the strip.
            if (R == Pad::EnterRow) {
                float Kx, Ky, Kw, Kh;
                Pad::RowRect(NumX, NumY, NumW, NumH, NumGap, R, Kx, Ky, Kw, Kh);
                Blit(Renderer, AccentMat_, Kx + Kw * 0.5f, Ky + Kh * 0.5f, Kw, Kh);
                Text_.Draw(Renderer, Pad::Label(R, 0), Kx, Ky, Kw, Kh, 15.0f * HS, Theme::AccentInk,
                           Lur::Text::EHAlign::Center, Lur::Text::EVAlign::Middle);
                continue;
            }
            for (int C = 0; C < Pad::Cols; ++C) {
                float Kx, Ky, Kw, Kh;
                Pad::KeyRect(NumX, NumY, NumW, NumH, NumGap, R, C, Kx, Ky, Kw, Kh);
                const char* Lbl = Pad::Label(R, C);
                Blit(Renderer, KeyMat_, Kx + Kw * 0.5f, Ky + Kh * 0.5f, Kw, Kh);
                // "+/-" is three glyphs where the digits are one — shrink it so it fits the key
                // instead of overflowing into its neighbours.
                const float FontPx = (std::strlen(Lbl) > 1 ? 15.0f : 22.0f) * HS;
                Text_.Draw(Renderer, Lbl, Kx, Ky, Kw, Kh, FontPx, Ink,
                           Lur::Text::EHAlign::Center, Lur::Text::EVAlign::Middle);
            }
        }
        // Cancel "x" — small, right of the top row; dismisses without writing.
        Blit(Renderer, KeyMat_, CancelX + CancelS * 0.5f, CancelY + CancelS * 0.5f, CancelS,
             CancelS);
        Text_.Draw(Renderer, "x", CancelX, CancelY, CancelS, CancelS, 12.0f * HS, Theme::Warn,
                   Lur::Text::EHAlign::Center, Lur::Text::EVAlign::Middle);
    }

    // ---- The colour picker: swatch, SV square, hue strip, alpha strip ----
    if (PickerOpen_ && SelectedCvar_ != nullptr && SelectedCvar_->IsColor()) {
        float Ch[4] = {0, 0, 0, 1};
        SelectedCvar_->GetColorChannels(Ch);
        Blit(Renderer, PanelMat_, PG.X + PG.W * 0.5f, PG.Y + PG.PanelH * 0.5f,
             PG.W + 10.0f * HS, PG.PanelH + 10.0f * HS);

        // Swatch — retint ONE material, never create per frame.
        float Sx, Sy2, Sw, Sh;
        Picker::SwatchRect(PG.X, PG.Y, PG.W, PG.SwatchH, Sx, Sy2, Sw, Sh);
        Renderer->SetMaterialTint(SwatchMat_, {Ch[0], Ch[1], Ch[2], Ch[3]});
        Blit(Renderer, SwatchMat_, Sx + Sw * 0.5f, Sy2 + Sh * 0.5f, Sw, Sh);

        // SV square: white base, then the saturation ramp TINTED WITH THE LIVE HUE, then the value
        // ramp. Layer 2's tint is why dragging the hue strip re-colours the square with no mesh
        // rebuild — the vertex shader does InColor * Tint.
        float Qx, Qy, Qw, Qh;
        Picker::SquareRect(PG.X, PG.Y, PG.W, PG.SwatchH, PG.SquareH, PG.Gap, Qx, Qy, Qw, Qh);
        float Hr = 0, Hg = 0, Hb = 0;
        ColorMath::HueColor(PickH_, Hr, Hg, Hb);
        Renderer->SetMaterialTint(HueMat_, {Hr, Hg, Hb, 1.0f});
        const Mat4 SquareXf = Mat4::Translation({Qx, Qy, 0.0f}) * Mat4::Scale({Qw, Qh, 1.0f});
        Blit(Renderer, WhiteMat_, Qx + Qw * 0.5f, Qy + Qh * 0.5f, Qw, Qh);
        Renderer->DrawMesh(SvSatMesh_, HueMat_, SquareXf);
        Renderer->DrawMesh(SvValMesh_, WhiteMat_, SquareXf);
        // Reticle: drawn twice for a light-on-dark outline so it stays visible against both the
        // white and the black corners of the square.
        float Rx2 = 0, Ry2 = 0;
        Picker::SvPoint(Qx, Qy, Qw, Qh, PickS_, PickV_, Rx2, Ry2);
        const float RetS = 9.0f * HS;
        Blit(Renderer, WhiteMat_, Rx2, Ry2, RetS + 3.0f * HS, RetS + 3.0f * HS);
        Renderer->SetMaterialTint(AlphaMat_, {Ch[0], Ch[1], Ch[2], 1.0f});
        Blit(Renderer, AlphaMat_, Rx2, Ry2, RetS, RetS);

        // Hue strip + its knob.
        float Hx, Hy, Hw, Hh;
        Picker::HueRect(PG.X, PG.Y, PG.W, PG.SwatchH, PG.SquareH, PG.StripH, PG.Gap, Hx, Hy,
                        Hw, Hh);
        Renderer->DrawMesh(HueStripMesh_, WhiteMat_,
                           Mat4::Translation({Hx, Hy, 0.0f}) * Mat4::Scale({Hw, Hh, 1.0f}));
        const float HueKnobX = Slider::KnobX(Hx, Hw, PG.KnobW, PickH_, 0.0f, 1.0f);
        Blit(Renderer, WhiteMat_, HueKnobX, Hy + Hh * 0.5f, PG.KnobW * 0.5f, Hh + 4.0f * HS);

        // Alpha strip: the current colour ramped transparent -> opaque OVER the key plate, so the
        // ramp reads as transparency rather than as a fade to black.
        float Ax, Ay, Aw, Ah;
        Picker::AlphaRect(PG.X, PG.Y, PG.W, PG.SwatchH, PG.SquareH, PG.StripH, PG.Gap, Ax,
                          Ay, Aw, Ah);
        Blit(Renderer, KeyMat_, Ax + Aw * 0.5f, Ay + Ah * 0.5f, Aw, Ah);
        Renderer->DrawMesh(SvSatMesh_, AlphaMat_,
                           Mat4::Translation({Ax, Ay, 0.0f}) * Mat4::Scale({Aw, Ah, 1.0f}));
        const float AKnobX = Slider::KnobX(Ax, Aw, PG.KnobW, PickA_, 0.0f, 1.0f);
        Blit(Renderer, WhiteMat_, AKnobX, Ay + Ah * 0.5f, PG.KnobW * 0.5f, Ah + 4.0f * HS);

        // Numeric readouts — updated by EITHER drag, which is the point of showing them.
        float Tx2, Ty2, Tw2, Th2;
        Picker::ReadoutRect(PG.X, PG.Y, PG.W, PG.SwatchH, PG.SquareH, PG.StripH, PG.ReadoutH,
                            PG.Gap, Tx2, Ty2, Tw2, Th2);
        const float CellW = Tw2 / static_cast<float>(Picker::Channels);
        for (int I = 0; I < Picker::Channels; ++I) {
            char Vs[24];
            std::snprintf(Vs, sizeof(Vs), "%s %.2f", Picker::ChannelLabel(I),
                          static_cast<double>(Ch[I]));
            Text_.Draw(Renderer, Vs, Tx2 + static_cast<float>(I) * CellW, Ty2, CellW, Th2,
                       10.5f * HS, Ink, Lur::Text::EHAlign::Center, Lur::Text::EVAlign::Middle);
        }

        Blit(Renderer, KeyMat_, PG.CancelX + PG.CancelS * 0.5f, PG.CancelY + PG.CancelS * 0.5f,
             PG.CancelS, PG.CancelS);
        Text_.Draw(Renderer, "x", PG.CancelX, PG.CancelY, PG.CancelS, PG.CancelS, 12.0f * HS,
                   Theme::Warn, Lur::Text::EHAlign::Center, Lur::Text::EVAlign::Middle);
    }

    // ---- The toaster — a panel + text, anchored below/above the row it came from ----
    // It shows two very different things: a one-line CVar tooltip (what it was built for) and a dev
    // command's result, which is one line PER overridden CVar. A fixed box crammed the second case
    // into the first case's height, so MEASURE and size to fit.
    if (!ToastText_.empty()) {
        const float TW = L.PW - 20.0f * HS, TX = L.X0 + (L.PW - TW) * 0.5f;
        const float TPad = 8.0f * HS, TFont = 12.0f * HS;
        float MW = 0.0f, MH = 0.0f;
        int MLines = 0;
        Text_.Measure(ToastText_.c_str(), TW - 2.0f * TPad, TFont, /*Wrap*/ true, MW, MH, MLines);
        // Floor at the old single-line height so a short tooltip looks unchanged, and CAP at a
        // fraction of the screen so a command listing every override produces a panel that fits.
        // The text field gets the FULL measured height and is top-aligned, so an over-cap block is
        // clipped from the BOTTOM — losing the tail, which beats losing the top.
        const float TMin = 40.0f * HS, TMax = HeightPx * 0.55f;
        float TH = MH + 2.0f * TPad;
        if (TH < TMin) TH = TMin;
        if (TH > TMax) TH = TMax;
        const float TY =
            PlaceBelowOrAbove(RowY(ToastCvar_, HeightPx * 0.4f), L.LineH, TH, NumGap, HeightPx);
        Blit(Renderer, PanelMat_, TX + TW * 0.5f, TY + TH * 0.5f, TW, TH);
        Blit(Renderer, AccentMat_, TX + TW * 0.5f, TY + 2.0f * HS, TW, 2.0f * HS);  // accent edge
        // Multi-line output reads top-down; a single line still wants to be centred, so pick the
        // alignment from the content rather than always doing one of them.
        const bool Multi = MLines > 1;
        Text_.Draw(Renderer, ToastText_.c_str(), TX + TPad, TY + (Multi ? TPad : 0.0f),
                   TW - 2.0f * TPad, TH - (Multi ? 2.0f * TPad : 0.0f), TFont, Ink,
                   Lur::Text::EHAlign::Left,
                   Multi ? Lur::Text::EVAlign::Top : Lur::Text::EVAlign::Middle);
    }

    if (TapPending) TapPending_.store(false, std::memory_order_release);   // one-shot
}

}  // namespace Lur::DevGui

#endif  // !LUR_SHIPPING
