#pragma once
// Lur::App::IGame — the game contract (#45, extraction Phase 7).
//
// ── What problem this solves ─────────────────────────────────────────────────
// #43 moved the engine's session + persistence choreography into GameHost, and the split it left is
// clean: GameHost::Config is entirely PLATFORM (save dir, transport, log sink) while Hooks and
// RecordSync are entirely GAME. But nothing said WHERE a game answers the game half, so each game
// answered it once per platform — in its Android main, its iOS main and its desktop main.
//
// Measured at f7b7ac1, chess: 11 / 10 / 11 wiring touchpoints across the three, with bodies that were
// identical apart from how they captured state:
//
//     Rec.Summarize = [&State] {        // Android
//     Rec.Summarize = [M = &_Match] {   // iOS
//     Rec.Summarize = [&G] {            // Desktop
//
// That is the same drift shape #43's own comment recorded when it consolidated the previous round
// ("ownership, capture style, wiring order, and a MATCH END line whose format differed per phone"),
// re-formed one level up. 3 copies x 2 games = 6; game 3 makes 9. Each copy is somewhere a hook can
// be FORGOTTEN rather than wrong, which is the failure mode a build cannot catch.
//
// ── Why it is ONE method ─────────────────────────────────────────────────────
// The temptation is a lifecycle interface — CreateResources, Tick, Render, OnPause. Resist it: the
// two games disagree about all of those in ways that are essential, not accidental. Chess renders on
// the platform thread from a turn-based state; RPS owns a dedicated render thread (#183) and a
// separate 10 Hz sim thread with rollback. An interface spanning both would either be fat or would
// force one of them to lie.
//
// What they DO agree on is exactly this: after the host exists and before it starts, the game states
// its answers. So that is the whole contract, and the evidence it must stay this thin is that RPS's
// implementation is nearly empty — no RecordSync at all (its ScoreBook is not an ISaveState and never
// crosses the wire) and no StateHash (it detects divergence itself with per-tick anchors). A contract
// that made RPS stub three methods to say "I don't do records" would be worse than the six copies.
//
// Where the two games disagree, that difference is the specification for an engine facility neither
// should own (CLAUDE.md). Here they agree on one thing, so the engine owns one thing.
#include "Lur/App/GameHost.h"

namespace Lur::App {

class IGame {
public:
    virtual ~IGame() = default;

    // State the game's own decisions on the host. Called by the platform shell AFTER Host.Init()
    // and BEFORE Host.Start(), which is the window GameHost's phase comment describes: the Store
    // and device id exist, so the game can hand them to its view, and the session is not live yet,
    // so nothing can call back into a view that has not been attached.
    //
    // Fill `Hooks` rather than calling Start yourself — the shell owns the lifecycle. Call
    // Host.EnableRecordSync() here if the game has a per-opponent record; skip it if it does not.
    virtual void Configure(GameHost& Host, GameHost::Hooks& Hooks) = 0;
};

}  // namespace Lur::App
