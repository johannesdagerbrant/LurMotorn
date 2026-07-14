# YAGNI pass + layout scope

**Date:** 2026-07-14 · Interview round 4

## CUT from v1 (add later on the same architecture)
- **Kerning application** — cook may emit kerning data, runtime ignores it in v1 (negligible HUD gain).
- **Auto-shrink-to-fit** — no binary-search-on-scale in v1; #22 text fits its field.
- **Ellipsis truncation** — v1 uses clip + dev overflow warning instead.

## KEPT in v1
- **Cook TWO fonts:** default UI font (Inter or similar OFL) **and** the OFL **DSEG** 7-/14-segment
  clock font — both via the offline `msdf-atlas-gen` cook. This un-defers the clock font (cheap now that
  it's just another MSDF cook, not the old analytic-rectangle special case) and proves the multi-font
  `FontRegistry` seam in v1. The clock *UI* itself still waits for speed chess; v1 only cooks +
  registers + smoke-renders the font.
- Greedy word-wrap + character-break fallback; text measurement; L/C/R + vertical alignment.
- `OverflowX/OverflowY` flags + **dev-build overflow warning** (red field outline + one-shot log,
  behind a `TextDebugOverflow` flag).
- Multi-font architecture (one-atlas-per-font, `FontRegistry` → `FontHandle`).
