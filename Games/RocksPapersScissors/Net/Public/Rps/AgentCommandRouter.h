#pragma once

// What an agent COMMAND does, written once (issue #43, section E).
//
// #if LUR_AGENT in its entirety. These are effects that drive the app on an assistant's behalf —
// writing the sim directly, corrupting state, suppressing outgoing frames — and CLAUDE.md's rule is
// that such code is ABSENT from a build a player holds, not merely idle in it. The command PARSER
// (Rps::AgentControl) stays compiled always, because it cannot drive anything and that keeps its
// grammar in the host suite.
#if LUR_AGENT

#include <functional>

#include "Rps/AgentControl.h"
#include "Rps/LockstepPeer.h"
#include "Rps/Sim.h"
#include "Lur/Core/Log.h"

// The verb table existed TWICE — the Android main and the iOS main — at 96 and 80 lines, handling
// the same ten verbs in the same order. Nothing had gone missing, which is the good news; what had
// drifted was everything around the decisions:
//
//   * `stress` logged the resulting unit count on Android and the live-sim flag too; iOS logged
//     neither, so the one verb whose whole purpose is reaching a specific unit count did not report
//     reaching it on half the pair.
//   * the `droptx` and `stress` messages had diverged in wording, which matters more than it sounds
//     when the workflow is "grep both phones' logs for the same line".
//   * every message went through a per-platform logger (os_log / LOGI) even though Lur::Log reaches
//     both — so the shared harness's output was the one thing that could not be grepped uniformly.
//
// And one real defect, present in BOTH copies: the `console` verb wrote GameView::SetDevOverlayOpen
// directly from the SIM thread, while the view is owned by another thread on both platforms (render
// on iOS since #183, glue on Android). DevOverlayOpen_ is a plain bool. The `gesture` verb three
// cases above it hands its request across an atomic for exactly this reason and says so in a
// comment; `console` simply did not. It is routed through RequestConsole here, so the owning thread
// applies it.
//
// WHAT STAYS WITH THE MAIN: only the things that genuinely differ — where a produced event goes,
// how a cross-thread request is published, and which object is the solo sim. Those are the hooks.

namespace Rps {

struct AgentHooks {
    // A produced input event, routed to whichever match is live. Per-main because that choice is.
    std::function<void(const InputEvent&)> Emit;

    // The team we play (0 in solo). Read through a hook because both mains keep it in an atomic the
    // sim thread publishes.
    std::function<uint8_t()> Team;

    // Is the SOLO sim the live one? Gates the misroute warning and picks the sim `stress` fills.
    std::function<bool()> SoloActive;

    // Is a peer linked and ready? Only sharpens the warning: "went to solo while a peer waits" is an
    // error, "went to solo, nothing linked yet" is routine.
    std::function<bool()> PeerReady;

    // The solo sim, or nullptr if this main has none. Never owned.
    std::function<Sim*()> SoloSim;

    // Open/close the dev console. MUST be published for the VIEW-OWNING thread to apply — pass an
    // atomic store, not a direct GameView call. See the header note.
    std::function<void(bool)> RequestConsole;

    // Synthetic two-finger triple-tap, fed to the shared recognizer by whichever thread owns it.
    std::function<void()> RequestGesture;

    // #170: switch from the opening solo match to the linked peer.
    std::function<void()> RequestLinked;
};

class AgentCommandRouter {
public:
    // Non-owning; Lp must outlive the router (in both mains they are siblings in one app state).
    void Init(LockstepPeer* Lp, AgentHooks Hooks) {
        Lp_ = Lp;
        Hooks_ = std::move(Hooks);
    }

