# RPS — Decision Sheet (answer by number, e.g. "1: yes, 2: A, …")

> **ANSWERED 2026-07-20** — 1: A, Pos−Prev variant · 2: A · 3: A · 4: A · 5: A · 6: B · 7: B (lever type TBD — see playbook deliverables) · 8: B, with playtest evidence: mines deplete ~2 min in and carts trek cross-map for the remnants → **a lot more mines** (density, not just capacity) · 9: **struck entirely — workers will never flee**, removed from all planning · 10: C · 11: A. For the living-doc changelog.

*2026-07-20. The open calls that gate agent execution. Each has context, options, and my recommendation (★). Answers get recorded in the living design doc's changelog.*

---

**1. Boid slice B (velocity state) — commit now or evaluate A first?**
Slice A (stateless blend) is hash-neutral and delivers most readability; slice B adds `VelX/VelY` to the SoA + pinned hash and delivers the *fluid* motion you asked for.
A) Commit A→B as consecutive ship cycles ★ (fluidity is a stated goal; B's hash change is routine)
B) Ship A, playtest, decide B after

**2. Enemy separation in slice A?**
Today enemies can fully overlap (the melee pixel-pile). Adding a small cross-team separation spreads engaged fights into readable arcs — but it changes combat feel (fewer units in range simultaneously → slower kills at chokepoints).
A) Yes, in slice A ★ (half the readability win; retune Range/cooldowns if kills slow)
B) Later, as its own knob cycle

**3. H5 targeting fallback — "march toward enemy camp when nothing within KMax".**
This is a perf fix that *changes gameplay*: distant soldiers advance on the enemy base line instead of exact-nearest-enemy pathing across the map. Arguably better behavior (armies push instead of drifting toward stragglers), but it's a design change riding a perf issue.
A) Approve ★  B) Perf-only alternative (cache last target, re-search every K ticks) — no behavior change, less gain

**4. Swapchain-loss policy on device.**
#73 made swapchain death loud; in Development builds that's a deliberate crash on a thermal/driver hiccup.
A) Heal + loud log in on-device Development; trap only in Debugging ★
B) Keep trapping everywhere until #86 is closed (maximal signal, worse playtests)

**5. Everyday phone install becomes optimized-Development.**
Verified: the `installDebug` loop ships -O0 native code. The fix makes optimized-Dev the default and reserves -O0 for explicit Debugging sessions.
A) Yes ★  B) Keep as-is (not recommended — every phone perf datum stays polluted)

**6. Target match length (the #84 north star — everything tunes toward it).**
Current bound: total map gold caps each side at ~216 soldiers lifetime; WorldHeight 240 sets march tempo.
A) Short & fierce: 4–6 min median
B) Standard: 7–10 min median ★ (fits couch sessions; snowball has room to tell a story)
C) Long: 12–15 min + sudden-death cap

**7. Snowball philosophy.**
Stack-accelerated queues + finite gold = games accelerate and leads compound (the design thesis).
A) Protect the thesis: no catch-up mechanics; tune runaway only via gold totals/costs ★
B) Add a soft comeback lever (e.g., cheaper units when army-count behind) — deterministic but dilutes the thesis

**8. Scale ambition vs the gold-bounded economy (a real tension I should flag).**
The engine targets thousands per side; the *economy* caps armies at ~216/side (36 mines × 300 ÷ 50). "Scale as a feature" currently can't be reached by any real match.
A) Intimate scale is the game; the 2048 cap is pure engine headroom ★ (balance #84 at ~100–220 armies)
B) Big-battle ambition is real: raise mine count/capacity toward 500+ armies (re-opens perf + readability work at that density)

**9. Worker flee — in scope for the #84 balance pass?**
A) No — stays a parked flag until boids land ★ (one variable at a time)
B) Yes — raid defense feels bad enough to fix now

**10. #85 attribution surface.**
A) An "About" panel reachable from the opponent selector, listing font/icon notices ★ (one screen, done)
B) Match-end footer line  C) Defer past balance

**11. Naming.**
"Sten Sax Skog" is retired; namespace stays `Rps` regardless.
A) Name after balance, brainstorm shortlist as away-work sometime ★
B) Name now (unblocks store-listing prep)
