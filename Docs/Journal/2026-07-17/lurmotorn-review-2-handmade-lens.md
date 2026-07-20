# LurMotorn — Second Review: The Handmade Lens

*A deliberately different value system from the first review. This one channels the school of thought associated with Jonathan Blow and Casey Muratori — data-oriented, abstraction-skeptical, obsessed with iteration speed and with being able to SEE what the program is doing. I'm applying their well-known public principles ("write games, not engines"; semantic compression; non-pessimization; make code usable before reusable), not putting words in their mouths. Where this review contradicts the first one, that's the point — §8 lays the disagreements side by side so you can arbitrate.*

*Same code was read: master at #38, all modules, the chess game, build system. New facts verified for this pass: zero compiler warning/exception/RTTI flags configured anywhere; 13 `std::function` sites across 5 public headers; GUIDs are `std::string` (Session.h:141); the on-disk record format has no version field; the host build produces unit-test binaries only — no runnable game exists off-device.*

---

## 1. The premise challenge: you are one game in

Before any code: the plan itself deserves the pushback this school is famous for.

You are building a general engine — `IGame`, `GameHost`, pluggable sync models, a lexicon, epics — on the evidence of **one shipped game**, and that game is the single most degenerate case in the genre: one move at a time, one byte per move, no clock, no physics, no animation. Everything you generalize right now, you generalize from a sample size of one, and the abstraction boundaries you draw will encode chess's shape whether you intend it or not. You've already seen this happen *inside your own engine*: `Session::SendMove` hardcodes chess's one-byte move; `EMsgType` grew `Resign` and `DrawOffer`. Those aren't sloppiness — they're what always happens when the "general" layer has one client. The first review catalogued them as leaks to fix. This review says: they are *evidence about method*. The general layer had one client, so it took its client's shape. Adding more interfaces now doesn't fix that dynamic; it gives it more surface to happen on.

The working alternative, straight from this school's playbook: **write game #2 now, badly, in a weekend, on top of whatever exists.** Air hockey, tic-tac-toe with a twist, a two-phone reaction duel — something real-time enough to stress what chess never touches (continuous input, per-tick sends, interpolation). Copy-paste chess's scaffolding shamelessly. *Then* look at the two games side by side and compress what actually repeats — Muratori's semantic compression: abstractions are discovered in real duplication, not designed in anticipation of it. The duplication IS the specification for the engine. Two data points draw a line; one data point draws anything you want.

Concretely, this reorders your roadmap: the `IGame` interface and `GameHost` from the first review are *fine designs* — but they're guesses until game #2 exists. Build the second game first, extract second. The one place this logic **inverts** is the BLE layer, and it's worth being precise about why: there the duplication *already exists* (the same link state machine written twice, Kotlin and ObjC++). Collapsing that is semantic compression of real repetition — this school would do it tomorrow. Pre-abstracting the game layer is speculation; deduplicating the radio layer is compression. Same-looking refactor, opposite epistemic status.

## 2. What this school salutes

Credit where it's genuinely due, because parts of this repo are *exactly* this philosophy executed well:

- **The payload thesis.** "A move costs 0–8 bits, derived from a deterministic legal-move list" is the kind of first-principles data thinking this school preaches. You didn't reach for JSON, protobuf, or a lobby server. You looked at the actual information content of a chess move and shipped that. Most of the industry would have sent a 400-byte packet.
- **Near-zero dependencies.** No middleware, no package manager sprawl, fonts and piece art cooked into headers, a from-scratch bitstream. You own your stack, so you can debug your stack. This is rarer and more valuable than almost anything else in the repo.
- **`MoveList` is the right instinct**: fixed 256-slot array, no heap, sized from a real bound (218 legal moves max). This is the pattern the rest of the codebase should catch up to (§3.3).
- **Library, not framework.** Right now, each app's `main` owns the loop and *calls* the engine. The game is the program; the engine is a toolbox. Guard this. (Note: the first review's `GameHost` drifts framework-ward — see §8.)
- **Comments that state invariants** ("move ordering IS the wire protocol") and a versioned wire changelog. This is engineering, not ritual.
- **Determinism treated as a real constraint** — fixed-point seeds, float quarantine, ordering discipline — before it was needed. That's the one speculative investment this school would bless, because determinism can't be retrofitted.

## 3. The non-pessimization pass

Muratori's term: before "optimization," just *stop doing obviously wasteful things*. None of what follows matters for chess's frame budget. All of it matters because you're pouring the engine's foundation right now, defaults propagate, and your own manifesto says "squeeze as much game out of as few bytes as possible." Currently that philosophy stops at the radio and doesn't reach the allocator.

