# LurMotorn — Issues

Issues are tracked on **GitHub**: <https://github.com/johannesdagerbrant/LurMotorn/issues>

This file is just an in-repo index/pointer to the canonical tracker.

## Open

- **#1** — [Optimize slider attacks with magic bitboards](https://github.com/johannesdagerbrant/LurMotorn/issues/1)
  — performance. Replace the ray-scan sliders in `Games/Chess/Core/Private/Bitboard.h` with magic
  bitboards. Acceptance: perft unchanged, measurably faster, no third-party libraries.
