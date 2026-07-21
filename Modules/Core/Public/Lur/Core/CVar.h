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
    virtual const char* Category() const  = 0;
    virtual uint32_t    Flags() const     = 0;
    virtual ECVarOrigin Origin() const    = 0;
    virtual bool        AffectsGameplay() const = 0;
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

    constexpr T Get() const noexcept { return Default_; }
    constexpr operator T() const noexcept { return Default_; }
#else
    CVar(const char* Name, T Default, uint32_t Flags = CVarFlagNone, const char* Category = "",
         ECVarOrigin Origin = ECVarOrigin::Game)
        : Default_(Default), Value_(Default), Name_(Name), Category_(Category), Flags_(Flags),
          Origin_(Origin) {}

    T Get() const noexcept {
        LUR_ASSERT_MSG(GCVarMainEntered, "CVar '%s' read before main()", Name_);
        return Value_;
    }
    operator T() const noexcept { return Get(); }

    // ---- ICVar (dev-only introspection / mutation for console, panel, cvars.cfg) ----
    const char* Name() const override { return Name_; }
    const char* Category() const override { return Category_; }
    uint32_t    Flags() const override { return Flags_; }
    ECVarOrigin Origin() const override { return Origin_; }
    bool        AffectsGameplay() const override { return (Flags_ & CVarFlagAffectsGameplay) != 0; }
    bool        SetFromString(const char* S) override {
        T Parsed{};
        if (!FromString(S, Parsed)) return false;  // unqualified: ADL finds Sim's Fixed overload
        Value_ = Parsed;
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

    // Typed accessors for code that holds the concrete CVar (not through ICVar).
    T    Default() const { return Default_; }
    void Set(T V) { Value_ = V; }
#endif

private:
    T Default_;
#if !LUR_SHIPPING
    T           Value_;
    const char* Name_;
    const char* Category_;
    uint32_t    Flags_;
    ECVarOrigin Origin_;
    uint64_t    EditWallMs_ = 0;
#endif
};

// CTAD: deduce CVar<T> from the default value, ignoring trailing (flags/category/origin)
// args — one guide covers both the shipping (2-arg) and dev (up to 5-arg) forms.
template <class T, class... A>
CVar(const char*, T, A...) -> CVar<T>;

}  // namespace Lur::Core

// LUR_CVAR(Var, "name", Default, Flags, "Category") — the ONE way to declare a CVar.
//   Dev:      inline mutable CVar + a separate registrar static (so the console/panel/
//             cvars.cfg can find it by name) + a compile-time float-gameplay ban.
//   Shipping: JUST the constant-initialized value (flags/category/registrar vanish),
//             satisfying §1.1's structural condition for the zero-overhead fold.
// The macro has no trailing ';' — call sites write `LUR_CVAR(...);`.
#if LUR_SHIPPING
    #define LUR_CVAR(Var, Name, Default, Flags, Category) \
        inline constexpr ::Lur::Core::CVar Var { Name, Default }
#else
    #define LUR_CVAR(Var, Name, Default, Flags, Category)                                   \
        static_assert(!(::std::is_same_v<::std::decay_t<decltype(Default)>, float> &&       \
                        (((Flags) & ::Lur::Core::CVarFlagAffectsGameplay) != 0)),           \
                      "AffectsGameplay CVar may not be float (determinism, spec §1): " Name); \
        inline ::Lur::Core::CVar Var { Name, Default, (Flags), Category };                   \
        inline const ::Lur::Core::CVarRegistrar Var##_Reg { Var }
#endif

// LUR_CVAR_ENGINE — identical, but tags the CVar as engine-origin for the panel's
// Engine/Game split (Addendum D.3). Only engine modules use it; games use LUR_CVAR.
#if LUR_SHIPPING
    #define LUR_CVAR_ENGINE(Var, Name, Default, Flags, Category) \
        inline constexpr ::Lur::Core::CVar Var { Name, Default }
#else
    #define LUR_CVAR_ENGINE(Var, Name, Default, Flags, Category)                            \
        static_assert(!(::std::is_same_v<::std::decay_t<decltype(Default)>, float> &&       \
                        (((Flags) & ::Lur::Core::CVarFlagAffectsGameplay) != 0)),           \
                      "AffectsGameplay CVar may not be float (determinism, spec §1): " Name); \
        inline ::Lur::Core::CVar Var { Name, Default, (Flags), Category,                     \
                                       ::Lur::Core::ECVarOrigin::Engine };                   \
        inline const ::Lur::Core::CVarRegistrar Var##_Reg { Var }
#endif
