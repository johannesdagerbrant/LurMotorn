#pragma once
// Refuses a match between peers built from different commits.
//
// Promoted out of Rps::LockstepPeer (#112, #164, #166, #201). It is the PROACTIVE form of the anchor-
// hash desync alarm: a fingerprint mismatch is proof, not suspicion, so it is caught before tick 0
// instead of as a mid-match divergence. It is also what makes 1-byte gameplay-CVar ids safe to agree
// on — identical builds imply an identical CVar list.
//
// Twenty-five lines, three rules, and all three came from the SAME symptom: the two phones disagreeing
// about whether there was a mismatch at all.
//
// ---- 1. ASSIGN the verdict, never latch it ----
// A MATCHING fingerprint is positive evidence that the peer's build is fine. A latch could not express
// that, so a peer reinstalled from a matching commit left the other phone reporting a mismatch against
// a build that no longer existed (2026-07-30). A diagnostic that stays lit after the fault is one
// people learn to ignore.
//
// ---- 2. REMEMBER THE STRING, not just the verdict ----
// Because the peer's fingerprint routinely arrives BEFORE this side initialises — that is the normal
// iOS ordering, where one frame pumps the session inbox before reaching the session-ready branch.
//
// ---- 3. RE-DERIVE at init; NEVER clear blind ----
// Clearing at our own init threw rule 2's evidence away, so WHICH peer noticed a mismatch depended on
// init order, and the same mismatched pair read mismatch=1 on one phone and 0 on the other (recorded
// unexplained 2026-07-30, reproduced on hardware 2026-08-01). Re-deriving still retires a stale verdict
// when the peer is reinstalled — its next fingerprint overwrites the string — without discarding one
// that already landed. Between init and a new peer's first fingerprint the verdict describes the
// PREVIOUS peer, which is strictly better than describing nothing.
//
// ---- On the fixed buffer ----
// Fingerprints are short ("9bf59f4c1a2b-dirty+Development"), so this holds one in a fixed array rather
// than allocating in the netcode. Truncation is handled so it can only ever produce a false MISMATCH,
// never a false match: an over-long fingerprint is remembered as truncated and never compares equal.
// Refusing to play is the safe direction; agreeing to play on unverified builds is not.
#include <cstddef>
#include <cstring>

namespace Lur::Net {

class BuildGate {
public:
    // Generous for "<12-hex commit>-dirty+<config>". Longer inputs still work — they are just never
    // declared a match.
    static constexpr int MaxFingerprint = 96;

    // The peer's fingerprint arrived. Stores it and re-derives the verdict (rules 1 and 2).
    // Returns true if it MISMATCHES Local.
    bool OnPeerFingerprint(const char* Data, std::size_t N, const char* Local) {
        Heard_ = true;
        Truncated_ = N > static_cast<std::size_t>(MaxFingerprint);
        const std::size_t Keep = Truncated_ ? static_cast<std::size_t>(MaxFingerprint) : N;
        if (Data != nullptr && Keep > 0) std::memcpy(Peer_, Data, Keep);
        Peer_[Keep] = '\0';
        PeerLen_ = N;
        Mismatch_ = Differs(Local);
        LogSlot_ = true;
        return Mismatch_;
    }

    // (Re-)initialisation. Rule 3: derive from the last fingerprint we actually HEARD rather than
    // clearing, and re-arm the one-shot log slot.
    void Rederive(const char* Local) {
        Mismatch_ = Heard_ && Differs(Local);
        LogSlot_ = true;
    }

    bool Mismatch() const { return Mismatch_; }
    bool HeardPeer() const { return Heard_; }
    bool Truncated() const { return Truncated_; }
    // NUL-terminated; possibly truncated (see Truncated()). Empty until a fingerprint has been heard.
    const char* PeerFingerprint() const { return Peer_; }

    // True the FIRST time after each Rederive/OnPeerFingerprint, false afterwards. A refusal is
    // evaluated on every pre-match tick, so without this the error scrolls the log away.
    bool ClaimLogSlot() {
        const bool Had = LogSlot_;
        LogSlot_ = false;
        return Had;
    }

    // Forget everything, including the heard fingerprint. NOT what init does — see rule 3.
    void Forget() {
        Heard_ = false;
        Mismatch_ = false;
        Truncated_ = false;
        Peer_[0] = '\0';
        PeerLen_ = 0;
        LogSlot_ = true;
    }

private:
    bool Differs(const char* Local) const {
        if (Truncated_) return true;   // never declare a match on a fingerprint we could not hold
        if (Local == nullptr) return true;
        const std::size_t L = std::strlen(Local);
        return PeerLen_ != L || std::memcmp(Peer_, Local, L) != 0;
    }

    char Peer_[MaxFingerprint + 1] = {};
    std::size_t PeerLen_ = 0;
    bool Heard_ = false;
    bool Mismatch_ = false;
    bool Truncated_ = false;
    bool LogSlot_ = true;
};

}  // namespace Lur::Net
