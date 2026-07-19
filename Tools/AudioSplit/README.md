# AudioSplit

Content sanitizer: split one long WAV of many discrete sounds into one **tight** WAV per
sound, cut on silence. It is the audio counterpart of `ImageConvert` — it normalises a raw
recording into the format the engine's **audio cook** accepts (16-bit PCM, mono, source
rate), so the cook can turn each sound into a slim, lossless embedded header.

Host-only content tool (Python + numpy). Never linked into the app or its build.

## Why this exists — responsiveness

A move sound is triggered the instant the move lands, so total mouth-to-ear delay must be
minimal. That delay is the audio device's callback latency **plus any leading silence baked
into the clip plus the clip's own attack**. This tool kills the middle term: it cuts each
sound as tightly as possible *just before* its onset, **without ever clipping the first bit
of the transient**.

How the near-zero attack is achieved:

- Detection runs on a short (~1 ms) **centred** peak envelope. Because it's centred, the
  envelope crosses the threshold slightly *before* the first loud sample — the cut leads the
  onset instead of chasing it.
- The start is pulled back a small `--pre` guard (default 2 ms) into the near-silent lead-in,
  so each clip **begins at ~0 amplitude** (no click) yet the sound is audible within ~2-3 ms.
- The reported **`lead`** column is exactly this residual leading silence (time to the first
  audible sample). It should stay at ~`--pre` for every clip — proof the cut is tight.
- The tail keeps `--post` ms of decay after the sound (trailing silence costs nothing for
  responsiveness; a chopped release sounds worse).

## Usage

```
python split-sounds.py INPUT.wav [--out DIR]
python split-sounds.py INPUT.wav --dry-run           # detect + report only, write nothing
python split-sounds.py INPUT.wav --normalize         # peak-normalise each clip to -1 dBFS
python split-sounds.py INPUT.wav --bits 24 --keep-channels   # verbatim depth/channels
```

Output defaults to `<input-stem>_split/` beside the input, one `…_NN.wav` per sound, as
**16-bit mono** at the source rate (the audio cook's canonical input).

### Knobs (thresholds in dBFS, times in ms)

| Flag | Default | What it does |
|---|---|---|
| `--peak` | -35 | a region must peak above this to count (rejects hiss / room tone) |
| `--floor` | -50 | envelope level marking the sound/silence edge |
| `--min-silence` | 80 | a silent gap this long or longer splits two sounds |
| `--min-sound` | 15 | drop detected blips shorter than this |
| `--pre` | 2 | guard pulled back before the onset — smaller = tighter attack |
| `--post` | 60 | decay tail kept after the sound |
| `--env-ms` | 1 | peak-envelope window |
| `--bits` | 16 | output depth (`16` = cook input, `24` keep, `0` = match source) |
| `--keep-channels` | off | keep source channels (default downmixes to mono) |
| `--normalize` | off | peak-normalise each clip to -1 dBFS |

**Tuning:** start with `--dry-run` and read the report. Too many tiny fragments → raise
`--min-silence` or `--min-sound`. Soft real sounds being dropped → lower `--peak`/`--floor`.
Any `lead` noticeably above `--pre` means a soft pre-cursor sits above `--floor`; usually
fine, lower `--floor` if you want it tighter.

## Where the output goes next

`AudioSplit` (sanitize) → per-sound 16-bit mono WAV → **audio cook** (encode to a slim
lossless header) → `Modules/Audio` loads + decodes once → the mixer plays it over AAudio
(Android) / RemoteIO (iOS). See `Cook/README.md` and `Modules/Audio/`.
