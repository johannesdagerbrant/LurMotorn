# Tools

Dev-host utilities for **debugging** and for **sanitizing content** into formats the cook
can handle — the things that are **not part of the shipped engine** but matter for its
long-term development. They live here (committed, one well-named subfolder per tool) so
they don't get lost in a local temp folder.

**Tools vs Cook.** A *tool* sanitizes raw content into a format the cook accepts (e.g.
an arbitrary image → a 2- or 4-channel PNG). The *cook* (`Cook/`, a separate build-
activated process) turns that content into built data the app embeds. Tools are hand-run
while authoring; the cook runs from the build. Keep them distinct — don't put cook steps
here.

Nothing here is ever linked into the app or its CMake build. These are host-only
instruments (Windows dev machine / CI).

| Subfolder | What it is |
|---|---|
| `BleDevRig/` | The Windows↔Android BLE dev rig: a VS-free WinRT (C#) BLE central that speaks the LurMotorn GATT protocol to an unmodified Android app, so the desktop can be a real second peer for developing/debugging/optimizing the Bluetooth networking (issue #58). |
| `ImageConvert/` | Content sanitizer: normalises any source image into a cook-acceptable PNG — a **2-channel** grayscale+alpha PNG (for an R8G8 cook; shade channel selectable: luma or a single R/G/B) or a **4-channel** RGBA PNG (for an R8G8B8A8 cook). Chess pieces are one preset. |

Conventions:
- One subfolder per tool, PascalCase, with its own `README.md`.
- Toolchain stays **VS-free**: host C++ builds with MinGW g++; the BLE rig's C# builds
  with the .NET Framework `csc.exe` that ships with Windows (no Visual Studio, no SDK
  install, no license).
- Built binaries and downloaded caches are gitignored (`Tools/**/*.exe`, etc.); only
  source is committed.
