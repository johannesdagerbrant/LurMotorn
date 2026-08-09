#pragma once
// Lur::Core::DevCommand — named dev/debug functions with args (dev-console spec §2), the
// sibling of the CVar system: a CVar is a tunable value, a command DOES something
// (restart a match, wipe save history, spawn units). Registered by engine modules
// (net.*, save.*) or the game (its own prefix), enumerated for console completion,
// dispatched by name.
//
// ENTIRELY dev-only: unlike CVars (which must survive into shipping as pure constexpr),
// commands do not exist in a shipping build at all — so a ctor-side-effect registration is
// fine here (no constexpr-shape contract to protect). The whole header is #if !LUR_SHIPPING.
//
// A command that mutates SIM state (a restart, a spawn) must route through the
// lockstep tick stream (LockstepPeer) so both peers stay in lockstep — the command handler
// itself does that via its context; this registry only names + dispatches. A command that
// touches only local/non-sim state runs immediately.
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "Lur/Core/Assert.h"

#if !LUR_SHIPPING
namespace Lur::Core {

// Tokenized command arguments. Argv entries point into a buffer the dispatcher keeps alive
// for the duration of the handler call.
struct DevArgs {
    static constexpr int MaxArgs = 8;
    const char* Argv[MaxArgs] = {};
    int         Argc = 0;
    const char* Arg(int I) const { return (I >= 0 && I < Argc) ? Argv[I] : ""; }
};

// Handler: reads args, appends human-readable result line(s) to Out (the console
// scrollback, or a test capture), and uses Ctx (the game/engine state it was registered
// with) to actually act.
using DevCommandFn = void (*)(const DevArgs& Args, std::string& Out, void* Ctx);

class DevCommand {
public:
    DevCommand(const char* Name, const char* Help, DevCommandFn Fn, void* Ctx = nullptr,
               const char* Category = "");
    const char* Name() const { return Name_; }
    const char* Help() const { return Help_; }
    const char* Category() const { return Category_; }
    void        Run(const DevArgs& Args, std::string& Out) const { Fn_(Args, Out, Ctx_); }

    DevCommand* NextRegistered_ = nullptr;  // intrusive registry list

private:
    const char*  Name_;
    const char*  Help_;
    DevCommandFn Fn_;
    void*        Ctx_;
    const char*  Category_;
};

// LUR_DEV_COMMAND(Var, "name", "help", Fn, Category) — declare + register in one line, the
// DevCommand mirror of LUR_CVAR. Dev-only by construction: this whole header is excluded from
// shipping, so a command cannot leak into a build a player runs.
//
// The console renders every registered command as a BUTTON grouped under its Category (#116).
// That is not ergonomics — it is the only way to invoke one. Neither platform has text entry
// (the console edits numbers through a numpad), so a command nobody can click is a command
// nobody can run.
#define LUR_DEV_COMMAND(Var, Name, Help, Fn, Category) \
    inline ::Lur::Core::DevCommand Var { Name, Help, Fn, nullptr, Category }

// Meyers-singleton registry (static-init-order independent across TUs, same as CVars).
class DevCommandRegistry {
public:
    static DevCommand*& Head() {
        static DevCommand* H = nullptr;
        return H;
    }
    static void Register(DevCommand* C) {
        for (DevCommand* K = Head(); K; K = K->NextRegistered_)
            LUR_ASSERT_MSG(std::strcmp(K->Name(), C->Name()) != 0, "duplicate dev-command: %s",
                           C->Name());
        C->NextRegistered_ = Head();
        Head() = C;
    }
    static DevCommand* Find(const char* Name) {
        for (DevCommand* C = Head(); C; C = C->NextRegistered_)
            if (std::strcmp(C->Name(), Name) == 0) return C;
        return nullptr;
    }
    template <class Fn>
    static void ForEach(Fn&& F) {
        for (DevCommand* C = Head(); C; C = C->NextRegistered_) F(C);
    }

    // Tokenize `Line` on whitespace, resolve the first token as a command name, run it with
    // the rest as args. Returns false (no output) if no such command — the caller then
    // tries a CVar assignment. Output goes to Out.
    static bool Dispatch(const char* Line, std::string& Out) {
        std::vector<std::string> Toks;
        std::istringstream Ss(Line ? Line : "");
        for (std::string T; Ss >> T;) Toks.push_back(T);
        if (Toks.empty()) return false;
        DevCommand* C = Find(Toks[0].c_str());
        if (!C) return false;
        DevArgs A;
        A.Argc = static_cast<int>(Toks.size()) - 1;
        if (A.Argc > DevArgs::MaxArgs) A.Argc = DevArgs::MaxArgs;
        for (int I = 0; I < A.Argc; ++I) A.Argv[I] = Toks[static_cast<size_t>(I) + 1].c_str();
        C->Run(A, Out);
        return true;
    }
};

inline DevCommand::DevCommand(const char* Name, const char* Help, DevCommandFn Fn, void* Ctx,
                              const char* Category)
    : Name_(Name), Help_(Help), Fn_(Fn), Ctx_(Ctx), Category_(Category) {
    DevCommandRegistry::Register(this);  // dev-only: ctor-side-effect registration is fine
}

// ---- Engine commands (#116) ----
// The registry had zero entries until now, which made "commands as buttons" untestable and
// unverifiable — an empty strip proves nothing. These two are genuinely useful while tuning
// and exercise both shapes the console needs to render (a destructive action, a report).
inline void DevCmdResetCvars(const DevArgs&, std::string& Out, void*) {
    int N = 0;
    CVarRegistry::ForEach([&](ICVar* C) {
        if (C->Overridden()) { C->Reset(); ++N; }
    });
    Out += "reset " + std::to_string(N) + " overridden cvar(s) to defaults";
}
LUR_DEV_COMMAND(GCmdResetCvars, "dev.reset_cvars",
                "Reset every overridden CVar to its compile-time default", DevCmdResetCvars,
                "dev");

inline void DevCmdListOverrides(const DevArgs&, std::string& Out, void*) {
    int N = 0;
    CVarRegistry::ForEach([&](ICVar* C) {
        if (!C->Overridden()) return;
        Out += std::string(C->Name()) + " = " + C->ValueString() +
               " (default " + C->DefaultString() + ")\n";
        ++N;
    });
    if (N == 0) Out += "no cvars overridden";
}
LUR_DEV_COMMAND(GCmdListOverrides, "dev.list_overrides",
                "List every CVar differing from its default, with both values",
                DevCmdListOverrides, "dev");

}  // namespace Lur::Core
#endif  // !LUR_SHIPPING
