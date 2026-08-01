#pragma once
// Lur::Core::CVar<T> — a named override of a compile-time-default value, the mechanism
// that "decides everything else" in the dev-tools spec (dev-console-cvar-tech-spec.md
// §1, §1.1). ONE expression at every call site, all build configs:
//
//     x += Rps::CvMinerSpeed.Get();      // or just `Rps::CvMinerSpeed` via operator T
//
// The two-worlds split lives ENTIRELY inside this class, never at call sites (§0 point 2):
//
//   * LUR_SHIPPING : Get() returns the raw constexpr Default_. The object has ZERO members
//                    other than Default_ (registry/override/metadata are all #if'd out), so
//                    the optimizer folds `CvFoo.Get()` to the literal — identical codegen to
//                    the old `constexpr`. This is the shipping contract; a disassembly-diff
//                    CI check (spec §7) is the enforcement.
//   * else (dev)   : Get() returns Value_ (Default_ unless overridden this session). The CVar
//                    is a polymorphic ICVar in an intrusive registry so the console/panel/
//                    cvars.cfg can enumerate + set it by name.
//
// Declare CVars ONLY through the LUR_CVAR macro (below): it guarantees registration and
// makes the shipping shape structural, not optimizer-luck.
//
// NAMING NOTE: the spec illustrates flags as `CVarFlags::AffectsGameplay`; per the house
// rule (unscoped bitmask enum values carry the concept prefix) they are ECVarFlags with a
// CVarFlag* prefix — `CVarFlagAffectsGameplay`. Same meaning.
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>

#include "Lur/Core/Assert.h"
#include "Lur/Core/FromString.h"

namespace Lur::Core {

// Bitmask of CVar properties, set at registration. AffectsGameplay is the sync boundary
// (Addendum C.0): only gameplay CVars are latched, synced, hashed. Default is none —
// the safe default is "local" (a forgotten flag = "my tweak didn't sync", a visible
// annoyance, never a silent desync).
enum ECVarFlags : uint32_t {
    CVarFlagNone            = 0,
    CVarFlagAffectsGameplay = 1u << 0,
};

// Panel top-level grouping (Addendum D.3). Engine-DERIVED, never game-set: engine-module
// registrations pass Engine; a game just registers and lands under Game automatically.
enum class ECVarOrigin : uint8_t { Game = 0, Engine = 1 };

#if !LUR_SHIPPING
// Type-erased handle the registry stores. Non-virtual, protected dtor: the registry only
// ever holds pointers to static-duration CVars and never deletes through the base.
class ICVar {
public:
    virtual const char* Name() const     = 0;
    // One-line help shown by the console's per-CVar "i" button. ALWAYS present: LUR_CVAR takes it
    // as a required argument and static_asserts it non-empty, so no CVar can reach the console
    // undocumented and the "i" button never has to render an inert/greyed state.
    virtual const char* Tooltip() const   = 0;
    virtual uint32_t    Flags() const     = 0;
    virtual ECVarOrigin Origin() const    = 0;
    virtual bool        AffectsGameplay() const = 0;
    // Is this a BOOLEAN knob? The console needs it to pick an editor: a bool row toggles on tap,
    // every other type opens the numpad. Type-erased because the console only ever sees ICVar.
    virtual bool        IsBool() const = 0;
    virtual bool        SetFromString(const char* S) = 0;  // false on parse failure
    virtual void        Reset() = 0;                       // back to the compile-time default
    virtual bool        Overridden() const = 0;
    virtual std::string ValueString() const = 0;           // current value, console/persist syntax
    virtual std::string DefaultString() const = 0;
    // Wall-clock (ms) of the last edit — the last-writer-wins resolver key (Addendum C.2)
    // and the cvars.cfg timestamp column (C.4). 0 = never stamped (loses any real edit).
    virtual uint64_t    EditWallMs() const = 0;
    virtual void        SetEditWallMs(uint64_t Ms) = 0;
    // Current value as a raw int32 for the wire (Fixed.Raw / int / enum ordinal / bool),
    // so a type-erased edit can be handed to LockstepPeer::SetGameplayCvar without knowing T.
    virtual int32_t     RawValue() const = 0;
    // Move the value by Steps (+1/-1 per arrow-key press, #119). The STEP SIZE is per-type
    // and chosen by the concrete CVar, because that is the only thing that knows T: a bool
    // flips, an int moves by 1, a Fixed by 1/64 of a unit, a float by 0.01. Returns false
    // (leaving the value untouched) for a type with no sensible nudge — an enum, whose valid
    // range this layer cannot know, would otherwise scrub to a value that isn't a member.
    virtual bool        Nudge(int Steps) = 0;
    // ---- Optional declared range (#116) ----
    // Advisory for the UI and ENFORCED on commit: a value outside [Min,Max] is clamped rather
    // than rejected, so a fat-fingered "1000" on a 0..1 weight lands at 1 instead of wedging a
    // match. Only numeric types can carry one. HasRange() false = unbounded (the common case).
    virtual bool        HasRange() const = 0;
    // Range and current value as floats, for widgets that must not know T (the slider).
    virtual float       RangeMinF() const = 0;
    virtual float       RangeMaxF() const = 0;
    virtual float       ValueF() const = 0;
    // ---- 4-channel colour knob (#117) ----
    // The console opens the PICKER popover for these instead of the numpad — a colour is four
    // numbers and typing them one at a time through a numpad is not editing a colour.
    // Duck-typed on the member names inside CVar<T> (R/G/B/A), which is what lets Core answer
    // "is this a colour" without ever knowing Render::Color exists.
    virtual bool        IsColor() const = 0;
    virtual bool        GetColorChannels(float Out[4]) const = 0;
    virtual bool        SetColorChannels(const float In[4]) = 0;

