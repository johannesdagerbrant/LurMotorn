#pragma once
// Lur::DevGui::DevTheme — the ONE place the dev layer's colours live (#113).
//
// Deliberately ANTI-GAME. The console paints over a running match, and if it shared the
// game's palette a reader could not tell at a glance which pixels are the game and which are
// the tool. So: flat charcoal panels, no gradients, a single cold cyan accent, and ink that
// is grey rather than any team colour. RPS's teams are cyan/yellow (#142) — the accent here
// is deliberately lighter and desaturated against team cyan so a highlighted row never reads
// as "team 0 owns this".
//
// These were literals scattered through GameView.cpp: three materials built at resource time
// and four Color locals declared inside the draw block, ~300 lines apart, with the same values
// re-typed. That is how a theme drifts — one gets tweaked, the others don't, and the panel
// stops looking like one surface. Values live here; GameView draws with them.
//
// Plain constexpr, no registration: a theme the console can retint at runtime is a feature
// nobody asked for, and CVar<Color> (#116) is the honest way to get it if we ever do.
#include "Lur/Render/Renderer.h"

namespace Lur::DevGui::DevTheme {

using Lur::Render::Color;

// ---- Surfaces ----
// Translucent, not opaque: you must be able to see the sim moving behind the console while
// tuning it, which is most of the point of tuning live.
inline constexpr Color Panel{0.08f, 0.08f, 0.08f, 0.88f};
// Key/row face — a touch lighter than Panel so a control reads as raised without a border.
inline constexpr Color KeyFace{0.20f, 0.22f, 0.24f, 0.98f};

// ---- Accent ----
// The single highlight: selected row marker, Enter key, scrollbar thumb, overridden values.
inline constexpr Color Accent{0.55f, 0.98f, 0.90f, 1.0f};
// The material-side accent (scroll thumb, selection bar) is marginally deeper so a large
// filled area doesn't glare next to accent-coloured text.
inline constexpr Color AccentFill{0.25f, 0.95f, 0.85f, 1.0f};
// Ink for text ON an accent fill — near-black, since the accent is a light colour.
inline constexpr Color AccentInk{0.04f, 0.07f, 0.07f, 1.0f};

// ---- Ink ----
inline constexpr Color Ink{0.86f, 0.90f, 0.92f, 1.0f};      // normal row text
inline constexpr Color CatInk{0.62f, 0.72f, 0.78f, 1.0f};   // category headers: quieter than rows
inline constexpr Color DimInk{0.40f, 0.45f, 0.48f, 1.0f};   // inert affordances (a tooltip-less "i")
inline constexpr Color Warn{0.94f, 0.52f, 0.46f, 1.0f};     // destructive/cancel

}  // namespace Lur::DevGui::DevTheme
