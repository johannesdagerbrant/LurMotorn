#include "Rps/MatchRecord.h"

#if LUR_INTERNAL
#include <cstdlib>
#include <cstring>

#include "Lur/Core/BuildFingerprint.h"
#include "Lur/Core/Log.h"

namespace Rps {
namespace {

// Count a team's live units by role + its producing buildings — the same census --aidiag prints, so
// a recording and a headless run are read the same way.
void Survey(const Sim& S, uint8_t Team, int32_t& Workers, int32_t& Soldiers, int32_t& Buildings) {
    Workers = Soldiers = Buildings = 0;
    for (int32_t I = 0; I < S.Count; ++I) {
        if (!S.IsAlive(I) || S.Team[I] != Team) continue;
        if (S.IsBuilding(I)) { if (!S.IsHomeBase(I)) ++Buildings; }
        else if (S.Type[I] == UnitMiner) ++Workers;
        else ++Soldiers;
    }
}

}  // namespace

bool MatchRecorder::Begin(const char* Path, const Sim& S, int Tier, uint8_t HumanTeam) {
    End(S);  // a previous match's file must be finalised before this one opens
    File_ = std::fopen(Path, "w");
    if (File_ == nullptr) {
        Lur::Log::Info("RPS rec: cannot open %s — match will not be recorded", Path);
        return false;
    }
    std::fprintf(File_, "rec 3\n");   // 3 = CVar lines keyed by NAME (see LoadMatchRecording)
    // #164: the build this recording came from, exact for THIS binary (never the previous commit's
    // sha, which is what the old configure-time macro wrote). Two recordings of one linked match are
    // only comparable if this line agrees, so a stale value here quietly invalidated the diff.
    std::fprintf(File_, "fp %s\n", Lur::BuildFingerprint());
    std::fprintf(File_, "seed %llx\n", static_cast<unsigned long long>(S.Seed));
    std::fprintf(File_, "tier %d human %u\n", Tier, static_cast<unsigned>(HumanTeam));
    // The whole latched gameplay set, keyed by the CVar's NAME. Replay applies these to a default
    // Cv, so a device running persisted overrides replays exactly — without this the replay
    // silently diverges.
    //
    // v3 writes the name, not the wire id, and that is the whole point of the version bump. The id
    // is DECLARATION ORDER in the X-list, so inserting one knob slides every later id by one and
    // yesterday's recordings replay with values shifted along the list — mine rows of "5/35" nobody
    // ever set. It cost an afternoon once and forced a second deliberate renumber later. A name is
    // durable identity (spec C.0.1): reordering the list cannot touch it, adding a knob cannot touch
    // it, and a knob that no longer exists is skipped by name instead of silently misapplied.
    for (uint8_t Id = 0; Id < CvIdCount; ++Id)
        std::fprintf(File_, "cv %s %d\n", GameplayNameForId(Id), CvOverrideRaw(S.Cv, Id));
    // Flush the header NOW, not at the first census (#156). A match sits pre-match until the player
    // places their opening camp, and nothing else flushes until then — so a session that was opened
    // and backgrounded left a ZERO-BYTE file, losing even the seed and the latched CVar set. The
    // header is the part worth keeping unconditionally: it identifies the match. Costs one flush per
    // match, against a per-tick write path that is untouched.
    std::fflush(File_);
    Lur::Log::Info("RPS rec: recording to %s (seed %llx, tier %d)", Path,
                   static_cast<unsigned long long>(S.Seed), Tier);
    return true;
}

void MatchRecorder::Events(uint32_t Tick, const InputEvent* Batch, int Count) {
    if (File_ == nullptr || Batch == nullptr) return;
    for (int I = 0; I < Count; ++I) {
        const InputEvent& E = Batch[I];
        std::fprintf(File_, "e %u %u %u %u %d %d\n", Tick, static_cast<unsigned>(E.Team),
                     static_cast<unsigned>(E.Kind), static_cast<unsigned>(E.Type), E.X, E.Y);
    }
}

void MatchRecorder::Hash(uint32_t Tick, uint64_t StateHash) {
    if (File_ == nullptr) return;
    std::fprintf(File_, "h %u %llx\n", Tick, static_cast<unsigned long long>(StateHash));
}

void MatchRecorder::Census(const Sim& S, uint8_t HumanTeam, int AiState, int AiCounter) {
    if (File_ == nullptr) return;
    const uint8_t Foe = static_cast<uint8_t>(1 - HumanTeam);
    int32_t W[2], So[2], B[2];
    Survey(S, HumanTeam, W[0], So[0], B[0]);
    Survey(S, Foe, W[1], So[1], B[1]);
    std::fprintf(File_, "c %u %d %d %d %d %d %d %d %d %d %d\n", S.Tick, S.Teams[HumanTeam].Gold,
                 W[0], So[0], B[0], S.Teams[Foe].Gold, W[1], So[1], B[1], AiState, AiCounter);
    // Flush on the census (a couple of times a minute, so the cost is nothing). Without it the whole
    // recording sits in the stdio buffer until End(), and a match that is abandoned — or an app that
    // is killed, or a match that simply never resolves — leaves a 0-byte file. A flight recorder
    // that only survives a clean landing is not a flight recorder.
    std::fflush(File_);
}

void MatchRecorder::End(const Sim& S) {
    if (File_ == nullptr) return;
    std::fprintf(File_, "end %u %u\n", static_cast<unsigned>(S.Result), S.Tick);
    std::fclose(File_);
    File_ = nullptr;
}

MatchRecording LoadMatchRecording(const char* Path) {
    MatchRecording R;
    R.Cv = DefaultCvs();
    std::FILE* F = std::fopen(Path, "r");
    if (F == nullptr) {
        Lur::Log::Error("RPS replay: cannot open %s", Path);
        return R;
    }
    char Line[256];
    while (std::fgets(Line, sizeof(Line), F) != nullptr) {
        if (std::strncmp(Line, "rec ", 4) == 0) {
            R.Version = std::atoi(Line + 4);
        } else if (std::strncmp(Line, "fp ", 3) == 0) {
            R.BuildFp = Line + 3;
            while (!R.BuildFp.empty() && (R.BuildFp.back() == '\n' || R.BuildFp.back() == '\r'))
                R.BuildFp.pop_back();
        } else if (std::strncmp(Line, "seed ", 5) == 0) {
            R.Seed = std::strtoull(Line + 5, nullptr, 16);
        } else if (std::strncmp(Line, "tier ", 5) == 0) {
            unsigned H = 0;
            std::sscanf(Line, "tier %d human %u", &R.Tier, &H);
            R.HumanTeam = static_cast<uint8_t>(H & 1u);
        } else if (std::strncmp(Line, "cv ", 3) == 0) {
            // v3 keys by NAME, v2 by ordinal. One parse handles both: a leading digit means the
            // legacy ordinal form (names are all "rps.…", so the two can never be confused).
            char Key[128] = {};
            int  Raw = 0;
            if (std::sscanf(Line, "cv %127s %d", Key, &Raw) == 2) {
                if (Key[0] >= '0' && Key[0] <= '9') {
                    const unsigned Id = static_cast<unsigned>(std::atoi(Key));
                    if (Id < CvIdCount) ApplyCvOverride(R.Cv, static_cast<uint8_t>(Id), Raw);
                } else {
                    const int Id = GameplayIdForName(Key);
                    // A knob this build no longer has: SKIP it. The default stays, which is the
                    // only honest answer — the alternative (applying it to whatever now occupies
                    // that slot) is exactly the silent corruption the name key exists to stop.
                    if (Id >= 0) ApplyCvOverride(R.Cv, static_cast<uint8_t>(Id), Raw);
                    else ++R.UnknownCvs;
                }
            }
        } else if (Line[0] == 'e' && Line[1] == ' ') {
            unsigned T = 0, Team = 0, Kind = 0, Type = 0;
            int X = 0, Y = 0;
            if (std::sscanf(Line, "e %u %u %u %u %d %d", &T, &Team, &Kind, &Type, &X, &Y) == 6) {
                InputEvent E;
                E.Team = static_cast<uint8_t>(Team & 1u);
                E.Kind = static_cast<uint8_t>(Kind);
                E.Type = static_cast<uint8_t>(Type);
                E.X = X;
                E.Y = Y;
                R.Events.push_back({T, E});
            }
        } else if (Line[0] == 'h' && Line[1] == ' ') {
            uint32_t T = 0;
            unsigned long long H = 0;
            if (std::sscanf(Line, "h %u %llx", &T, &H) == 2)
                R.Hashes.push_back({T, static_cast<uint64_t>(H)});
        } else if (Line[0] == 'c' && Line[1] == ' ') {
            RecordedCensus C{};
            if (std::sscanf(Line, "c %u %d %d %d %d %d %d %d %d %d %d", &C.Tick, &C.Gold[0],
                            &C.Workers[0], &C.Soldiers[0], &C.Buildings[0], &C.Gold[1],
                            &C.Workers[1], &C.Soldiers[1], &C.Buildings[1], &C.AiState,
                            &C.AiCounter) == 11)
                R.Census.push_back(C);
        } else if (std::strncmp(Line, "end ", 4) == 0) {
            std::sscanf(Line, "end %d %u", &R.Result, &R.EndTick);
        }
    }
    std::fclose(F);
    // A recording without an "end" line is an ABANDONED match — the app was killed, the player
    // walked away, or (common today) the match simply never resolved. Replay it to the last thing
    // it actually recorded instead of to tick 0, or the most interesting captures read as empty.
    if (R.Result < 0) {
        uint32_t Last = 0;
        if (!R.Events.empty()) Last = R.Events.back().Tick + 1;
        if (!R.Census.empty() && R.Census.back().Tick > Last) Last = R.Census.back().Tick;
        // Hashes too: a LINKED recording writes one every 10 ticks, so on a quiet stretch they are
        // the only evidence of how far the match actually got. Without this an abandoned linked
        // capture replayed to its last INPUT, throwing away every tick after the players stopped
        // touching the screen — which on a desync is exactly the interesting part.
        if (!R.Hashes.empty() && R.Hashes.back().Tick > Last) Last = R.Hashes.back().Tick;
        R.EndTick = Last;
    }
    // VERSION 1 IS REFUSED, not tolerated. The 2026-07-30 ladder collapse deleted a 16-entry
    // LUR_AI_TIER_IDS block from the middle of the gameplay CVar X-list, so the ids after it moved:
    // every `cv <id>` line in a v1 file now names a different knob. That is precisely the failure
    // this file's header warns about (mine rows of "5/35" nobody set), and it does not announce
    // itself — the replay just runs a match with quietly wrong tunables. Loud is the only safe
    // option, so v1 recordings are dead: keep them as text, don't replay them.
    if (R.Version == 1)
        Lur::Log::Error("RPS replay: %s is format v1 (pre-3-tier-collapse); its CVar ids no longer "
                        "match this build and it will NOT be replayed", Path);

    // v2 is ordinal-keyed, so the same hazard applies to it the moment the X-list changes — but it
    // is safe to replay when the recording came from THIS EXACT BUILD, which the `fp` line pins.
    // That keeps today's captures (the #159/#172 material) readable while closing the hole: a v2
    // file from any other build is refused rather than replayed with values slid along the list.
    // v3 is name-keyed and needs no such check, which is the point of it.
    const bool V2SameBuild = (R.Version == 2 && !R.BuildFp.empty() &&
                              R.BuildFp == Lur::BuildFingerprint());
    if (R.Version == 2 && !V2SameBuild)
        Lur::Log::Error("RPS replay: %s is format v2 (CVar ids by declaration order) from build "
                        "'%s', but this is '%s'. Its ids may name different knobs, so it will NOT "
                        "be replayed.", Path, R.BuildFp.c_str(), Lur::BuildFingerprint());
    if (R.UnknownCvs > 0)
        Lur::Log::Info("RPS replay: %s names %d CVar(s) this build does not have — left at their "
                       "defaults", Path, R.UnknownCvs);

    R.Ok = (R.Version == 3 || V2SameBuild) && R.Seed != 0;
    return R;
}

uint64_t ReplayMatch(const MatchRecording& R, Sim& Out, uint32_t StopAtTick) {
    Out.InitWithCvs(R.Seed, R.Cv);   // the recorded Cv, NOT the local globals
    const uint32_t Last = R.EndTick < StopAtTick ? R.EndTick : StopAtTick;
    std::size_t Next = 0;
    for (uint32_t T = 0; T < Last; ++T) {
        // The recorded batch for tick T: events are stored in ascending tick order, so this walks
        // the list once. Ticks with no recorded events step empty, exactly as they did live.
        InputEvent Batch[2 * MaxEventsPerTick];
        int Count = 0;
        while (Next < R.Events.size() && R.Events[Next].Tick == T) {
            if (Count < static_cast<int>(2 * MaxEventsPerTick)) Batch[Count++] = R.Events[Next].Event;
            ++Next;
        }
        Out.StepEvents(Batch, Count);
    }
    return Out.StateHash();
}

}  // namespace Rps
#endif  // LUR_INTERNAL
