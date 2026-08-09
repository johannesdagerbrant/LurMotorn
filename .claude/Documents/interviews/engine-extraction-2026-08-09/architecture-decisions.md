# Architecture decisions

## Key Points

- BLE: **dedup + move the drifted logic** to shared C++; leave genuine API ceremony in place.
- Coordinator: **templated ring, plain coordinator** — template only where the concrete type is
  structurally required.
- **Replace `std::filesystem`, turn exceptions off**, in this extraction.
- **Engine owns the thread topology**; single-threaded is a config value.
- Flight recorder: **unified engine framework + a 4-function game seam** (user's proposal —
  supersedes the earlier "don't merge" exclusion). **Binary format + a text dump tool.**

---

## BLE unification — dedup + move the drifted logic

One copy per platform, **and** move into shared C++ exactly the parts that have drifted and caused
one-sided bugs: **send queue, retry/backoff, watchdog, role escalation, reconnect scheduling.**
Leave genuine GATT / CoreBluetooth callback ceremony (permission flows, callback shapes) in the
platform file.

**Why this scope and not the full ~600-line diet:** `Modules/Transport` is the most battle-hardened
code in the repo (#83/#146/#163/#182 hardening all lives there); a big-bang rewrite is the
highest-risk item in the entire extraction. This scope is a *bounded* rewrite of precisely the code
whose drift is already causing correctness bugs.

**Two wins, kept distinct:**
- **Deduplication** alone fixes the one-sided-fix class (chess has advertise/scan retry-with-backoff
  RPS lacks; RPS has the #146 deadlock breaker and #182 radio restart chess lacks).
- **Moving logic to C++** additionally makes it host-testable against a `FakeBleRadio` — which is
  the shared-first doctrine's own test: *"could this logic be unit-tested on the host against a
  fake? Then it must live where the host can build it."*

Shape: `BleLinkController` (shared C++ policy) over dumb `IBleRadio` drivers. `IBleRadio` passes the
counting rule — Android, iOS, Windows, **and `FakeBleRadio` which coexists at runtime in tests**,
the same argument that qualifies `ITransport`.

---

## Rollback/lockstep coordinator — split shape

```cpp
// Modules/Net — compiled ONCE, no templates
class RollbackCoordinator {
    void Tick(uint64_t ElapsedNs);
    // recovery, gap repair, resync, anchors, cvar sync
    // — none of this touches Sim
};

// templated ONLY where the concrete type is structurally required
template<class Sim>
struct SnapshotRing {
    static_assert(std::is_trivially_copyable_v<Sim>);
    Sim Slots[Horizon + 2];      // memcpy, not byte-buffer
};

using PhysicsRing = SnapshotRing<Physics::Sim>;
```

**Rationale:** the coordinator's bulk never touches `Sim`. Only `Step`, `StateHash`, and the ring
do. Templating the ring keeps `memcpy` + `static_assert(is_trivially_copyable)` — **the contract
that makes rollback work at all** — while everything else compiles once and stays readable.
A type-erased byte-buffer ring (the runtime-interface option) would have destroyed exactly that
guarantee.

Execution modes on one coordinator: **rollback** (RPS, physics) and **lockstep** (chess, the
degenerate confirmed-only case). Chess gets transport/session/wire/recovery/determinism-harness free
and the mode selector states explicitly that it doesn't need rollback.

---

## `std::filesystem` replacement + exceptions off — IN SCOPE

Replace `std::filesystem` in `Lur::Save` with ~60 lines of `fopen`/`mkdir`/`rename`, then flip
`-fno-exceptions` engine-wide.

**Why now:** the *"retrofit opportunistically when the file is already open"* rule applies exactly —
the App extraction touches save-directory discovery anyway. `EngineFlags.cmake` currently keeps
exceptions on **specifically because** `Lur::Save` throws; review §3.1 traces the iOS 13 deployment
floor (oldest supported iPhone = 6s rather than 5s/6) to that one include. `FlightRecorder` and
`Rps::MatchRecord` already call `std::fopen` directly, so the codebase is half-converted.

Gains: no unwind tables, smaller binaries, potentially a lower iOS floor, and the stdlib stops
choosing product decisions.

---

## Threading — engine owns the topology

The App layer creates the sim thread, the render thread and the mailbox. **Single-threaded is a
config value**, not a different code path. Chess declares single-threaded; RPS and physics declare
sim+render. The game still owns what happens inside `Tick()`.

**Why:** the plumbing is 88 lines on Android and 149 on iOS of pure atomics-and-handshakes, and it
has *already drifted between RPS's own two mains* (`RouteLocalEvent` vs `placeLocal:` — CLAUDE.md
warns "change one, change both"). Critically, the **iOS render thread must be a pthread with a 4 MB
stack** — `std::thread`'s 512 KB overflowed into `SIGBUS` (`___chkstk_darwin`). That is exactly the
class of platform knowledge that must be learned once by the engine, never re-derived per game.

Follows from the entry-point decision: if the engine owns the entry, it should own the thread
topology it starts.

---

## Flight recorder — REVISED: unified framework + game seam

**This supersedes the earlier "exclude: merging the two flight recorders" decision.** The original
objection was that the two recorders capture different layers. The user's framing dissolves it:
**the layers stop being two files and become event CATEGORIES in one stream.** That is strictly
better for debugging — a datagram arriving can be seen interleaved with the sim tick that consumed
it, a correlation that is currently impossible because neither recorder knows the other exists.

**Why an interface works here where it did NOT for the coordinator:** the snapshot ring needs the
concrete `Sim` type because it `memcpy`s POD state — type erasure would cost the
`is_trivially_copyable` contract. **The recorder's payload is already serialized bytes**, so a
virtual `Write(BitWriter&)`/`Read(BitReader&)` seam erases nothing that matters, and a per-tick
virtual call is free next to the I/O.

### Engine owns (free for every game)
- File lifecycle, ring buffer, rotation, atomic writes, timestamps
- **The format version field — this closes #66**, open since July, which applies to *every* game's
  on-disk format
- The replay driver that feeds a recording back through the sim
- Two-peer pull-and-diff tooling (`device-rig pullrec` already does this for RPS; becomes universal)
- **Automatic engine-level events**: datagrams in/out, link up/down, **CVar changes** — RPS records
  CVars because they change sim behaviour, and CVars are already engine-owned, so every game
  inherits that correctness property without knowing it needed it
- The `LUR_INTERNAL` + on-by-default + **visible console checkbox** policy from the revised #156
  doctrine

### Game implements (four functions)
```cpp
struct IMatchRecorder {
    virtual void     WriteEvent(BitWriter&, const void* Ev) = 0;
    virtual bool     ReadEvent(BitReader&, void* Ev)        = 0;
    virtual uint64_t StateHash() const                      = 0;
    virtual void     ApplyOnReplay(const void* Ev)          = 0;
};
```

### Format: **binary + a text dump tool**
Compact stream, with a `--dump` flag printing text for diffing.
**Consequence to plan for:** the diff workflow (`--recdiff`, `device-rig pullrec` auto-diff) is the
primary desync instrument, and it **breaks until the dump tool exists** — so the dump tool ships
*with* the format change, not after it. Grepping a `.rec` by hand on the phone stops being possible.
Chosen over text because the physics game's dense per-tick input would make a text stream wasteful,
and it matches the repo's bytes-are-the-product philosophy.

---

## Open Questions

- `ProtocolVersion` split (engine vs per-game).
- Input pipeline ownership + **multi-touch** (`Lur::Input::TouchEvent` has never grown it, and the
  physics game is a two-hand manipulation game).
- Dead-code sweep.