### 3.1 The `std::filesystem` parable

`Lur::Save::Store` is built on `std::filesystem`. Follow the bill for that convenience: it drags in exception machinery and locale plumbing; it's one of the heaviest headers in the standard library; and on Apple platforms its availability floor **is the reason your deployment target is iOS 13** — which is the reason the oldest iPhone you support is a 6s instead of a 5s/6. One stdlib include chose your hardware floor. The replacement is ~60 lines of `fopen`/`mkdir`/`rename` wrappers you'd own outright, compile instantly, and could take to iOS 11. Whether you *want* iPhone 6 support is your call; the lesson stands regardless: **library choices are product decisions**, and the standard library's defaults were not designed for your values. Audit every std include with that eye.

### 3.2 `std::function` and `std::string` in the plumbing

Thirteen `std::function` members across your core headers (`Transport.h`, `Session.h`, `ChessMatchState.h`, `BoardView.h`), and the peer identity is a heap-allocated 32-char hex `std::string`, compared and copied as such (`Session.h:141`). Each `std::function` is a potential heap allocation, a guaranteed indirect call, a non-inspectable blob in the debugger, and a fat object in your cache lines. Each hex-string GUID is 32+ bytes plus heap header to represent **16 bytes of entropy**.

The Handmade replacements are boring and better: a `Guid` = `struct { uint64_t Hi, Lo; }` (compare = two integer compares; wire = 16 bytes instead of 32; printable on demand); callbacks = plain function pointer + `void* User`, or — since almost every one of these has exactly one subscriber wired at startup — just a direct call to a known function. You are not writing a GUI toolkit with dynamic listeners; you're wiring a fixed pipeline once at boot. Say so in the types.

### 3.3 Game state that can't be `memcpy`'d

`ChessRecord::Moves` is a `std::vector<Move>` — the *canonical game state* heap-allocates and can't be snapshotted, hashed, or diffed without walking it. Meanwhile your own future plans (rollback, desync hashes, replays) all want exactly one property: **state as a flat, fixed-size, trivially-copyable block.** You already know the bound — you cap sync payloads anyway, and the 75-move rule bounds a finished game. `Move Moves[MaxPlies]; uint16_t Count;` makes the entire match state POD: snapshot = `memcpy`, hash = one pass over bytes, save = `fwrite`, rollback = restore a copy. This single change buys more future capability than any interface in the first review, and it deletes allocations instead of adding indirection. Extend the doctrine: **fixed capacity with a justified bound beats dynamic growth** everywhere in the sim. (You did it in `MoveList` already. Finish the thought.)

### 3.4 Interfaces with one implementation

`IRenderer` has exactly one implementation. Virtual dispatch on every `DrawMesh` isn't a chess performance problem — it's a *clarity* problem: an interface with one implementer is not abstraction, it's indirection, and it teaches readers there's a choice where none exists. The platform seam you actually have is **compile-time** (each app links exactly one backend). Prefer link-time polymorphism: declare free functions or a concrete `Renderer` struct in the header; let each platform build supply the implementation. Same for single-subscriber callbacks (§3.2). Keep runtime interfaces where multiple implementations *genuinely coexist at runtime* — `ITransport` legitimately qualifies (loopback in tests, BLE on device, your planned CoC/GATT split). That's the honest dividing line: count the implementations.

### 3.5 You are compiling with no flags