    ICVar* NextRegistered_ = nullptr;  // intrusive singly-linked registry list

protected:
    ~ICVar() = default;
};

// The registry: a Meyers-singleton list head so registration is static-init-order
// independent across TUs (a registry only needs the SET, not an order). Dev-only.
class CVarRegistry {
public:
    static ICVar*& Head() {
        static ICVar* H = nullptr;
        return H;
    }
    static void Register(ICVar* V) {
        // A duplicate name is a bug (identity must be unique — Addendum C.0.1). Catch it
        // loudly at startup rather than silently shadowing.
        for (ICVar* C = Head(); C; C = C->NextRegistered_)
            LUR_ASSERT_MSG(std::strcmp(C->Name(), V->Name()) != 0, "duplicate CVar name: %s",
                           V->Name());
        V->NextRegistered_ = Head();
        Head() = V;
    }
    static ICVar* Find(const char* Name) {
        for (ICVar* C = Head(); C; C = C->NextRegistered_)
            if (std::strcmp(C->Name(), Name) == 0) return C;
        return nullptr;
    }
    template <class Fn>
    static void ForEach(Fn&& F) {
        for (ICVar* C = Head(); C; C = C->NextRegistered_) F(C);
    }
};

// Separate dev-only static whose ctor registers the CVar — NEVER a CVar-ctor side effect
// (§1.1), so the CVar itself stays trivially constant-initialized data.
struct CVarRegistrar {
    explicit CVarRegistrar(ICVar& V) { CVarRegistry::Register(&V); }
};

// "No CVar value read before main()" guard (§1.1). A read before main would touch the
// dev override state during static init, where order is unspecified. Host mains / tests
// call CVarEnterMain() first; Get() asserts on it in dev.
inline bool GCVarMainEntered = false;
inline void CVarEnterMain() { GCVarMainEntered = true; }
#endif  // !LUR_SHIPPING

template <class T>
class CVar
#if !LUR_SHIPPING
    final : public ICVar
#endif
{
public:
#if LUR_SHIPPING
    // Shipping: store ONLY Default_. The extra dev args are dropped by the macro, so the
    // object is a pure value the optimizer folds. constexpr ctor => constant-initialized.
    constexpr CVar(const char* /*Name*/, T Default) noexcept : Default_(Default) {}
    // Ranged form: min/max are dev-only metadata, so shipping keeps just the default and the
    // object stays the single value the optimizer folds (§1.1's structural condition).
    constexpr CVar(const char* /*Name*/, T Default, T /*Min*/, T /*Max*/) noexcept
        : Default_(Default) {}

    constexpr T Get() const noexcept { return Default_; }
    constexpr operator T() const noexcept { return Default_; }
#else
    CVar(const char* Name, T Default, uint32_t Flags = CVarFlagNone,
         const char* Tooltip = nullptr, ECVarOrigin Origin = ECVarOrigin::Game)
        : Default_(Default), Value_(Default), Name_(Name),
          Tooltip_(Tooltip), Flags_(Flags), Origin_(Origin) {}

    // Ranged form (#116). Min/Max are inclusive and enforced on every commit path.
    CVar(const char* Name, T Default, T Min, T Max, uint32_t Flags,
         const char* Tooltip, ECVarOrigin Origin = ECVarOrigin::Game)
        : Default_(Default), Value_(Default), Name_(Name), Tooltip_(Tooltip), Flags_(Flags),
          Origin_(Origin), Min_(Min), Max_(Max), HasRange_(true) {}

    T Get() const noexcept {
        LUR_ASSERT_MSG(GCVarMainEntered, "CVar '%s' read before main()", Name_);
        return Value_;
    }
    operator T() const noexcept { return Get(); }

    // ---- ICVar (dev-only introspection / mutation for console, panel, cvars.cfg) ----
    const char* Name() const override { return Name_; }
    const char* Tooltip() const override { return Tooltip_ ? Tooltip_ : ""; }
    uint32_t    Flags() const override { return Flags_; }
    ECVarOrigin Origin() const override { return Origin_; }
    bool        AffectsGameplay() const override { return (Flags_ & CVarFlagAffectsGameplay) != 0; }
    bool        IsBool() const override { return std::is_same_v<T, bool>; }
    bool        SetFromString(const char* S) override {
        T Parsed{};
        if (!FromString(S, Parsed)) return false;  // unqualified: ADL finds Sim's Fixed overload
        Value_ = Clamped(Parsed);
        return true;
    }
    void        Reset() override { Value_ = Default_; }
    bool        Overridden() const override { return !(Value_ == Default_); }
    std::string ValueString() const override { return ToString(Value_); }
    std::string DefaultString() const override { return ToString(Default_); }
    uint64_t    EditWallMs() const override { return EditWallMs_; }
    void        SetEditWallMs(uint64_t Ms) override { EditWallMs_ = Ms; }
    int32_t     RawValue() const override {
        if constexpr (std::is_enum_v<T>) return static_cast<int32_t>(Value_);
        else if constexpr (std::is_same_v<T, bool>) return Value_ ? 1 : 0;
        else if constexpr (std::is_integral_v<T>) return static_cast<int32_t>(Value_);
        else if constexpr (requires(const T& V) { V.Raw; }) return Value_.Raw;  // Fixed-like
        else return 0;  // float (never AffectsGameplay) — not sent on the wire
    }
    // ---- Colour (#117) ----
    // Detected structurally, not by naming a type: Core must not depend on Render.
    static constexpr bool IsColorType = requires(const T& V) { V.R; V.G; V.B; V.A; };
    bool IsColor() const override { return IsColorType; }
    bool GetColorChannels(float Out[4]) const override {
        if constexpr (IsColorType) {
            Out[0] = Value_.R; Out[1] = Value_.G; Out[2] = Value_.B; Out[3] = Value_.A;
            return true;
        } else { (void)Out; return false; }
    }
    bool SetColorChannels(const float In[4]) override {
        if constexpr (IsColorType) {
            Value_.R = In[0]; Value_.G = In[1]; Value_.B = In[2]; Value_.A = In[3];
            return true;
        } else { (void)In; return false; }
    }

    // ---- Range (#116) ----
    bool  HasRange() const override { return HasRange_; }
    float RangeMinF() const override { return AsFloat(Min_); }
    float RangeMaxF() const override { return AsFloat(Max_); }
    float ValueF() const override { return AsFloat(Value_); }

    bool Nudge(int Steps) override {
        if (Steps == 0) return true;
        // bool FIRST: it is integral, so the integral branch would otherwise claim it and
        // "increment" a bool, which lands on true and then stays there.
        if constexpr (std::is_same_v<T, bool>) { Value_ = !Value_; return true; }
        else if constexpr (std::is_enum_v<T>) { return false; }  // no known valid range
        else if constexpr (std::is_integral_v<T>) {
            Value_ = Clamped(static_cast<T>(Value_ + static_cast<T>(Steps)));
            return true;
        } else if constexpr (requires { T::One; } && requires(const T& V) { V.Raw; }) {
            // Fixed: 1/64 of a unit. Fine enough to feel out a flock weight, coarse enough
            // that holding the key visibly moves the sim.
            T Next = Value_;
            Next.Raw += Steps * (T::One / 64);
            Value_ = Clamped(Next);
            return true;
        } else if constexpr (std::is_floating_point_v<T>) {
            Value_ = Clamped(static_cast<T>(Value_ + static_cast<T>(Steps) * static_cast<T>(0.01)));
            return true;
        } else {
            return false;
        }
    }

    // Typed accessors for code that holds the concrete CVar (not through ICVar).
    T    Default() const { return Default_; }
    void Set(T V) { Value_ = Clamped(V); }
#endif

private:
#if !LUR_SHIPPING
    // Clamp to the declared range, if there is one. Comparison uses operator< so it works for
    // int, float AND Fixed (which defines the relationals) without a per-type branch; a type
    // with no ordering (Color) can never have HasRange_ set, because the ranged ctor is only
    // reachable through LUR_CVAR_RANGE and that is documented numeric-only.
    T Clamped(T V) const {
        if constexpr (std::is_same_v<T, bool> || std::is_enum_v<T>) {
            return V;  // nothing to clamp against
        } else if constexpr (requires(const T& A, const T& B) { A < B; }) {
            if (!HasRange_) return V;
            if (V < Min_) return Min_;
            if (Max_ < V) return Max_;
            return V;
        } else {
            return V;  // unordered (Color): ranges are not offered
        }
    }

    // One value as a float, for widgets that must not know T. Fixed divides out its scale so a
    // slider sees 0.75 rather than 49152 — a knob positioned by raw units would be unusable.
    static float AsFloat(const T& V) {
        if constexpr (std::is_same_v<T, bool>) return V ? 1.0f : 0.0f;
        else if constexpr (std::is_enum_v<T>) return static_cast<float>(static_cast<int>(V));
        else if constexpr (requires { T::One; } && requires(const T& X) { X.Raw; })
            return static_cast<float>(V.Raw) / static_cast<float>(T::One);
        else if constexpr (std::is_arithmetic_v<T>) return static_cast<float>(V);
        else return 0.0f;  // Color and friends: no single scalar
    }
#endif

    T Default_;
#if !LUR_SHIPPING
    T           Value_;
    const char* Name_;
    const char* Tooltip_;
    uint32_t    Flags_;
    ECVarOrigin Origin_;
    uint64_t    EditWallMs_ = 0;
    T           Min_{};
    T           Max_{};
    bool        HasRange_ = false;
#endif
};

// CTAD: deduce CVar<T> from the default value, ignoring trailing (flags/tooltip/origin)
// args — one guide covers both the shipping (2-arg) and dev (up to 5-arg) forms.
template <class T, class... A>
CVar(const char*, T, A...) -> CVar<T>;

}  // namespace Lur::Core

