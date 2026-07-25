#pragma once
#include <atomic>
#include <string>
#include <unordered_set>

#include "Lur/Core/CVar.h"  // ICVar* (selected cvar)
#include "Lur/DevGui/Numpad.h"
#include "Lur/Hud/Dropdown.h"
#include "Lur/Hud/TextField.h"
#include "Lur/Math/Spring.h"   // visual-only smoothing (frontier retraction, ghost obstacle push)
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

    // #139/feedback: the camp you placed while WAITING for the opponent to place theirs. Pre-match
    // it isn't in the sim yet (both camps are applied together as tick 0's input), so the field
    // looked empty right after you committed it — as if the drop had been lost. Draw it from the
    // peer's pending local camp instead. Production buttons come from the SIM's buildings, so a
    // pending camp correctly has none: you can't queue units before the match starts.
    // Call with Active=false to clear (solo, or once the match is running and the real camp exists).
    void SetPendingCamp(bool Active, float WorldX, float WorldY) {
        PendingCamp_ = Active; PendingCampX_ = WorldX; PendingCampY_ = WorldY;
    }

    // Point the selector at the Linked-opponent row. Called when the main AUTO-switches to the
    // peer as a link establishes: without it the HUD kept naming the AI tier ("Easy") through a
    // linked match, so the player had no way to see who they were actually playing.
    void SelectLinkedOpponent();

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

    // ---- #139 drag-to-place a building ----
    // Which build plate (building type 0..3) is under (XPx,YPx), or -1. The main tests this on
    // a pointer-DOWN to decide whether a drag is a building placement (vs a camera pan).
    int PlateAt(float XPx, float YPx) const;
    // Begin dragging building Type out of its plate at pointer (XPx,YPx); the ghost follows the
    // pointer from that spot until release (seeded here so it never flashes at a stale position).
    void BeginPlaceDrag(int Type, float XPx, float YPx);

    // #139/§9: show a just-placed building at world (Wx,Wy) view-only, before the sim reflects it
    // (the pre-match camp waits for the opponent to ready; a normal placement waits out the input
    // delay). Active=false clears it. Prevents "my camp is invisible until both players placed".
    void SetPlacedPreview(int Type, float Wx, float Wy, bool Active);

    // (#106's minimap-as-scrollbar was REMOVED — playtest 2026-07-25. To be a finger target the
    // strip needed a touch rect ~3.7x wider than the 12*HS ribbon it draws, and that invisible
    // margin reached back over the playfield, stealing taps from the production buttons of any
    // building near the right edge. The minimap is a passive overview again; if it returns, its
    // target must not extend past what it draws.)

    // #140 per-building production: hit-test a tap against the x1/x5 buttons drawn over EVERY local
    // building this frame. Returns the batch COUNT (1/5) and sets OutSlot to the building's sim
    // slot, or 0 if the tap missed. The main routes a hit to QueueLocalEvent(Queue).
    int OnProductionButton(float XPx, float YPx, int32_t& OutSlot) const;
    // #107 press feedback: acknowledge a production-button press on the pointer-DOWN, the frame it
    // happens — the enqueue itself cannot be that fast (1 tick solo, InputDelayTicks in lockstep,
    // both load-bearing), so the BUTTON has to say "heard you" locally. Call this from the mains'
    // down/began path; the commit stays on the release via OnProductionButton, so a press that
    // turns into a camera drag still doesn't queue anything. Returns true if a button was hit.
    // Safe from the input thread (the stamp is atomic; Draw picks it up).
    bool PressProductionButton(float XPx, float YPx);
    // Update the dragged ghost's screen position + whether the current drop is valid (the caller
    // computes validity from the authoritative sim: Sim::WouldAcceptPlace at the drop world pos).
    void UpdatePlaceDrag(float XPx, float YPx, bool Valid);
    // Preferred form: pass BOTH the pointer's own (thumb-offset) point and the RESOLVED spot the sim
    // would accept. The icon is then drawn glued to the pointer with the obstacle-induced offset
    // between them run through a spring, so it never lags the finger while its sidestep around a
    // mine or a building eases instead of snapping. Purely cosmetic: the release still commits the
    // resolved position, so placement is identical with or without the spring.
    void UpdatePlaceDrag(float DesXPx, float DesYPx, float ResXPx, float ResYPx, bool Valid);
    // Release: Placed==true when the caller emitted the place event (valid drop); false slides the
    // ghost back to its plate (invalid drop / no-op). Either way the drag ends.
    void EndPlaceDrag(bool Placed);
    bool IsPlacing() const { return GhostType_ >= 0 && GhostDragging_; }  // a live drag is following the pointer
    int  PlacingType() const { return GhostType_; }
    // Invert the world<->screen transform Render uses, so the main can turn a pointer pixel into a
    // world position (for the place event + validity). Pure function of the passed view params.
    void ScreenToWorld(float XPx, float YPx, float CameraY, float WidthPx, float HeightPx,
                       bool FlipY, float& OutWx, float& OutWy) const;
    // Forward transform (world -> screen pixel), the inverse of ScreenToWorld — used to place the
    // ghost at a SNAPPED world position (#148). Pure function of the passed view params.
    void WorldToScreen(float Wx, float Wy, float CameraY, float WidthPx, float HeightPx,
                       bool FlipY, float& OutXPx, float& OutYPx) const;
    // #148 magnetic placement: resolve a desired pointer position (DesXPx,DesYPx, already thumb-
    // offset) into a placement. Screen->world, snap to the nearest valid spot within ~the icon size
    // (Snapshot::SnapToValidPlace), and report BOTH the snapped world drop (OutWx,OutWy — feed a
    // valid release's Place event) AND where to draw the ghost (OutGhostXPx/YPx — the snapped spot
    // when valid, else the desired point for the red blink). Returns validity. One home for the
    // desktop/Android/iOS mains so the feel can't drift.
    bool ResolvePlacement(float DesXPx, float DesYPx, float CameraY, float WidthPx, float HeightPx,
                          bool FlipY, const Snapshot& Snap, uint8_t Team,
                          float& OutWx, float& OutWy, float& OutGhostXPx, float& OutGhostYPx) const;

    // One-shot: the AI tier (0=Easy,1=Medium,2=Hard) just chosen from the opponent selector,
    // or -1 if none since the last call. The main polls this to (re)start a single-player match
    // (#127/#2 — picking a tier at ANY time starts a fresh match). Reused on desktop + phone.
    int TakeAiTier() { const int T = AiTierPicked_; AiTierPicked_ = -1; return T; }
    // #2 one-shot: the LINKED-opponent row was chosen — the main switches from the AI match to the
    // peer (lockstep) match. True only when a peer is actually linked (the row is otherwise absent).
    bool TakePeerPick() { const bool P = PeerRowPicked_; PeerRowPicked_ = false; return P; }
    // #2 per-opponent SESSION score shown at each selector row's right end ("W-L-D"). The main
    // updates these when a match resolves; persistence across launches rides the #15-20 save work.
    void SetAiScore(int Tier, int W, int L, int D);
    void SetPeerScore(int W, int L, int D);
    // #2: a peer linked while an AI match is running — blink "opponent link established" on the
    // opponent bar (the player can then pick the linked row to switch). One-shot; view times it out.
    void NotifyPeerLinked();