There is no `-Wall`, no `-Werror`, no `-Wextra`, no exceptions/RTTI decision, no explicit optimization configuration **anywhere in the tree** — every target builds on compiler defaults. For a from-scratch engine that owns its whole stack, this is free money left on the table: warnings are the cheapest bug-finder that exists, and an engine with your philosophy should almost certainly build `-fno-exceptions -fno-rtti` (you throw nothing, you `dynamic_cast` nothing — currently you pay for the machinery anyway, and on iOS it's entangled with the §3.1 story). Add a shared `EngineFlags.cmake`: `-Wall -Wextra -Werror`, exceptions/RTTI off, and a defined Debug/Release story. Expect a day of warning cleanup. Worth it the same week.

## 4. You cannot see your program

This is the section your first review underweighted most, and it's the one this school cares about above everything: **iteration speed and observability are the engine.** Your current debug loop for the thing that actually breaks — the link — is: build two apps, deploy to two phones, poke them by hand, read logcat. Minutes per experiment, unreproducible failures, evidence scattered across two devices. Every hard bug in your commit history (#17, #33, #38) was fought in that loop.

### 4.1 The desktop build is the single highest-leverage thing you can do

You have a Vulkan renderer, a pure-C++ sim, and an `ITransport` abstraction with a loopback. You are **one Win32/X11 surface seam and an input shim away** from the actual game running on your PC — two windows, two full game instances, loopback (or UDP-localhost) between them, keyboard/mouse as touch. That turns the build-deploy-two-phones loop into *alt-tab*. It makes every net-flow bug reproducible in a debugger with both peers visible simultaneously. It gives the future stress-test harness, the replay viewer, and the sim fuzzer a place to live. The Vulkan backend already speaks desktop Vulkan dialect-wise; this is days of work, not weeks, and it pays back on every single bug forever after. If you take one thing from this review, take this.

### 4.2 The flight recorder you already almost have

Your architecture's deepest property — *state is derived from the input stream* — is the industry's best debugging tool, and you're not using it as one. Record everything, always, in every build: every datagram in/out, every touch, every link-state transition, timestamped, appended to a ring file. At your data rates this is **bytes per second** — there is no cost argument. A crash or a desync then ships as a file that *replays*: feed it back through the loopback transport on the desktop build and watch the exact failure re-happen, every time, in a debugger. Bug reports stop being logcat archaeology and become artifacts. This composes with everything: the desktop build plays recordings, the stress harness generates them, soak failures save them automatically.

### 4.3 A soak mode for the radio

Your hardest bug class — role collisions, reconnect races, silent link death — only appears across hours of real radio behavior. So make the phones hunt it for you: a burn-in mode where two devices auto-play random legal games continuously, auto-reset, and log/flight-record everything, with a failure counter on screen. Leave them on a shelf overnight. Every morning either the counter says zero or you have N recorded repros. This converts your scarcest resource (hands-on two-phone time) into a machine's job. Note the honest dependency: watchdog/reconnect logic must be *reachable* by tests — which is an argument the first review already made from a different direction (the shared `BleLinkController` over a fake radio). The two reviews agree here; see §8.

### 4.4 Fuzz the attacker-facing surface, and fuzz the sim

`Session::OnDatagram` and the codec chain parse **radio bytes from a peer you don't control**. That surface should survive garbage by construction, and the way to know is to feed it garbage: a host fuzz loop throwing random/mutated buffers at the frame parser and `DecodeGame` (the varint shift issue from review #1 is exactly the class of thing this finds mechanically). Separately, property-test the sim the way you already perft it: millions of random legal games on the host, two instances over loopback, assert byte-identical records and identical results. Ten seconds of CI that guarantees the determinism your whole design leans on.

### 4.5 See it on screen

You built a text renderer — point it at yourself. A debug overlay (toggle: frame ms, link state, ticks since last datagram, peer GUID short-form, send/recv counters) replaces logcat squinting for 80% of on-device questions. Half a day. This school's razor: if you can't see it, you can't fix it, and log files are not seeing.

## 5. Crash loudly, guard quietly — currently you have it backwards

A pattern repeats across the codebase: failure paths **return silently**. `Session::Send` drops oversized payloads and returns void (review #1 found the >61-ply resync failure exactly there — note that bug's shape: *a silent guard*). `EncodeMove` silently encodes move #0 when handed an illegal move. `MoveList::Add` overflows silently. `OnHello` silently stalls on GUID collision. Each silent guard is a bug that will cost hours precisely because it declined to announce itself.

Invert the philosophy: **in development builds, wrongness should be deafening.** One real `LUR_ASSERT` macro (log the expression, the file:line, the values, then trap into the debugger), used aggressively at every "can't happen": encode-of-illegal-move, list overflow, oversized send, unknown message index, GUID self-collision. Release builds keep the quiet guards — a shipped game shouldn't crash on a hostile packet — but dev builds should never *absorb* a contradiction. This costs a day and would have caught, at the keyboard and on the first occurrence, at least three of the issues review #1 had to find by reading.

## 6. Platform reality checks

- **Mali is not Adreno is not Apple.** Your own test Android (A14, Mali-G52) sits on the driver family with the industry's worst Vulkan conformance reputation. Your backend is wisely boring (Vulkan 1.0, FIFO, push constants — the safest possible subset), so you may sail through — but treat *driver bugs* as a first-class bug category now: validation layers on in every dev run on both platforms, and when something renders wrong on one device only, suspect the driver before your code. Budget for one "works on Adreno, broken on Mali" incident before ship; every Vulkan team gets one.
- **Minimize the garbage-collected layer, don't grow it.** Android forces Java-family code for BLE (no NDK API) — fine, that's physics. But every line of Kotlin beyond raw API forwarding is logic in a GC'd runtime you can't step through alongside your C++, with its own threading model (your P0 race lives exactly on that boundary). The shared-first doctrine from review #1 and this school converge perfectly here: Kotlin/ObjC files should be *transcription*, not *thought*.
- **The on-disk format has no version field.** `ChessRecord`'s serialized layout carries no format version (verified — the wire has `ProtocolVersion`; the disk has nothing). The first layout change you ship silently misparses every existing save. One reserved byte today; a migration story before it ever matters. Cheap now, miserable later.

## 7. Where I'd take it next — this school's roadmap

Ordered by leverage-per-effort, and deliberately different in kind from review #1's list:

1. **Desktop build + two-instance loopback harness** (§4.1). Everything else gets faster forever.
2. **Flight recorder + replay** (§4.2). Bugs become files.
3. **`LUR_ASSERT` + `-Wall -Wextra -Werror`, exceptions/RTTI off** (§3.5, §5). One day. Permanent floor-raise.
4. **Game #2, fast and ugly** (§1). A real-time toy that stresses continuous input and per-tick sends. Do this *before* `IGame` — it will redesign `IGame` for you.
5. **POD-ify match state** (§3.3): fixed arrays, trivially copyable, then free `StateHash` and snapshots — which is your desync detection and your rollback substrate, without new architecture.
6. **Soak mode + sim/codec fuzzing** (§4.3, §4.4). Make machines find the radio and determinism bugs.
7. **Byte-budget regression tests.** Your manifesto says the payload is the product — so *test the product*: CI asserts `sizeof(encoded move) ≤ 1 byte`, `hello ≤ N`, `sync(60 plies) ≤ M`. A size regression should fail the build exactly like a logic regression. (This school loves turning values into asserts.)
8. **Guid struct + function-pointer callbacks + filesystem replacement** (§3.1, §3.2) — opportunistic, whenever you touch the files anyway.
9. **Debug overlay** (§4.5). Half a day, pays daily.

Note what's *absent*: no new interfaces, no new modules, no framework. This roadmap is all about tightening the loop between you and the truth of the running program.

## 8. Where the two reviews disagree — and how to arbitrate

The point of this exercise. Both reviews read the same code; they weight values differently.

| Question | Review #1 (architecture lens) | This review (Handmade lens) | Arbitration |
|---|---|---|---|
| `IGame` contract now? | Yes — name the contract, port chess onto it | Not yet — write game #2 first; extract the contract from two real games | These compose: build game #2 *against today's engine*, then do review #1's extraction with two data points. Only the **order** is in dispute — and order is cheap to get right. |
| `GameHost` | Extract the duplicated bootstrap into an engine class | Agree the duplication must die; keep it a *toolbox the game's `main` calls*, not a lifecycle object that calls the game. Library, not framework. | Do the extraction; let this review set its shape (plain struct + functions, game keeps `main`). |
| BLE unification (`BleLinkController`) | Yes — shared C++ policy over thin drivers | **Emphatic yes** — it's compression of *existing* duplication, and it's what makes the soak/fuzz harness possible | Full agreement. Highest-confidence item across both reviews. |
| More `I*` interfaces (`IBleRadio`, `ILog`…) | Interfaces as the unit of design | Count the implementations: multiple-at-runtime → interface (transport: yes). One-per-platform → link-time seam. One total → just code. | Adopt the counting rule; it satisfies both reviews' goals with less indirection. |
| Where's the biggest risk? | Correctness defects + architecture debt (threading race, seam) | **Iteration speed** — no desktop build, no replay, minutes-per-experiment on two phones | Not actually in conflict: fix review #1's P0s this week; build this review's §4 loop next. The P0 threading fix is even a *component* of both (the event queue). |
| stdlib usage | Mostly fine; noted sharp edges | A values mismatch: `filesystem` chose your iOS floor, `function`/`string`/`vector` chose your allocation profile | You wrote "as few bytes as possible" into the brief — this review is holding the code to *your own* stated value. Decide if you meant it below the radio layer too. |
| Error handling | Guard + log the sharp edges found | Systemic inversion: dev builds must trap, not absorb | Complementary — review #1 found instances; this review names the policy that prevents the class. |

**Bottom line from this chair:** the repo's soul — bytes-as-product, zero dependencies, determinism, comments-as-invariants — is *already* this school's philosophy, executed better than most professional teams manage. The gaps are that the philosophy currently stops at the radio (the allocator and the stdlib haven't heard the memo), and that you're flying the two-phone debug loop blind when a desktop build and a flight recorder would give you eyes. Tighten the loop, crash loudly, POD the state, write the second game before the second abstraction — and the engine you extract afterward will be the one the games actually needed, not the one we all guessed at.