// LUR_CVAR(Var, "name", Default, Flags, "Description") — the ONE way to declare a CVar. The
// dotted name IS the hierarchy: the console groups cvars into a tree by splitting the name on '.'
// (so "rps.boid.sep_strength" nests under rps -> boid), which is why there is no separate category.
//   Dev:      inline mutable CVar + a separate registrar static (so the console/panel/
//             cvars.cfg can find it by name) + a compile-time float-gameplay ban.
//   Shipping: JUST the constant-initialized value (flags/registrar/description vanish),
//             satisfying §1.1's structural condition for the zero-overhead fold.
// The macro has no trailing ';' — call sites write `LUR_CVAR(...);`.
//
// THE DESCRIPTION IS MANDATORY, and that is enforced by the compiler two ways: it is a required
// macro argument (omit it and the arity is wrong), and the static_assert below rejects an EMPTY
// one. A knob nobody can explain is a knob nobody can tune — the console's per-CVar "i" button
// shows this string, and it is the only place a tuner learns what a name like "w_coh_all" means.
// There is deliberately NO tooltip-less form: the previous LUR_CVAR/LUR_CVAR_T split let 19 of
// them ship undocumented, with the explanation stranded in a trailing // comment the console
// could never show.
#if LUR_SHIPPING
    #define LUR_CVAR(Var, Name, Default, Flags, Tooltip)                                      \
        static_assert(sizeof(Tooltip) > 1, "every CVar needs a description: " Name);           \
        inline constexpr ::Lur::Core::CVar Var { Name, Default }
