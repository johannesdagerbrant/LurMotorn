#pragma once
#include <functional>

#include "Lur/Core/Log.h"
#include "Lur/Input/Input.h"
#include "Lur/Input/ScrollCamera.h"
#include "Rps/GameView.h"
#include "Rps/Snapshot.h"
#include "Rps/ViewMetrics.h"

// What a touch MEANS in RPS, written once (issue #43, section D).
//
// This lived four times: the Android main, the iOS main, and BOTH halves of DesktopMain.cpp. Not
// four adaptations of one idea — four copies of the same ~90 lines, which had drifted in six
// separate ways by the time they were counted:
//
//   behaviour                              Android  iOS   Desktop solo  Desktop loopback
//   ghost offset + magnetic snap (#1/#148)    yes    yes       yes            NO
//   production-button press flash             yes    yes       yes            NO
//   Cancel the console gesture mid-drag        NO    yes       n/a            n/a
//   pan only while exactly one finger down    yes     NO       n/a            n/a
//   suppress the HUD tap after a 2-finger      yes    yes       n/a            n/a
//   24 px tap slop (a pan release is no tap)  yes    yes        NO            NO
//
// The loopback column is the one that stings: that harness is "the correctness and balance loop"
// (CLAUDE.md), so the place where placement feel was judged was the one place placement did not
// behave like the game. See #209.
//
// THE RESOLUTION IS THE UNION, NOT THE INTERSECTION. Every row above resolves to the behaviour the
// SHIPPING phones have, because that is the one players experience and the one the newest work
// (#1, #107, #148, #151) was tuned against. So this is not purely a refactor: desktop gains the
// ghost offset, the snap, the flash and the tap slop; iOS gains the one-finger pan gate. Those are
// deliberate, and they are the point — a shared router whose behaviour is the lowest common
// denominator would just be a fifth copy.
//
// WHAT STAYS WITH THE MAIN. Only the things that genuinely differ per platform: where a produced
// event goes (a solo inbox, a lockstep peer, a desktop window's own peer), and how a selector pick
// is published (an atomic on the phones, a direct call on desktop). Those are the hooks below.
// Everything else — hit-test order, what commits on press versus release, when the console eats the
// gesture — is a game decision and lives here.

namespace Rps {

// The per-main plumbing. Emit is required; the two selector hooks may be empty (the two-window
// loopback harness has no opponent selector to drive).
struct TouchRouterHooks {
    // A Place or Queue event the local player just produced. The main routes it to whichever match
    // is live — that choice is genuinely per-main and deliberately not modelled here.
    std::function<void(const InputEvent&)> Emit;

    // An AI row in the opponent selector was tapped: (re)start a solo match at this tier (#127).
    std::function<void(int Tier)> PickAiTier;

    // The linked-opponent row was tapped: switch from solo to the peer match (#2).
    std::function<void()> PickPeer;
};

// The bits of per-frame state the router cannot know. Rebuilt by the caller for every batch, because
// every field of it can change between frames.
struct TouchFrame {
    float   ViewW = 0.0f;    // drawable width in pixels
    float   ViewH = 0.0f;    // drawable height in pixels
    uint8_t Team  = 0;       // the team the local player controls (0 in solo)
    bool    Live  = false;   // a match (solo or peer) is running — gates placement and production
};

class TouchRouter {
public:
    // Non-owning. All four must outlive the router, which every main satisfies trivially: they are
    // sibling members of the same app state.
    void Init(GameView* View, Lur::Input::ScrollCamera* Cam,
              TouchRouterHooks Hooks) {
        View_ = View;
        Cam_ = Cam;
        Hooks_ = std::move(Hooks);
    }

    // Route one sample. Returns true if it was consumed (the Android glue wants that; the others
    // ignore it). Safe to call before Init only in the sense that it does nothing.
    bool Route(const Lur::Input::TouchEvent& T, const Snapshot& Snap, const TouchFrame& F) {
        if (View_ == nullptr) return false;
        switch (T.Phase) {
            case Lur::Input::ETouchPhase::Began:     return Began(T, Snap, F);
            case Lur::Input::ETouchPhase::Moved:     return Moved(T, Snap, F);
            case Lur::Input::ETouchPhase::Ended:     return Ended(T, Snap, F);
            case Lur::Input::ETouchPhase::Cancelled: return Cancelled();
        }
        return false;
    }

private:
    // A release counts as a tap only if the pointer barely moved. 24 px is a finger's worth of
    // jitter; without it, ENDING A PAN fires OnTap, which on desktop meant every camera drag also
    // hit-tested the HUD underneath it.
    static constexpr float TapSlopPx = 24.0f;

