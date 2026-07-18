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
| `ImageConvert/` | General image → engine-valid channel-format converter (`convert-image.py`): normalises any source image into a **2-channel** grayscale+alpha PNG (for an R8G8 cook; the shade channel is selectable — luma or a single R/G/B) or a **4-channel** RGBA PNG (for an R8G8B8A8 cook). Chess pieces are one preset. |
| `ChessPieceCook/` | Cooks the 2-channel piece PNGs from `Games/Chess/Content/Pieces/` into `Games/Chess/View/Private/PieceMasks.h` — an embedded R8G8 (shade+coverage) byte stream, so no image decoder ships in the app (`gen-piece-masks.ps1`). |

Conventions:
- One subfolder per tool, PascalCase, with its own `README.md`.
- Toolchain stays **VS-free**: host C++ builds with MinGW g++; the BLE rig's C# builds
  with the .NET Framework `csc.exe` that ships with Windows (no Visual Studio, no SDK
  install, no license).
- Built binaries and downloaded caches are gitignored (`Tools/**/*.exe`, etc.); only
  source is committed.
