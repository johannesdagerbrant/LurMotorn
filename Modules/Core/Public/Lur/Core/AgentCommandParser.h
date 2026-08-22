#pragma once
// Parser for the assistant remote-control command line: "<seq> <verb> [a [b [c]]]".
//
// Promoted out of Rps::AgentControl (#201). The game keeps its VERB TABLE — the names and what they
// mean are entirely game-specific — and hands it in; everything else here is the same for any game
// that wants to be driven headlessly.
//
// ---- THE SEQUENCE NUMBER IS THE WHOLE DESIGN ----
// Both delivery channels are LEVEL-TRIGGERED: an Android system property and a file in the iOS
// container each hold the same text until something changes it, and the app POLLS them. A command with
// no identity would therefore be re-applied on every poll, so "place a camp" becomes a thousand
// places. Only a strictly greater seq is accepted. That single rule is what makes re-reading
// idempotent, makes a channel that fails to clear harmless, and lets a stale value survive a relaunch
// without doing anything.
//
// Corollary, and it has bitten: the sequence must STRICTLY INCREASE across a whole session. Reusing a
// number is not "resending the command", it is a no-op, and it presents as the channel being dead.
//
// ---- WHY THIS IS COMPILED ALWAYS ----
// It is a parser: text in, a struct out. It cannot drive anything, so it is NOT behind LUR_AGENT —
// which is deliberate, because it keeps the command grammar covered by the ordinary host test suite
// instead of only by a build nobody runs. Everything that ACTS on a parsed command (the platform
// channel that reads the text, and every effect) is `#if LUR_AGENT`: absent from every config
// including Development, force-zeroed in Shipping. The gate lives at the call sites.
//
// ---- TOTAL ON HOSTILE INPUT ----
// This parses text from outside the process, so it never traps, never reads past the terminator, and
// rejects anything it does not fully understand. Integers CLAMP rather than wrap, and an unrecognised
// verb does NOT consume the sequence number — so fixing a typo in place still works, instead of
// requiring the author to also remember to bump the seq.
#include <cstdint>

namespace Lur {

// One row of the caller's verb table. Id is the game's own enum value, widened to int32_t so the
// engine never needs to know the enum type.
struct AgentVerb {
    const char* Name;
    int32_t Id;
};

struct AgentCommandLine {
    int32_t Verb = 0;   // the matched row's Id; only meaningful when Poll returned true
    int32_t A = 0, B = 0, C = 0;
    uint32_t Seq = 0;
};

class AgentCommandParser {
public:
    // Parse Text against the caller's verb table and, if it carries a NEW command, fill Out and
    // return true. Verbs is caller-owned and may be a static table.
    bool Poll(const char* Text, const AgentVerb* Verbs, int VerbCount, AgentCommandLine& Out) {
        if (Text == nullptr || Verbs == nullptr || VerbCount <= 0) return false;
        const char* P = Text;
        uint32_t Seq = 0;
        if (!ParseUInt(P, Seq)) return false;
        if (Seq <= LastSeq_) return false;   // already applied (or a stale channel re-read)
        AgentCommandLine Cmd;
        if (!ParseVerb(P, Verbs, VerbCount, Cmd.Verb)) return false;   // unknown verb: ignore, and do
                                                                       // NOT consume the seq
        ParseInt(P, Cmd.A);
        ParseInt(P, Cmd.B);
        ParseInt(P, Cmd.C);
        Cmd.Seq = Seq;
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
            if (V > 0xFFFFFFFFull) return false;   // absurd seq: reject rather than wrap
            ++P;
        }
        Out = static_cast<uint32_t>(V);
        return true;
    }

    // Optional signed argument; leaves Out untouched when absent, so the caller's default stands.
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
                if (V > 0x7FFFFFFF) { V = 0x7FFFFFFF; Clamped = true; }   // clamp, never wrap
            }
            ++P;   // always advance, so digits can't be re-read
        }
        Out = static_cast<int32_t>(Neg ? -V : V);
    }

    // Whole-token match: "place" must not match a "placex" verb, or a typo would silently run
    // something else.
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

    static bool ParseVerb(const char*& P, const AgentVerb* Verbs, int VerbCount, int32_t& Out) {
        SkipSpace(P);
        for (int I = 0; I < VerbCount; ++I) {
            if (Verbs[I].Name == nullptr) continue;
            if (Match(P, Verbs[I].Name)) { Out = Verbs[I].Id; return true; }
        }
        return false;
    }

    uint32_t LastSeq_ = 0;
};

}  // namespace Lur