#if !LUR_SHIPPING
    // The CONSOLE (#114) is one tool with ONE UI on both platforms: this cvar-browser
    // overlay, driven by pointer taps. A tap (input thread on the phone) is stashed and
    // consumed on the render thread, where the category/row/numpad rects are known — so
    // hit-test + edits are race-free with the ValueString read. Tapping a category header
    // folds it; tapping a row selects that cvar + opens the numpad; the numpad Enter commits.
    void DevTap(float XPx, float YPx);

    // Scroll the console's cvar list (it holds more cvars than fit on screen, #121). DeltaY in
    // pixels, POSITIVE scrolls the content DOWN (reveals lower rows). Accumulated on the input
    // thread, applied + clamped on the render thread. No-op while the console is hidden.
    void DevScroll(float DeltaY) { DevScrollAccum_.fetch_add(DeltaY, std::memory_order_relaxed); }

    // Show/hide the console. Default hidden (the game is unobstructed). Opened by the phone's
    // two-finger TRIPLE-tap or the desktop § key; closed by the in-panel top-right X button.
    // Closing also dismisses the numpad.
    void SetDevOverlayOpen(bool On) { DevOverlayOpen_ = On; if (!On) NumpadOpen_ = false; }
    bool DevOverlayOpen() const { return DevOverlayOpen_; }

    // Called after a gameplay CVar is committed via the numpad/keyboard (the global value
    // is already set). The app persists (cvars.cfg) and, on a phone in a live match, routes
    // it through the LockstepPeer sync. Null on desktop-solo (LiveCvLatch + save suffice).
    using CvCommitFn = void (*)(void* Ctx, Lur::Core::ICVar& Cv);
    void SetCvCommitHook(CvCommitFn Fn, void* Ctx) { CvCommitFn_ = Fn; CvCommitCtx_ = Ctx; }
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
                  GlyphOreLoad, GlyphMineCamp, GlyphHammer, GlyphCount };
    // OreLoad = the heap on a full cart; MineCamp = the miner building (mine entrance, #140);
    // Hammer = the build-frontier "up to here" legend (#141). Order MUST match the LUR_COOK src.
    Lur::Render::TextureHandle IconAtlas = 0;
    Lur::Render::MaterialHandle AtlasMat = 0;        // white tint: per-instance colour is the fill
    Lur::Render::MeshHandle GlyphMesh[GlyphCount] = {};  // unit quads with per-glyph atlas UVs

    // Atlas-tinted materials for the DrawMesh path (mines / camps).
    Lur::Render::MaterialHandle CampMat[2] = {};
    Lur::Render::MaterialHandle MineMat = 0;
    // #139 placement ghost: translucent team-tinted silhouette while valid; blinking red (two
    // alpha steps) while the drop is invalid. Materials are immutable, so the blink walks a LUT.
    Lur::Render::MaterialHandle GhostMat[2] = {};
    Lur::Render::MaterialHandle GhostBadMat[2] = {};
    Lur::Render::MaterialHandle ProdBtnBg = 0;    // #140 semi-transparent production-button plate
    // #143 production pulse: the first building's x1/x5 buttons animate — the PLATE only in
    // opacity (transparent -> opaque, same base colour), the coin toward bright white glow; the
    // text colour glows toward white in code. The throb walks these step LUTs.
    static constexpr int PulseSteps = 5;
    Lur::Render::MaterialHandle PulsePlate[PulseSteps] = {};  // base-colour plate, rising alpha
    Lur::Render::MaterialHandle PressPlate[PulseSteps] = {};  // #107 LIGHT plate, rising alpha
    Lur::Render::MaterialHandle CoinGlow[PulseSteps] = {};    // gold coin -> white glow
    Lur::Render::MaterialHandle FrontierMat[2] = {};  // #141 per-team build-frontier dotted line
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
    bool               DevOverlayOpen_ = false;     // console shown? (two-finger triple-tap / desktop §)
    Lur::Core::ICVar*  SelectedCvar_ = nullptr;     // highlighted cvar (numpad target)
    std::unordered_set<std::string> CollapsedCats_; // folded category headers, keyed by FULL path
    std::atomic<float> DevScrollAccum_{0.0f};       // input-thread scroll delta -> render thread
    float              ScrollY_ = 0.0f;             // console scroll offset (px into the content)
    Lur::Core::ICVar*  ToastCvar_ = nullptr;        // cvar whose tooltip toaster is showing
    std::string        ToastText_;                  //   the tooltip text ("" = no toaster)
    float              ToastAge_ = 0.0f;            //   seconds shown (auto-dismiss)
    Lur::DevGui::Numpad Numpad_;                    // tap-driven numeric entry (the #118 answer)
    bool               NumpadOpen_ = false;         //   shown after selecting a cvar; Enter commits
    Lur::Render::MaterialHandle DevKeyMat = 0;      //   numpad key face (DevTheme)
    CvCommitFn         CvCommitFn_ = nullptr;       //   app hook: persist + (phone) sync
    void*              CvCommitCtx_ = nullptr;
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
    // Opponent-list layout: the linked row FIRST (when a peer is up), then a non-selectable
    // "AI OPPONENTS" header that renders as the separator, then the three AI tiers. So the AI rows
    // SHIFT depending on whether a peer is linked — never remember a row index, remember WHICH
    // opponent (SelPeer_/SelAiTier_) and re-derive the row.
    int PeerRow() const { return 0; }
    int AiRow(int Tier) const { return (Linked ? 2 : 0) + Tier; }
    bool SelPeer_ = false;    // the selection is the linked peer (else an AI tier)
    int  SelAiTier_ = 0;      // which AI tier the selection is/was
    int  AiTierPicked_ = -1;              // #127: AI tier chosen from the selector (one-shot via TakeAiTier)
    bool PeerRowPicked_ = false;          // #2: linked-opponent row chosen (one-shot via TakePeerPick)
    // #2 per-opponent session score ("W-L-D"), shown at each row's right end.
    int  AiScoreW_[3] = {}, AiScoreL_[3] = {}, AiScoreD_[3] = {};
    int  PeerScoreW_ = 0, PeerScoreL_ = 0, PeerScoreD_ = 0;
    float PeerLinkBannerT_ = 0.0f;        // #2 "opponent link established" blink countdown (seconds)
    Lur::Text::Font ClockFont;            // DSEG7: monospaced digits for the match clock
    Lur::Hud::TextField ClockText;
    float PlateRect[4][4] = {};           // per-type plate {x,y,w,h}, cached for OnTap
    // §9 opening gate: this plate's building type isn't unlocked yet, so it draws greyed and is NOT
    // a drag source (PlateAt / OnTap skip it). Refreshed from the snapshot each Draw.
    bool  PlateLocked[4] = {};
    // #139/feedback: a committed-but-not-yet-simulated mining camp (waiting on the opponent).
    bool  PendingCamp_ = false;
    float PendingCampX_ = 0.0f, PendingCampY_ = 0.0f;
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
    // #141 build-frontier lines. ADVANCE snaps (it must match where you can build, exactly);
    // RETRACTION runs through a double spring, because losing ground happens when a far-off unit
    // dies and an instant jump goes unseen. Visual only — the sim's frontier stays the authority.
    // Advance rides the same per-tick Alpha the units do (the sim moves this line at 10 Hz, so
    // re-snapping it every frame made it visibly step); retraction runs the spring.
    float FrontierPrev_[2] = {0.0f, 0.0f};
    float FrontierCur_[2] = {0.0f, 0.0f};
    uint32_t FrontierTick_ = 0;
    bool FrontierHaveTick_ = false;
    bool Retracting_[2] = {false, false};
    Lur::Math::DoubleSpring FrontierSpring[2];
    static constexpr float FrontierRetractHalflife = 0.45f;   // slow enough to notice, not to annoy
    // #143 onboarding (view-only, per session): a looping pointing hand demos the first camp
    // placement until you place one; then the first building's x1/x5 buttons PULSE until you
    // queue anything there (taught once, never nags again).
    float OnbHandT_ = 0.0f;      // placement-hand loop clock
    float PulseT_ = 0.0f;        // production-pulse throb clock
    bool  ProductionTaught_ = false;
    // First-scroll hint: pointing finger + up/down arrows bobbing mid-screen from the
    // moment one of YOUR units leaves the screen until the first camera pan.
    enum class EHint : uint8_t { Idle, Active, Fading, Done };
    EHint Hint = EHint::Idle;
    float HintAge = 0.0f, HintFade = 0.0f, HintCamY = 0.0f;
    static constexpr int HintAlphaSteps = 6;   // materials are immutable: fade = LUT
    Lur::Render::MaterialHandle HintPointer[HintAlphaSteps] = {};
    Lur::Render::MaterialHandle HintArrow[HintAlphaSteps] = {};
    Lur::Render::MeshHandle ArrowUp = 0, ArrowDown = 0;

    // #139 drag-to-place state (view-only). GhostType_ >= 0 while a placement is in play (either
    // following the pointer, GhostDragging_, or sliding back after an invalid drop, SlideT_>=0).
    int   GhostType_ = -1;
    bool  GhostDragging_ = false;
    float GhostXPx_ = 0.0f, GhostYPx_ = 0.0f;   // RESOLVED spot (or slide-back head) — what gets placed
    // The pointer's own position, plus a spring on the (resolved - desired) offset. Springing the
    // OFFSET rather than the position is what keeps the icon on the finger while still smoothing how
    // obstacles shove it aside. Visual only; the release commits GhostXPx_/GhostYPx_.
    float GhostDesiredX_ = 0.0f, GhostDesiredY_ = 0.0f;
    Lur::Math::DoubleSpring GhostPushX_, GhostPushY_;
    static constexpr float GhostPushHalflife = 0.09f;   // brisk: this must read as reactive, not soft
    bool  GhostValid_ = false;
    float GhostBlink_ = 0.0f;                    // invalid-blink clock (seconds)
    float SlideT_ = -1.0f;                       // >=0 while the ghost tweens back to its plate
    float SlideFromX_ = 0.0f, SlideFromY_ = 0.0f;
    // #139 placed-preview: a committed building shown view-only until the sim reflects it.
    int   PreviewType_ = -1;
    float PreviewWx_ = 0.0f, PreviewWy_ = 0.0f;
    bool  PreviewActive_ = false;

    // #140 per-building x1/x5 buttons — rects captured each Render (over the local team's
    // buildings) so a tap on the input thread can hit-test them. Slot is the building's sim slot.
    static constexpr int ProdBtnPerBldg = 2;
    struct ProdButtons { int32_t Slot = -1; float R[ProdBtnPerBldg][4] = {}; };
    static constexpr int MaxProdButtons = 128;
    static constexpr int ProdMult[ProdBtnPerBldg] = {1, 5};
    ProdButtons ProdBtns_[MaxProdButtons];
    int ProdBtnCount_ = 0;
    // #107 press feedback. The press is stamped on the INPUT thread (Android/iOS) and consumed by
    // the render thread, so the hand-off is atomic — same discipline as DevTap/DevScroll. One slot
    // is enough: a finger presses one button at a time, and a second press just restarts the flash.
    std::atomic<int32_t> PressSlot_{-1};      // input -> render: building slot pressed
    std::atomic<int32_t> PressBtn_{-1};       //   ... and which button of its stack (0=x1, 1=x5)
    std::atomic<bool>    PressPending_{false};
    static constexpr float PressFlashSec = 0.13f;   // long enough to see, short enough to feel crisp
    int32_t FlashSlot_ = -1, FlashBtn_ = -1;  // render-thread only, latched from the atomics
    float   FlashT_ = 0.0f;                   // counts down PressFlashSec -> 0

    bool Ready = false;
};

}  // namespace Rps