    // The desired placement point: lifted up-left of the pointer by half a footprint so a thumb does
    // not cover the thing it is placing (#1). The SAME offset feeds the ghost draw, the validity read
    // and the drop, which is why it is computed once per event and passed around.
    float GhostOffset(const Snapshot& Snap, const TouchFrame& F) const {
        return GhostOffsetPx(Snap.Cv.BuildingFootprint.Raw, Ppu(F.ViewW));
    }

    // #148 magnetic drag-to-place: the desired point snaps to the nearest valid spot within about an
    // icon. Returns validity; Gsx/Gsy are where to DRAW the ghost (snapped when valid, else the
    // desired point, so an invalid spot blinks red under the finger rather than jumping away).
    bool Resolve(const Snapshot& Snap, const TouchFrame& F, float DesX, float DesY,
                 float& Wx, float& Wy, float& Gsx, float& Gsy) const {
        return View_->ResolvePlacement(DesX, DesY, Cam_->Y, F.ViewW, F.ViewH, F.Team == 1, Snap,
                                       F.Team, Wx, Wy, Gsx, Gsy);
    }

    bool Began(const Lur::Input::TouchEvent& T, const Snapshot& Snap, const TouchFrame& F) {
        // A SECOND finger landing is not a new press. It feeds the recognizer and stops there — it
        // must not re-seed the tap origin, and above all must not run the plate hit-test, or resting
        // a second finger on a build plate mid-pan starts a placement nobody asked for.
        //
        // This was a seventh point of drift: Android had a dedicated ACTION_POINTER_DOWN case that
        // did exactly this, while iOS sent every -touchesBegan: through the full press path, so the
        // two phones disagreed about what a second finger does. Android's is the careful one.
        // The early return is UNCONDITIONAL; only feeding the recognizer is dev-only. Guarding the
        // whole block would let a second finger fall through to the full press path in Shipping —
        // the exact bug, present only in the build nobody tests against.
        if (T.PointerCount >= 2) {
#if !LUR_SHIPPING
            (void)View_->DevConsole().PointerDown(T.PointerCount, T.XPx, T.YPx, T.TimeNs);
#endif
            return true;
        }
        DownX_ = T.XPx;
        DownY_ = T.YPx;
#if !LUR_SHIPPING
        // The console gets first refusal and answers for itself: it owns its open gesture AND, while
        // open, the whole pointer (a drag scrolls the CVar list, a still release taps a row).
        // Swallowing is the point — the panel sits over a LIVE match, so a scroll must not leak
        // through and pan the camera or start a building drag.
        if (View_->DevConsole().PointerDown(T.PointerCount, T.XPx, T.YPx, T.TimeNs)) return true;
#endif
        const float Off = GhostOffset(Snap, F);
        const float GhX = T.XPx - Off, GhY = T.YPx - Off;

        // Plate hit-test at the REAL pointer, not the offset point — you grab what you touched.
        const int Plate = F.Live ? View_->PlateAt(T.XPx, T.YPx) : -1;
        if (Plate >= 0) {
            View_->BeginPlaceDrag(Plate, GhX, GhY);   // ghost type; seeded at the offset spot
            float Wx = 0, Wy = 0, Gsx = 0, Gsy = 0;
            const bool V = Resolve(Snap, F, GhX, GhY, Wx, Wy, Gsx, Gsy);
            View_->UpdatePlaceDrag(GhX, GhY, Gsx, Gsy, V);
            return true;
        }

        // #107 revised (feedback 2026-08-04): units queue on PRESS-DOWN, not release — the queue
        // fires the instant you touch the button. A miss just primes a pan, and Cam.Begin runs
        // regardless so a drag that starts on a button still scrolls (harmless: already queued).
        int32_t Slot = -1;
        const int Cnt = F.Live ? View_->OnProductionButton(T.XPx, T.YPx, Slot) : 0;
        if (Cnt > 0 && Hooks_.Emit) {
            Hooks_.Emit(InputEvent::Queue(F.Team, Slot, Cnt));
            View_->PressProductionButton(T.XPx, T.YPx);   // visual flash on the pressed button
        }
        Cam_->Begin(T.YPx);
        return true;
    }

