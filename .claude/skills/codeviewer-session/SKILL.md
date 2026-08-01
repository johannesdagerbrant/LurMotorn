---
name: codeviewer-session
description: Author a CodeViewer walkthrough session for this repo — the base_dir relocation recipe and the anchor rules. Use when creating or revising a .codeviewer file, or when the user asks for a focused walkthrough of a completed phase.
---

# Authoring a CodeViewer session in LurMotorn

Sessions live in `CodeViewerSessions/`. The create tool cannot write them there directly with working
code paths, so authoring is a two-step move:

1. Call the create tool with `directory` = **the repo root**, so code paths validate while you author.
   It writes `<slug>.codeviewer` there with `base_dir: "."`.
2. Move the file into `CodeViewerSessions/` and patch `base_dir` from `"."` to `".."`, so code paths
   still resolve to the repo root from its new location.

Rules that bite:

- Use `symbol` or `anchor_start`/`anchor_end` ranges, not raw line numbers — the repo moves under them.
- **Anchors are regex**: avoid parentheses in anchor strings (this is also in `CLAUDE.md`'s gotchas).
- Author a `lexicon` — a full newline-separated list, one term per line, not an inline paragraph.

Do **not** treat a CodeViewer session as a gate on anything: create one only when the user asks for a
walkthrough. Never block a commit or push on it.
