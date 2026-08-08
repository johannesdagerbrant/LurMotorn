#pragma once

namespace Chess {

// What a move SOUNDED like — the category BoardView classifies each applied move into,
// and the only thing it says about audio.
//
// This lives in its own dependency-free header on purpose. BoardView must be able to
// name the event without pulling in the mixer (it renders and takes taps; it owns no
// audio), and SfxLibrary must map the same names to clips. One enum, two users, no
// audio dependency leaking into the view's public header.
//
// Precedence when a move is several things at once (a capture that gives mate is a
// Checkmate): Checkmate > Check > Capture > Move.
enum class EMoveSound {
    Move,       // a quiet move — the common case, and the one that must never repeat itself
    Capture,    // something was taken
    Check,      // the move gives check (capture or not)
    Checkmate,  // the move ends the match
};

} // namespace Chess