#else
    #define LUR_CVAR(Var, Name, Default, Flags, Tooltip)                                      \
        static_assert(!(::std::is_same_v<::std::decay_t<decltype(Default)>, float> &&          \
                        (((Flags) & ::Lur::Core::CVarFlagAffectsGameplay) != 0)),              \
                      "AffectsGameplay CVar may not be float (determinism, spec §1): " Name);  \
        static_assert(sizeof(Tooltip) > 1, "every CVar needs a description: " Name);           \
        inline ::Lur::Core::CVar Var { Name, Default, (Flags), (Tooltip) };                    \
        inline const ::Lur::Core::CVarRegistrar Var##_Reg { Var }
#endif

// LUR_CVAR_RANGE(Var, "name", Default, Min, Max, Flags, "Description") — a CVar declaring an
// inclusive [Min,Max]. NUMERIC TYPES ONLY (int / float / Fixed): the range is enforced by
// clamping every commit path (console, keyboard scrub, cvars.cfg load, peer sync), and the
// console renders a slider for it instead of a bare field.
//
// Clamping rather than rejecting is deliberate. A tuner typing 1000 into a 0..1 weight wants
// "as high as it goes", not an error toast and an unchanged value — and on a phone, where the
// only editor is a numpad, an extra keystroke is easy and a rejected commit is invisible.
// Out-of-range is still ALLOWED to be expressed (the console warns), it just lands at the bound.
#if LUR_SHIPPING
    #define LUR_CVAR_RANGE(Var, Name, Default, Min, Max, Flags, Tooltip)                      \
        static_assert(sizeof(Tooltip) > 1, "every CVar needs a description: " Name);           \
        inline constexpr ::Lur::Core::CVar Var { Name, Default, Min, Max }
