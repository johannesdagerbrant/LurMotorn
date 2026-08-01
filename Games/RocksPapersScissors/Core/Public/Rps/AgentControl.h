#pragma once
#include <cstdint>

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
};

struct AgentCommand {
    EAgentCmd Kind = EAgentCmd::None;
    int32_t A = 0, B = 0, C = 0;
};

// How many synthetic two-finger taps the `gesture` command feeds the console recognizer. Matches
// Lur::Input::ConsoleGesture::TapsToOpen; named here so both mains drive the same number and a change
// to the gesture cannot silently leave the harness pressing twice.
constexpr int AgentGestureTaps = 3;

// Wire format: "<seq> <verb> [a [b [c]]]", e.g. "7 place 17 16 0".
//
// The SEQUENCE NUMBER is what makes a polled channel usable: both channels are level-triggered (a
// property or a file holds the same text until it is changed), so a command with no identity would be
// re-applied on every poll — "place a camp" would become a thousand places. Only a strictly greater
// seq is accepted, so re-reading is idempotent and a channel that fails to clear is harmless.
//
// Total on hostile input: it is parsing text from outside the process, so it never traps, never reads
// past the terminator, and rejects anything it does not fully understand.
class AgentControl {
public:
    // Parse Text and, if it carries a NEW command, fill Out and return true.
    bool Poll(const char* Text, AgentCommand& Out) {
        if (Text == nullptr) return false;
        const char* P = Text;
        uint32_t Seq = 0;
        if (!ParseUInt(P, Seq)) return false;
        if (Seq <= LastSeq_) return false;          // already applied (or a stale channel re-read)
        AgentCommand Cmd;
        if (!ParseVerb(P, Cmd)) return false;       // unknown verb: ignore, and do NOT consume the seq,
                                                    // so fixing a typo in place still works
        ParseInt(P, Cmd.A);
        ParseInt(P, Cmd.B);
        ParseInt(P, Cmd.C);
        LastSeq_ = Seq;
        Out = Cmd;
        return true;
    }

    uint32_t LastSeq() const { return LastSeq_; }

private:
    static void SkipSpace(const char*& P) {
        while (*P == ' ' || *P == '\t' || *P == '\r' || *P == '\n') ++P;
    }
    static bool ParseUInt(const char*& P, uint32_t& Out) {
        SkipSpace(P);
        if (*P < '0' || *P > '9') return false;
        uint64_t V = 0;
        while (*P >= '0' && *P <= '9') {
            V = V * 10 + static_cast<uint64_t>(*P - '0');
            if (V > 0xFFFFFFFFull) return false;    // absurd seq: reject rather than wrap
            ++P;
        }
        Out = static_cast<uint32_t>(V);
        return true;
    }
    // Optional signed argument; leaves Out untouched when absent, so defaults stand.
    static void ParseInt(const char*& P, int32_t& Out) {
        SkipSpace(P);
        const bool Neg = *P == '-';
        if (Neg) ++P;
        if (*P < '0' || *P > '9') return;
        int64_t V = 0;
        bool Clamped = false;
        while (*P >= '0' && *P <= '9') {
            if (!Clamped) {
                V = V * 10 + static_cast<int64_t>(*P - '0');
                if (V > 0x7FFFFFFF) { V = 0x7FFFFFFF; Clamped = true; }  // clamp, never wrap
            }
            ++P;                                     // always advance, so digits can't be re-read
        }
        Out = static_cast<int32_t>(Neg ? -V : V);
    }
    static bool Match(const char*& P, const char* Word) {
        const char* Q = P;
        while (*Word != '\0') {
            if (*Q != *Word) return false;
            ++Q; ++Word;
        }
        if (*Q != '\0' && *Q != ' ' && *Q != '\t' && *Q != '\r' && *Q != '\n') return false;
        P = Q;
        return true;
    }
    static bool ParseVerb(const char*& P, AgentCommand& Cmd) {
        SkipSpace(P);
        if (Match(P, "place"))   { Cmd.Kind = EAgentCmd::Place;   return true; }
        if (Match(P, "queue"))   { Cmd.Kind = EAgentCmd::Queue;   return true; }
        if (Match(P, "stress"))  { Cmd.Kind = EAgentCmd::Stress;  return true; }
        if (Match(P, "corrupt")) { Cmd.Kind = EAgentCmd::Corrupt; return true; }
        if (Match(P, "droptx"))  { Cmd.Kind = EAgentCmd::DropTx;  return true; }
        if (Match(P, "console")) { Cmd.Kind = EAgentCmd::Console; return true; }
        if (Match(P, "gesture")) { Cmd.Kind = EAgentCmd::Gesture; return true; }
        if (Match(P, "killown")) { Cmd.Kind = EAgentCmd::KillOwn; return true; }
        return false;
    }
    uint32_t LastSeq_ = 0;
};

}  // namespace Rps
