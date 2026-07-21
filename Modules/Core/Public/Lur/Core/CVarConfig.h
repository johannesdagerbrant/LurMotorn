#pragma once
// Lur::Core::CVarConfig — per-game, human-readable CVar override persistence
// (dev-console-cvar-tech-spec.md Addendum B). A flat text file the dev layer OWNS,
// deliberately NOT the Save/Store byte-blob path: overrides want to be legible,
// diffable, and hand-editable.
//
//     # LurMotorn dev cvar overrides - safe to hand-edit; delete a line to un-override.
//     render.debug_bars = true
//     rps.miner.speed   = 0.7
//
// Keyed by the CVar's NAME (its durable identity, C.0.1); stable-sorted on write so
// diffs stay clean; written temp-file-then-rename for crash safety. Entirely dev-only:
// shipping CVars are pure constexpr (§1), so there is nothing to persist.
//
// Determinism note (B.3): this applies overrides LOCALLY at load — correct for solo /
// desktop / disconnected. When peer sync lands (Addendum C / issue #112), AffectsGameplay
// overrides instead route through the match-start sync so both peers apply them at the
// same tick; LoadLocalOnly() already separates the two so that split is a small change.
#include "Lur/Core/CVar.h"

#if !LUR_SHIPPING
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

#include "Lur/Core/Log.h"

namespace Lur::Core {

// Split "name = value  @ <ms>" (ignoring a trailing '#' comment and surrounding
// whitespace). The "@ <ms>" edit-timestamp column (C.4) is optional: a hand-edited line
// with no "@" yields Ms=0 (epoch — loses any conflict to a real edit, re-stamped on next
// write). Returns false for blank / comment-only lines (skip them), true for an assignment.
inline bool ParseCfgLine(const std::string& Line, std::string& Name, std::string& Value,
                         uint64_t& Ms) {
    std::string L = Line;
    if (const auto H = L.find('#'); H != std::string::npos) L.erase(H);  // strip comment
    const auto Eq = L.find('=');
    if (Eq == std::string::npos) return false;
    auto Trim = [](std::string S) {
        const auto B = S.find_first_not_of(" \t\r\n");
        if (B == std::string::npos) return std::string{};
        const auto E = S.find_last_not_of(" \t\r\n");
        return S.substr(B, E - B + 1);
    };
    Name = Trim(L.substr(0, Eq));
    std::string Rhs = L.substr(Eq + 1);
    Ms = 0;
    if (const auto At = Rhs.find('@'); At != std::string::npos) {
        const std::string MsStr = Trim(Rhs.substr(At + 1));
        Rhs.erase(At);
        if (!MsStr.empty()) Ms = std::strtoull(MsStr.c_str(), nullptr, 10);
    }
    Value = Trim(Rhs);
    return !Name.empty() && !Value.empty();
}

// Apply overrides from Path. Unknown names are warned + skipped (a CVar may have been
// renamed/removed — never hard-fail a hand-edited file, C.0.1); malformed values are
// logged loudly per house policy. A missing file is fine (returns 0). When OnlyLocal is
// true, AffectsGameplay CVars are left for the sync layer (#112) and only their count is
// reported; otherwise every override applies immediately (solo/desktop). Returns the
// number applied.
inline int LoadCVarConfig(const char* Path, bool OnlyLocal = false) {
    std::ifstream In(Path);
    if (!In) return 0;
    int Applied = 0;
    std::string Line;
    while (std::getline(In, Line)) {
        std::string Name, Value;
        uint64_t Ms = 0;
        if (!ParseCfgLine(Line, Name, Value, Ms)) continue;
        ICVar* V = CVarRegistry::Find(Name.c_str());
        if (!V) {
            Lur::Log::Info("cvars.cfg: unknown CVar '%s' (skipped)", Name.c_str());
            continue;
        }
        if (OnlyLocal && V->AffectsGameplay()) continue;  // staged for match-start sync (#112)
        if (!V->SetFromString(Value.c_str())) {
            Lur::Log::Error("cvars.cfg: bad value for '%s': '%s'", Name.c_str(), Value.c_str());
            continue;
        }
        V->SetEditWallMs(Ms);  // carry the edit timestamp (0 if the line had none)
        ++Applied;
    }
    return Applied;
}

// Rewrite Path with every currently-overridden CVar as "name = value", stable-sorted by
// name. Whole-file rewrite (the override set is tiny) via a temp file + rename so a crash
// mid-write can't corrupt the file. Returns false on I/O failure.
inline bool SaveCVarConfig(const char* Path) {
    struct Row { std::string Name, Value; uint64_t Ms; };
    std::vector<Row> Rows;
    CVarRegistry::ForEach([&](ICVar* C) {
        if (C->Overridden()) Rows.push_back({C->Name(), C->ValueString(), C->EditWallMs()});
    });
    std::sort(Rows.begin(), Rows.end(), [](const Row& A, const Row& B) { return A.Name < B.Name; });

    const std::string Tmp = std::string(Path) + ".tmp";
    {
        std::ofstream Out(Tmp, std::ios::trunc);
        if (!Out) return false;
        Out << "# LurMotorn dev cvar overrides - safe to hand-edit; delete a line to un-override.\n";
        Out << "# format: name = value  @ <edit-wall-clock-ms>   (the @ column is optional)\n";
        for (const auto& R : Rows) Out << R.Name << " = " << R.Value << "  @ " << R.Ms << "\n";
        if (!Out) return false;
    }
    std::remove(Path);  // Windows rename won't overwrite; POSIX would, but be explicit.
    return std::rename(Tmp.c_str(), Path) == 0;
}

// `reset <name>` (Addendum B.1): drop one override and rewrite. `cvar.reset_all`: clear
// every override and delete the file.
inline void ResetCVar(const char* Name, const char* Path) {
    if (ICVar* V = CVarRegistry::Find(Name)) V->Reset();
    SaveCVarConfig(Path);
}
inline void ResetAllCVars(const char* Path) {
    CVarRegistry::ForEach([](ICVar* C) { C->Reset(); });
    std::remove(Path);
}

}  // namespace Lur::Core
#endif  // !LUR_SHIPPING