    void Apply(const AgentCommand& Cmd) {
        if (Lp_ == nullptr) return;
        const uint8_t Team = Hooks_.Team ? Hooks_.Team() : 0;
        switch (Cmd.Kind) {
            case EAgentCmd::Place: {
                // EXACT coordinates, which the touch UI cannot produce: drag-to-place snaps to the
                // nearest VALID square and emits nothing on an invalid drop, so a human cannot place
                // onto the square an existing camp occupies — and that is exactly the #160 collision.
                Lur::Log::Info("AGENT place type=%d at (%d,%d) team=%u", Cmd.C, Cmd.A, Cmd.B,
                               static_cast<unsigned>(Team));
                WarnIfSolo("place");
                Emit(InputEvent::Place(Team, static_cast<uint8_t>(Cmd.C & 3), F(Cmd.A), F(Cmd.B)));
                break;
            }
            case EAgentCmd::Queue:
                Lur::Log::Info("AGENT queue slot=%d count=%d", Cmd.A, Cmd.B);
                WarnIfSolo("queue");
                Emit(InputEvent::Queue(Team, Cmd.A, Cmd.B));
                break;
            case EAgentCmd::Stress: {
                // #162's load scenario. Writes the sim directly rather than going through input,
                // because the point is to reach a unit count the economy would take an hour to fund.
                //
                // SOLO is the sim to use for a clean PERF measurement: filling only this peer
                // diverges a linked pair by construction, and #161's recovery then fires in the
                // middle of the numbers. Linked is the right one for the COLLAPSE scenario, where the
                // divergence is part of what is being tested.
                const bool Solo = Hooks_.SoloActive && Hooks_.SoloActive();
                Sim* Sm = Solo && Hooks_.SoloSim ? Hooks_.SoloSim() : &MutableLinkedSim();
                if (Sm == nullptr) break;
                Lur::Log::Info("AGENT stress %d per team, type %d (solo=%d, count %d -> ...)", Cmd.A,
                               Cmd.B, Solo ? 1 : 0, Sm->Count);
                Sm->StressFill(Cmd.A, static_cast<uint8_t>(Cmd.B));
                Lur::Log::Info("AGENT stress done, count=%d", Sm->Count);
                break;
            }
            case EAgentCmd::Corrupt:
                Lur::Log::Info("AGENT corrupt gold %+d - forcing a divergence (#161)", Cmd.A);
                Lp_->AgentCorruptState(Cmd.A);
                break;
            case EAgentCmd::DropTx:
                Lur::Log::Info("AGENT drop next %d produced frame(s) - simulating #163's half-open link",
                               Cmd.A);
                Lp_->AgentDropOutgoing(Cmd.A);
                break;
            case EAgentCmd::Console:
                // Published, not applied: the view belongs to another thread on both platforms.
                Lur::Log::Info("AGENT console %d", Cmd.A);
                if (Hooks_.RequestConsole) Hooks_.RequestConsole(Cmd.A != 0);
                break;
            case EAgentCmd::Gesture:
                // #151's real subject: drive the SHARED recognizer with a synthetic two-finger
                // triple-tap, so it exercises the recognizer -> SetDevOverlayOpen wiring without
                // needing multitouch injection — which neither platform offers.
                Lur::Log::Info("AGENT gesture: synthetic two-finger triple-tap requested");
                if (Hooks_.RequestGesture) Hooks_.RequestGesture();
                break;
            case EAgentCmd::KillOwn: {
                // #160 setup: free the ground under our own camp so it can be REBUILT on the same
                // square. That is the only route by which a produced placement can carry coordinates
                // equal to the opening camp's, which is what the old payload-sniffing re-send check
                // mistook for a re-send. Kills via Hp so the sim's own death handling runs.
                Sim& Sm = MutableLinkedSim();
                for (int32_t I = 0; I < Sm.Count; ++I) {
                    if (!Sm.IsAlive(I) || Sm.Team[I] != Team) continue;
                    if (!Sm.IsBuilding(I) || Sm.IsHomeBase(I)) continue;
                    if (Sm.Type[I] != static_cast<uint8_t>(Cmd.A & 3)) continue;
                    Lur::Log::Info("AGENT killown slot=%d type=%d at (%d,%d)", I, Sm.Type[I],
                                   Sm.PosX[I].ToInt(), Sm.PosY[I].ToInt());
                    Sm.Hp[I] = 0;
                    break;
                }
                break;
            }
            case EAgentCmd::Linked:
                // #170: the ONE route into the linked session a harness can rely on. It sets the same
                // flag the selector's "Linked opponent" row sets, and that route is deliberately
                // exempt from the `!HasMinerCamp(0)` gate the AUTO-switch carries — so it still works
                // after a stray `place` has put a camp in the solo sim, which is the state the
                // auto-switch can never leave. The flag is LATCHED, so sending this before the link
                // is up is fine; it takes effect on the frame the peer becomes ready.
                Lur::Log::Info("AGENT linked -> requesting the switch to the linked opponent");
                if (Hooks_.RequestLinked) Hooks_.RequestLinked();
                break;
            case EAgentCmd::None:
                break;
        }
    }

private:
    // #170: name the misroute instead of letting it pass. Input produced while the app is still in
    // its opening AI match goes to the SOLO sim, so the peer sits at stall=1 waiting for a camp that
    // was never sent — and every step of that reports success.
    void WarnIfSolo(const char* What) {
        if (!Hooks_.SoloActive || !Hooks_.SoloActive()) return;
        if (Hooks_.PeerReady && Hooks_.PeerReady())
            Lur::Log::Error("AGENT %s -> the SOLO sim, not the linked peer (a peer IS linked). The "
                            "other phone will wait forever. Send `linked` first, then re-send this.",
                            What);
        else
            Lur::Log::Info("AGENT %s -> the solo sim (no peer linked yet)", What);
    }

    void Emit(const InputEvent& E) {
        if (Hooks_.Emit) Hooks_.Emit(E);
    }

    // The lockstep sim, writable. const_cast because the peer hands out a const view by design — the
    // sim is its business — and these verbs exist precisely to violate that from outside.
    Sim& MutableLinkedSim() { return const_cast<Sim&>(Lp_->GetSim()); }

    LockstepPeer* Lp_ = nullptr;
    AgentHooks    Hooks_;
};

}  // namespace Rps

#endif  // LUR_AGENT
