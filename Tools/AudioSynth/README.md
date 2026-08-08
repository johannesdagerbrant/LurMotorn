# AudioSynth

Content **author** (not a sanitizer): builds a big, resounding **stinger** out of a small
percussive impact that is already in the game's content set. Written for chess's
checkmate sound (issue #78) — the one event that has to feel unmistakably bigger than the
hundreds of ordinary clicks around it.

Host-only Python + numpy. Never linked into the app or its build; same category as
`AudioSplit` and `ImageConvert`.

## Why derive instead of record

A checkmate stinger is a studio problem, and a borrowed one would arrive with a different
room, a different mic and a different piece of wood than every other sound in the game.
Deriving it from `Capture1.wav` keeps the same character, needs no new source material,
and keeps the licence story to exactly one upstream recording (see
`Games/Chess/Content/Audio/NOTICE.md`).

## The three layers

| Layer | What it contributes |
|---|---|
| **Impact** | the source clip verbatim — the transient, so the stinger still lands *instantly* (the same responsiveness rule `AudioSplit` exists for) |
| **Body** | the same clip resampled down an octave (`--body-rate 0.5`), which stretches it in time as well as pitch: a bigger, slower version of the same object |
| **Tone** | a struck low chord — damped sinusoids on a root and its harmonics, each with its own decay, so the bright partials die first the way a real resonating body's do. This is what makes it *ring* rather than just thud |

Then: Schroeder reverb (4 parallel combs → 2 series allpasses) to put all three in one
room, a high-pass, a saturator, a tail fade, and peak normalisation.

## The part that is easy to get wrong

Peak-normalising the raw mix produces a stinger that **measures** loud and **plays back
quieter than an ordinary move** on a phone. Two reasons, and the two stages that fix them:

- **`--highpass` (150 Hz).** The body layer and the reverb combs pile most of their energy
  below ~100 Hz. A phone speaker cannot move air down there at all, but the peak
  normaliser still spends the whole −1 dBFS budget on it. Measured on the first attempt at
  `--root 55`: 96.8% of the energy sat under 500 Hz, and through a 500 Hz high-pass (a
  crude phone-speaker stand-in) the stinger came out at **−35.0 dBFS rms against a move's
  −22.6** — 12 dB *quieter* than the sound it was supposed to dwarf.
- **`--drive` (3.0).** What is left is one very tall, very short transient spike holding
  the ring several dB below where it belongs. A tanh curve rounds the spike off — the same
  trade a mastering limiter makes — and the body comes up with the normalisation.

With both, the committed asset lands at **−21.9 dBFS** in that band: the loudest, lowest
(383 Hz spectral centroid) and by far the longest (1100 ms vs 123–254 ms) sound in the
set. That is the intended shape, and it is what the flags are tuned against.

If you re-tune by ear, check the same three numbers rather than only the peak — the
docstring's sweep is cheap to repeat.

## Usage

```
# Reproduces the committed chess checkmate byte-for-byte (the defaults ARE the recipe):
python Tools/AudioSynth/make-stinger.py Games/Chess/Content/Audio/Capture1.wav \
    -o Games/Chess/Content/Audio/Checkmate.wav

# A different flavour:
python Tools/AudioSynth/make-stinger.py IMPACT.wav -o OUT.wav --root 110 --length 1400 --drive 4
```

Every knob is a flag with the chess value as its default (`--root 165`, `--tone-gain 0.35`,
`--body-gain 0.70`, `--highpass 150`, `--drive 3.0`, `--length 1100`, `--wet 0.26`,
`--rt60 0.45`, `--fade-out 140`, `--peak-db -1`). Same input + same flags → identical
bytes, so the asset is reproducible rather than a mystery binary.

## Where the output goes next

`AudioSynth` (author) → 16-bit mono WAV in a game's `Content/Audio/` → the **audio cook**
(`Cook/CookAudio.ps1`, driven by the `// LUR_COOK audio` marker in
`Games/Chess/View/Private/SfxLibrary.cpp`) → `SfxClips.h` → `Chess::SfxLibrary` decodes it
once into `Lur::Audio::Mixer`.