    bool Moved(const Lur::Input::TouchEvent& T, const Snapshot& Snap, const TouchFrame& F) {
#if !LUR_SHIPPING
        if (View_->DevConsole().PointerMove(T.XPx, T.YPx, T.TimeNs)) return true;
#endif
        if (View_->IsPlacing()) {
            const float Off = GhostOffset(Snap, F);
            const float GhX = T.XPx - Off, GhY = T.YPx - Off;
            float Wx = 0, Wy = 0, Gsx = 0, Gsy = 0;
            const bool V = Resolve(Snap, F, GhX, GhY, Wx, Wy, Gsx, Gsy);
            View_->UpdatePlaceDrag(GhX, GhY, Gsx, Gsy, V);
#if !LUR_SHIPPING
            View_->DevConsole().CancelGesture();   // #151: a placement drag is not a console tap
#endif
            return true;
        }
        // One finger pans; two or more is a gesture in progress, so the camera must stay put — the
        // console's two-finger chain would otherwise scroll the world under the panel being opened.
        if (T.PointerCount <= 1) Cam_->Move(T.YPx, Ppu(F.ViewW));
        return true;
    }

    bool Ended(const Lur::Input::TouchEvent& T, const Snapshot& Snap, const TouchFrame& F) {
#if !LUR_SHIPPING
        // The console answers first: while open it keeps the release (tap a row, or finish a
        // scroll), and while closed it only keeps one that completed its two-finger chain.
        if (View_->DevConsole().PointerUp(T.XPx, T.YPx, T.TimeNs)) return true;
#endif
        if (View_->IsPlacing()) {
            const float Off = GhostOffset(Snap, F);
            const float GhX = T.XPx - Off, GhY = T.YPx - Off;
            bool Placed = false;
            float Wx = 0, Wy = 0, Gsx = 0, Gsy = 0;
            if (Resolve(Snap, F, GhX, GhY, Wx, Wy, Gsx, Gsy) && Hooks_.Emit) {
                Hooks_.Emit(InputEvent::Place(F.Team, static_cast<uint8_t>(View_->PlacingType()),
                                              WorldToFixed(Wx), WorldToFixed(Wy)));
                Placed = true;
            }
            View_->EndPlaceDrag(Placed);   // valid -> the real building takes over; else slide back
#if !LUR_SHIPPING
            View_->DevConsole().CancelGesture();   // a placement is not a console tap
#endif
            return true;
        }
        Cam_->End();
        const float Dx = T.XPx - DownX_, Dy = T.YPx - DownY_;
        if (Dx * Dx + Dy * Dy < TapSlopPx * TapSlopPx) {
            // The opponent selector consumes its own taps: an AI row (re)starts solo, the linked row
            // switches to the peer, a plate tap does nothing (you drag to place).
            (void)View_->OnTap(T.XPx, T.YPx);
            const int Tier = View_->TakeAiTier();
            if (Tier >= 0) {
                if (Hooks_.PickAiTier) Hooks_.PickAiTier(Tier);
            } else if (View_->TakePeerPick()) {
                if (Hooks_.PickPeer) Hooks_.PickPeer();
            }
            // (x1/x5 queue buttons fire on Began — see above — not here.)
        }
#if !LUR_SHIPPING
        View_->DevConsole().CancelGesture();   // the gesture is over either way
#endif
        return true;
    }

    // UIKit/Android saying "this gesture did not happen" (a call, a system alert). It must NOT
    // commit: sliding the ghost back is the whole difference between a cancel and a release.
    bool Cancelled() {
        if (View_->IsPlacing()) View_->EndPlaceDrag(false);
        Cam_->End();
#if !LUR_SHIPPING
        View_->DevConsole().CancelGesture();
#endif
        return true;
    }

    GameView*                   View_    = nullptr;
    Lur::Input::ScrollCamera*               Cam_     = nullptr;
    TouchRouterHooks            Hooks_;
    float                       DownX_ = 0.0f, DownY_ = 0.0f;
};

}  // namespace Rps
