#pragma once
#include <cstdint>

#include "Lur/Core/AgentCommandParser.h"

namespace Rps {

// The assistant's remote-control command set for a phone (CLAUDE.md's LUR_AGENT axis).
//
// WHY THIS EXISTS. Several bugs in this batch can only be reproduced on real hardware, and the
// scenarios need input a human would have to perform by hand — placing a camp on an exact square,
// dropping a single BLE frame, reaching 1600 units, forcing a divergence. iOS has no touch injection
// at all (pymobiledevice3 can launch and screenshot, nothing more), so without this the iPhone half of
// every two-phone scenario is untestable except by asking a person to tap.
//
// WHAT IS GATED WHERE. This header is a PARSER: text in, a struct out. It cannot drive anything, so it
// is compiled always — which is deliberate, because it means the command grammar is covered by the
// ordinary host test suite instead of only by a build nobody runs. Everything that ACTS on a command
// — the platform channel that reads it (an Android system property, a file in the iOS container) and
// every effect it applies — is `#if LUR_AGENT`, absent from every config including Development, and
// force-zeroed in Shipping. That split follows the same rule as the console plumbing: the gate lives
// at the call sites.
//
// The two handover rules from CLAUDE.md apply to anything built on this: rebuild WITHOUT the flag so
// the code is absent rather than idle, and leave no device state behind (clear the property, delete
// the command file) — "inert by default" is not the same as "not there". The 2026-07-25 playtest lost
// an evening to a stale setprop fighting the player's own gesture.
enum class EAgentCmd : uint8_t {
    None,
    Place,    // A=world X, B=world Y, C=unit type -> inject a Place event at EXACT coordinates
    Queue,    // A=building slot, B=count      -> inject a Queue event
    Stress,   // A=units per team, B=unit type -> bulk-spawn (the #162 load scenario)
    Corrupt,  // A=gold delta                  -> diverge this peer's state on purpose (#161)
    DropTx,   // A=frames                      -> silently drop the next A produced input frames (#163)
    Console,  // A=0/1                         -> force the dev console overlay open/closed (#151)
    Gesture,  // (none)                        -> drive the shared two-finger-tap recognizer (#151)
    KillOwn,  // A=unit type                   -> destroy our own building of that type (#160 setup)
    Linked,   // (none)                        -> take the selector's "Linked opponent" route (#170)
};

struct AgentCommand {
    EAgentCmd Kind = EAgentCmd::None;
    int32_t A = 0, B = 0, C = 0;
};

// How many synthetic two-finger taps the `gesture` command feeds the console recognizer. Matches
// Lur::Input::ConsoleGesture::TapsToOpen; named here so both mains drive the same number and a change
// to the gesture cannot silently leave the harness pressing twice.
constexpr int AgentGestureTaps = 3;

// WHICH SIM A COMMAND REACHES (#170). The app OPENS in a solo match vs the AI, so `place` sent before
// the phone has switched to the linked session lands in the SOLO sim — accepted, logged, and useless,
// while the other phone waits forever for a camp that went somewhere else. A human cannot make this
// mistake (they can see the AI match on screen and pick the linked opponent); an assistant driving
// headlessly cannot see it at all. Worse, the misrouted camp then makes the solo->linked AUTO-switch
// refuse for the life of the process, because that switch deliberately never abandons an AI match the
// player has started.
//
// So `linked` exists: it takes the same route the player's selector does, which is exempt from that
// gate. Send it FIRST in any two-phone scenario, and check the log says the switch happened, before
// sending anything that produces input. The mains also warn when a `place`/`queue` is about to land in
// solo while a peer is ready, so the misroute names itself rather than passing silently.
//
// Wire format: "<seq> <verb> [a [b [c]]]", e.g. "7 place 17 16 0".
//
// The SEQUENCE NUMBER is what makes a polled channel usable: both channels are level-triggered (a
// property or a file holds the same text until it is changed), so a command with no identity would be
// re-applied on every poll — "place a camp" would become a thousand places. Only a strictly greater
// seq is accepted, so re-reading is idempotent and a channel that fails to clear is harmless.
//
// Total on hostile input: it is parsing text from outside the process, so it never traps, never reads
// past the terminator, and rejects anything it does not fully understand.
// Since #201 the grammar, the sequence-number gating and the hostile-input hardening are
// Lur::AgentCommandParser; what stays here is the VERB TABLE, which is the only game-specific part of
// a command line. The table is the single place a verb name is bound to its meaning — the enum above
// and this array are the whole contract.
class AgentControl {
public:
    // Parse Text and, if it carries a NEW command, fill Out and return true.
    bool Poll(const char* Text, AgentCommand& Out) {
        static constexpr Lur::AgentVerb Verbs[] = {
            {"place",   static_cast<int32_t>(EAgentCmd::Place)},
            {"queue",   static_cast<int32_t>(EAgentCmd::Queue)},
            {"stress",  static_cast<int32_t>(EAgentCmd::Stress)},
            {"corrupt", static_cast<int32_t>(EAgentCmd::Corrupt)},
            {"droptx",  static_cast<int32_t>(EAgentCmd::DropTx)},
            {"console", static_cast<int32_t>(EAgentCmd::Console)},
            {"gesture", static_cast<int32_t>(EAgentCmd::Gesture)},
            {"killown", static_cast<int32_t>(EAgentCmd::KillOwn)},
            {"linked",  static_cast<int32_t>(EAgentCmd::Linked)},
        };
        Lur::AgentCommandLine Line;
        if (!Parser_.Poll(Text, Verbs, static_cast<int>(sizeof(Verbs) / sizeof(Verbs[0])), Line))
            return false;
        Out.Kind = static_cast<EAgentCmd>(Line.Verb);
        Out.A = Line.A;
        Out.B = Line.B;
        Out.C = Line.C;
        return true;
    }

    uint32_t LastSeq() const { return Parser_.LastSeq(); }

private:
    Lur::AgentCommandParser Parser_;
};

}  // namespace Rps
