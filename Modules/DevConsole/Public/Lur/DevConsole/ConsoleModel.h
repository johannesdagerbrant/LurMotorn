#pragma once
// Lur::DevConsole::ConsoleModel — the console's LOGIC, decoupled from rendering so it is
// unit-testable on the host without a GPU (dev-console spec §4). The DevGui widget layer
// (the bottom TextField + the completion SelectList + scrollback panel) is a thin view
// over this model; nothing here draws.
//
// It owns: the in-progress input line, the completion list (CVars ∪ dev-commands filtered
// by prefix, MRU-ranked newest-at-bottom), arrow navigation with original-input restore,
// a scrollback ring, and line execution (command dispatch, or CVar set/print — routing an
// AffectsGameplay set through the app's lockstep hook instead of touching it locally).
//
// Entirely dev-only (Modules/DevConsole is excluded from shipping at the CMake level).
#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

#include "Lur/Core/CVar.h"
#include "Lur/Core/DevCommand.h"

namespace Lur::DevConsole {

class ConsoleModel {
public:
    // App hooks. GameplayHook routes an AffectsGameplay CVar set through the lockstep sync
    // (LockstepPeer::SetGameplayCvar) instead of a local write. SaveHook persists after a
    // local (non-gameplay) set. Both optional (null = act locally / don't persist).
    using GameplayHook = void (*)(void* Ctx, Lur::Core::ICVar& Cv, const char* Value);
    using SaveHook = void (*)(void* Ctx);
    void SetGameplayHook(GameplayHook Fn, void* Ctx) { GameplayFn_ = Fn; GameplayCtx_ = Ctx; }
    void SetSaveHook(SaveHook Fn, void* Ctx) { SaveFn_ = Fn; SaveCtx_ = Ctx; }

    // ---- Input + completion ----
    void SetInput(const std::string& Text) {
        Input_ = Text;
        Highlight_ = -1;       // not browsing the list
        Stashed_ = Input_;     // remember the in-progress text for restore-on-arrow-past-end
        Refresh();
    }
    const std::string& Input() const { return Input_; }
    const std::vector<std::string>& Completions() const { return Completions_; }
    int Highlight() const { return Highlight_; }

    // ↑/↓ move the highlight; highlighting OVERWRITES the field with that entry's name.
    // Arrowing DOWN past the last row restores the stashed in-progress text (non-destructive
    // browse). Newest-at-bottom, so ↑ from the field enters at the bottom.
    void ArrowUp() {
        if (Completions_.empty()) return;
        if (Highlight_ < 0) Highlight_ = static_cast<int>(Completions_.size()) - 1;
        else if (Highlight_ > 0) --Highlight_;
        Input_ = Completions_[static_cast<size_t>(Highlight_)];
    }
    void ArrowDown() {
        if (Highlight_ < 0) return;  // already in the field
        ++Highlight_;
        if (Highlight_ >= static_cast<int>(Completions_.size())) {
            Highlight_ = -1;
            Input_ = Stashed_;       // past the end -> restore the original in-progress text
        } else {
            Input_ = Completions_[static_cast<size_t>(Highlight_)];
        }
    }

    // ---- Execution ----
    // Run the current input line: a dev-command if the first token names one, else a CVar
    // assignment ("name value" sets, "name" prints). Appends a result line to scrollback,
    // records the name in the MRU, clears the input.
    void Execute() {
        const std::string Line = Trim(Input_);
        Input_.clear();
        Highlight_ = -1;
        if (Line.empty()) return;
        Echo("> " + Line);

        std::string Out;
        if (Lur::Core::DevCommandRegistry::Dispatch(Line.c_str(), Out)) {
            if (!Out.empty()) Echo(Out);
            Bump(FirstToken(Line));
            Refresh();
            return;
        }

        const std::string Name = FirstToken(Line);
        const std::string Value = Rest(Line);
        Lur::Core::ICVar* Cv = Lur::Core::CVarRegistry::Find(Name.c_str());
        if (!Cv) {
            Echo("unknown cvar/command: " + Name);
            return;
        }
        if (Value.empty()) {
            Echo(Name + " = " + Cv->ValueString());  // read
        } else if (Cv->AffectsGameplay() && GameplayFn_) {
            GameplayFn_(GameplayCtx_, *Cv, Value.c_str());  // route through lockstep sync
            Echo(Name + " := " + Value + "  (synced)");
        } else if (Cv->SetFromString(Value.c_str())) {
            if (SaveFn_) SaveFn_(SaveCtx_);
            Echo(Name + " = " + Cv->ValueString());
        } else {
            Echo("bad value for " + Name + ": " + Value);
        }
        Bump(Name);
        Refresh();
    }

