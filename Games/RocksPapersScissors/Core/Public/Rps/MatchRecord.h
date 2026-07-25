#pragma once
// Rps::MatchRecord — a flight recorder for a SINGLE-PLAYER match (#144 tooling).
//
// Why it exists: understanding how a human beats a tier needs the whole match, not a summary. The
// sim is deterministic and driven entirely by per-tick input events (design §1, the "replay law"),
// so recording the seed, the latched CVar set, and every tick's COMBINED event batch is enough to
// rebuild the match bit-for-bit on a dev machine — where it can be re-read at any granularity,
// plotted, or (approximately, see below) re-fought against a redesigned AI.
//
// It records the COMBINED batch (human events and AI events together, exactly as StepEvents saw
// them), for two reasons: replay then needs no AI at all, and the AI's own decisions become
// readable — which is the point when the question is "why did hard lose".
//
// Format is deliberately LINE-ORIENTED TEXT, not our slim wire encoding: this is a dev artifact to
// be grepped, diffed and plotted, never something that ships or crosses the BLE link. Roughly a few
// KB per match.
//
//   rec 1                       # format version
//   fp <build fingerprint>      # refuse to trust a replay from a different build
//   seed <hex>
//   tier <0|1|2>  human <team>
//   cv <id> <raw>               # every AffectsGameplay CVar, so replay latches the SAME Cv
//   e <tick> <team> <kind> <type> <xraw> <yraw>     # one line per event, ticks with none are absent
//   c <tick> <g0> <w0> <s0> <b0> <g1> <w1> <s1> <b1> <aistate> <aicounter>   # periodic census
//   end <result> <tick>
//
// CAVEAT for re-fighting a recording against a NEW AI: queue events carry a building SLOT index,
// and slots are shared across both teams in one array — so if a candidate AI places buildings
// differently, the human's recorded queue events point at different slots. Exact replay is exact;
// human-vs-new-AI is a best-effort re-simulation and should be read as indicative.
//
// LUR_INTERNAL only: it is dev tooling, never in a player's build.
#include <cstdint>

#include "Rps/Sim.h"
#include "Rps/Tunables.h"

#if LUR_INTERNAL
#include <cstdio>
#include <string>
#include <vector>

namespace Rps {

// ---- Writing (on the device / in a main) ----
class MatchRecorder {
public:
    // Open Path and write the header from the match's authoritative state. Tier is the AI tier
    // (EAiTier as int, -1 when not applicable). Returns false if the file can't be opened — the
    // caller should carry on regardless; a missing recording must never break a match.
    bool Begin(const char* Path, const Sim& S, int Tier, uint8_t HumanTeam);
    // One tick's COMBINED event batch, exactly as handed to StepEvents. Call after the step so the
    // recorded tick number is the tick those events were applied ON.
    void Events(uint32_t Tick, const InputEvent* Batch, int Count);
    // A census line. AiState/AiCounter are the opponent's internals (or -1 when unknown).
    void Census(const Sim& S, uint8_t HumanTeam, int AiState, int AiCounter);
    // Footer + close. Safe to call twice; safe if Begin failed.
    void End(const Sim& S);
    bool IsOpen() const { return File_ != nullptr; }

private:
    std::FILE* File_ = nullptr;
};

// ---- Reading (host-side analysis) ----
struct RecordedEvent {
    uint32_t Tick;
    InputEvent Event;
};
struct RecordedCensus {
    uint32_t Tick;
    int32_t Gold[2], Workers[2], Soldiers[2], Buildings[2];
    int AiState, AiCounter;
};
struct MatchRecording {
    int Version = 0;
    std::string BuildFp;
    uint64_t Seed = 0;
    int Tier = -1;
    uint8_t HumanTeam = 0;
    CvSnapshot Cv{};                       // the exact latched set the match ran with
    std::vector<RecordedEvent> Events;     // ascending tick
    std::vector<RecordedCensus> Census;
    int Result = -1;
    uint32_t EndTick = 0;
    bool Ok = false;
};

// Parse a recording. Never throws; Ok==false on a malformed/missing file.
MatchRecording LoadMatchRecording(const char* Path);

// Rebuild the match by replaying its events into a fresh Sim (InitWithCvs from the recorded Cv, so
// the state is bit-identical to what the device ran). Steps to EndTick, or to StopAtTick when that
// is smaller. Returns the resulting sim's StateHash.
uint64_t ReplayMatch(const MatchRecording& R, Sim& Out, uint32_t StopAtTick = 0xFFFFFFFFu);

}  // namespace Rps
#endif  // LUR_INTERNAL
