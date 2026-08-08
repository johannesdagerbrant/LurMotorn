# Chess sound effects — provenance & license

Every clip in this folder descends from **one** upstream recording:

Source: **"Chess pieces move close"** by **JJTaynos** — freesound.org sound **733927**
(<https://freesound.org/s/733927/>), kept verbatim in `../Sound/`.

License: **Creative Commons 0 (CC0 1.0, Public Domain Dedication)** — copy, modify,
distribute and perform, **including commercially**, with no attribution required. Safe to
ship on Google Play and the App Store, same bar as the piece art (`../Pieces/NOTICE.md`).
This NOTICE is courtesy, not an obligation.

## How each clip was derived

The 49-second take holds 19 discrete hits. `Tools/AudioSplit` cuts them apart on silence
into tight 16-bit mono WAVs with a ~2 ms attack, peak-normalised to −1 dBFS:

```
python Tools/AudioSplit/split-sounds.py \
    Games/Chess/Content/Sound/733927__jjtaynos__chess-pieces-move-close.wav \
    --out <tmp> --normalize
```

| Clip | Split index | Why that hit |
|---|---|---|
| `Move1.wav` | 14 | the brightest, fullest light hits — 21–31% of their energy below 500 Hz |
| `Move2.wav` | 15 | " |
| `Move3.wav` | 16 | " |
| `Capture1.wav` | 09 | the weightier hits — 46–51% below 500 Hz, so a capture reads heavier than a move by timbre, not just by volume |
| `Capture2.wav` | 10 | " |
| `Capture3.wav` | 07 | " |
| `Check.wav` | 11 | the darkest short hit (1562 Hz centroid) — a distinct, consistent alert |

`Checkmate.wav` is **authored, not recorded**: `Tools/AudioSynth/make-stinger.py` layers
`Capture1.wav` with an octave-down copy of itself and a struck low chord, puts them in one
room, and masters the result into a 1100 ms game-ender. It is reproducible from committed
content alone:

```
python Tools/AudioSynth/make-stinger.py Games/Chess/Content/Audio/Capture1.wav \
    -o Games/Chess/Content/Audio/Checkmate.wav
```

Being a derivative of a CC0 recording plus synthesis, it carries no additional licence
obligations. See `Tools/AudioSynth/README.md` for why the high-pass and saturator stages
exist — without them the stinger measures loud but plays back *quieter* than a move on a
phone speaker.

## What ships

Not these WAVs. The cook (`Cook/CookAudio.ps1`, driven by the `// LUR_COOK audio` marker
in `Games/Chess/View/Private/SfxLibrary.cpp`) compresses them into LSF1 lossless blobs
embedded in `View/Private/SfxClips.h`; the app decodes those once at load. No audio
decoder and no WAV file is ever shipped.