    const std::vector<std::string>& Scrollback() const { return Scrollback_; }

private:
    // Rebuild the completion list from the current input's first token as a prefix. Matches
    // are CVar names ∪ command names; ranked so recently-used entries sit at the BOTTOM
    // (nearest the field), everything else alphabetical above.
    void Refresh() {
        Completions_.clear();
        const std::string Prefix = FirstToken(Input_);
        auto Consider = [&](const char* Name) {
            if (Prefix.empty() || std::string(Name).rfind(Prefix, 0) == 0)
                Completions_.emplace_back(Name);
        };
        Lur::Core::CVarRegistry::ForEach([&](Lur::Core::ICVar* C) { Consider(C->Name()); });
        Lur::Core::DevCommandRegistry::ForEach([&](Lur::Core::DevCommand* C) { Consider(C->Name()); });

        std::sort(Completions_.begin(), Completions_.end());
        // Stable-partition MRU entries to the bottom, in recency order (newest last).
        std::stable_sort(Completions_.begin(), Completions_.end(),
                         [&](const std::string& A, const std::string& B) {
                             return MruRank(A) < MruRank(B);
                         });
    }

    // Rank: non-MRU names get rank 0 (stay on top in alphabetical order); MRU names get
    // 1 + their recency index so the newest-used lands last (bottom).
    int MruRank(const std::string& Name) const {
        for (size_t I = 0; I < Mru_.size(); ++I)
            if (Mru_[I] == Name) return 1 + static_cast<int>(I);
        return 0;
    }
    void Bump(const std::string& Name) {
        Mru_.erase(std::remove(Mru_.begin(), Mru_.end(), Name), Mru_.end());
        Mru_.push_back(Name);  // most-recent at the end (= bottom of the list)
    }

    void Echo(const std::string& Line) {
        Scrollback_.push_back(Line);
        if (Scrollback_.size() > MaxScrollback) Scrollback_.erase(Scrollback_.begin());
    }

    static std::string Trim(const std::string& S) {
        const auto B = S.find_first_not_of(" \t\r\n");
        if (B == std::string::npos) return {};
        const auto E = S.find_last_not_of(" \t\r\n");
        return S.substr(B, E - B + 1);
    }
    static std::string FirstToken(const std::string& S) {
        const std::string T = Trim(S);
        const auto Sp = T.find_first_of(" \t");
        return Sp == std::string::npos ? T : T.substr(0, Sp);
    }
    static std::string Rest(const std::string& S) {
        const std::string T = Trim(S);
        const auto Sp = T.find_first_of(" \t");
        if (Sp == std::string::npos) return {};
        const auto B = T.find_first_not_of(" \t", Sp);
        return B == std::string::npos ? std::string{} : Trim(T.substr(B));
    }

    static constexpr size_t MaxScrollback = 256;
    std::string              Input_;
    std::string              Stashed_;
    std::vector<std::string> Completions_;
    int                      Highlight_ = -1;
    std::vector<std::string> Mru_;
    std::vector<std::string> Scrollback_;
    GameplayHook             GameplayFn_ = nullptr;
    void*                    GameplayCtx_ = nullptr;
    SaveHook                 SaveFn_ = nullptr;
    void*                    SaveCtx_ = nullptr;
};

}  // namespace Lur::DevConsole
