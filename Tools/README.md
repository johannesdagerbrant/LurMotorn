# Tools

Dev-host utilities for building, debugging, and authoring content for LurMotorn — the
things that are **not part of the shipped engine** but are important for its long-term
development. They live here (committed, one well-named subfolder per tool) so they don't
get lost in a local temp folder.

Nothing here is ever linked into the app or its CMake build. These are host-only
instruments (Windows dev machine / CI).

| Subfolder | What it is |
|---|---|
| `BleDevRig/` | The Windows↔Android BLE dev rig: a VS-free WinRT (C#) BLE central that speaks the LurMotorn GATT protocol to an unmodified Android app, so the desktop can be a real second peer for developing/debugging/optimizing the Bluetooth networking (issue #58). |
| `ChessPieceArt/` | One-off content-authoring scripts that rasterise the chess piece art into `Games/Chess/Content/Pieces/` (`gen-piece-pngs.py`) and cook it into `PieceMasks.h` (`gen-piece-masks.ps1`). |

Conventions:
- One subfolder per tool, PascalCase, with its own `README.md`.
- Toolchain stays **VS-free**: host C++ builds with MinGW g++; the BLE rig's C# builds
  with the .NET Framework `csc.exe` that ships with Windows (no Visual Studio, no SDK
  install, no license).
- Built binaries and downloaded caches are gitignored (`Tools/**/*.exe`, etc.); only
  source is committed.