#else
    #define LUR_CVAR_RANGE(Var, Name, Default, Min, Max, Flags, Tooltip)                      \
        static_assert(!(::std::is_same_v<::std::decay_t<decltype(Default)>, float> &&          \
                        (((Flags) & ::Lur::Core::CVarFlagAffectsGameplay) != 0)),              \
                      "AffectsGameplay CVar may not be float (determinism, spec §1): " Name);  \
        static_assert(sizeof(Tooltip) > 1, "every CVar needs a description: " Name);           \
        inline ::Lur::Core::CVar Var { Name, Default, Min, Max, (Flags), (Tooltip) };          \
        inline const ::Lur::Core::CVarRegistrar Var##_Reg { Var }
#endif

// LUR_CVAR_ENGINE — identical, but tags the CVar as engine-origin for the panel's
// Engine/Game split (Addendum D.3). Only engine modules use it; games use LUR_CVAR.
#if LUR_SHIPPING
    #define LUR_CVAR_ENGINE(Var, Name, Default, Flags, Tooltip)                               \
        static_assert(sizeof(Tooltip) > 1, "every CVar needs a description: " Name);           \
        inline constexpr ::Lur::Core::CVar Var { Name, Default }
#else
    #define LUR_CVAR_ENGINE(Var, Name, Default, Flags, Tooltip)                               \
        static_assert(!(::std::is_same_v<::std::decay_t<decltype(Default)>, float> &&          \
                        (((Flags) & ::Lur::Core::CVarFlagAffectsGameplay) != 0)),              \
                      "AffectsGameplay CVar may not be float (determinism, spec §1): " Name);  \
        static_assert(sizeof(Tooltip) > 1, "every CVar needs a description: " Name);           \
        inline ::Lur::Core::CVar Var { Name, Default, (Flags), (Tooltip),                      \
                                       ::Lur::Core::ECVarOrigin::Engine };                     \
        inline const ::Lur::Core::CVarRegistrar Var##_Reg { Var }
#endif
