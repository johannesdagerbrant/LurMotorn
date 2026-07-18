# Cook

The **cook** turns a game's committed `Content/` into **built data** the app embeds (raw
byte arrays in generated headers), so no image/font decoder ships at runtime. It is a
build-activated process — the build script (`build.ps1`) runs it — **not** a hand-run
tool. (Tools, in `Tools/`, *sanitize* raw content into cook-acceptable formats; the cook
turns that content into built data. Keep the two distinct.)

## Reference-driven & game-agnostic

`Cook.ps1` contains no game-specific knowledge. It derives *what* to cook and *how* from
how the gameplay code **references** its content: each content dependency is declared
inline, next to the code that uses the generated data, with a marker

```
// LUR_COOK <format> src=<paths> out=<header> <format-specific key=value ...>
```

The driver scans `Games/**` for markers, and for each:
- derives that game's `Content/` root from the marker file's own location (`Games/<Game>/`),
- resolves `src=` as **partial paths relative to that `Content/` folder** (order matters —
  it becomes the generated array order),
- resolves `out=` relative to the game root,
- dispatches to the cooker for `<format>` (the format = how the code uses the content).

Example (in `Games/Chess/View/Private/BoardView.cpp`, above the `#include`):

```
// LUR_COOK rg8-shade-coverage src=Pieces/wP.png,Pieces/wN.png,Pieces/wB.png,Pieces/wR.png,Pieces/wQ.png,Pieces/wK.png out=View/Private/PieceMasks.h ns=ChessArt size=PieceMaskSize coverage=PieceCoverage shade=PieceShade
```

## Incremental

Each generated header carries a `// cook-source-hash:` stamp of its ordered sources. A
recipe is up-to-date iff the current source hash matches — robust across git checkouts
(mtimes aren't) and needing no cook tools. Cooked outputs are committed, so a normal
build with unchanged content is a no-op; only changed content re-cooks (on the dev host,
which has the cook tools). Run `Cook/Cook.ps1 -Force` to re-cook everything.

## Formats (cookers)

| `<format>` | Cooker | Produces |
|---|---|---|
| `rg8-shade-coverage` | `CookRg8ShadeCoverage.ps1` | Two parallel byte arrays (shade = Rec.601 luma, coverage = alpha) from an ordered set of grayscale+alpha PNGs; uploaded as an R8G8 texture. |

Add a content type by writing a cooker beside `Cook.ps1` and adding a `case` to the
driver's `Dispatch`. Candidates to fold in next: the font cook (`scripts/gen-font.ps1`)
and the shader cook (`scripts/gen-shaders.ps1`).
