#!/usr/bin/env python3
"""Split one long WAV of many discrete sounds into one tight WAV per sound.

Given a recording that holds several distinct hits separated by silence (e.g. a take
of a chess piece being placed, over and over), this finds each sound by its silence
boundaries and writes it to its own file.

The point is RESPONSIVENESS. In-game the clip is triggered the instant a move lands, so
the delay between "play" and "the ear hears it" — the clip's leading silence plus its own
attack — must be as close to zero as possible. So each output starts as tightly as we can
manage *just before* the transient, WITHOUT ever clipping its first bit:

  - Detection runs on a short, CENTRED peak envelope (~1 ms). Because it is centred, it
    crosses the threshold slightly BEFORE the first loud sample — the cut leads the onset
    rather than chasing it.
  - The start is pulled back a small `--pre` guard (default 2 ms) into the near-silent
    lead-in. So the file begins at ~0 amplitude (no click) yet the sound is audible within
    ~2-3 ms of playback. That residual is reported per clip as "attack".
  - The tail keeps `--post` ms of decay after the sound falls back to silence (trailing
    silence costs nothing for responsiveness; a chopped release sounds worse).

Output is the canonical format the engine's audio cook accepts — **16-bit PCM, mono**, at
the source sample rate (48 kHz for our SFX; the cook keeps that so the runtime mixer never
resamples). It is the audio counterpart of ImageConvert's "normalise into a cook-acceptable
PNG": AudioSplit sanitises a raw take into per-sound WAVs the cook then encodes to a slim,
lossless embedded header. Override with --bits / --keep-channels if you need the raw take.

Pure host-side content tool (numpy only) — never linked into the app or its build; same
category as Tools/ImageConvert. Handles 16- and 24-bit PCM mono/stereo WAV in and out.

Usage:
  python split-sounds.py INPUT.wav [--out DIR]
  python split-sounds.py INPUT.wav --out DIR --pre 2 --post 60 --min-silence 80
  python split-sounds.py INPUT.wav --normalize        # peak-normalise each clip to -1 dBFS
  python split-sounds.py INPUT.wav --bits 24 --keep-channels   # verbatim depth/channels
  python split-sounds.py INPUT.wav --dry-run          # detect + report, write nothing

Key knobs (all thresholds in dBFS, all times in ms):
  --peak    -35   a region must peak above this to count as a real sound (rejects hiss)
  --floor   -50   envelope level that marks the edge of sound vs. silence
  --min-silence 80   silence gap this long or longer splits two sounds
  --min-sound   15   drop detected blips shorter than this
  --pre          2   guard pulled back before the onset (smaller = tighter attack)
  --post        60   decay tail kept after the sound
"""
import argparse
import struct
import wave
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent.parent  # Tools/AudioSplit/ -> repo root


# ---- WAV I/O (16- and 24-bit PCM; wave's stdlib parser handles the chunk walk) ----------

def read_wav(path):
    """-> (samples float64 [-1,1] shape (frames, channels), sample_rate, sample_width_bytes)."""
    with wave.open(str(path), "rb") as w:
        ch, width, sr, n = (w.getnchannels(), w.getsampwidth(),
                             w.getframerate(), w.getnframes())
        raw = w.readframes(n)
    if width == 2:
        x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 2**15
    elif width == 3:
        b = np.frombuffer(raw, dtype=np.uint8).reshape(-1, 3).astype(np.int32)
        v = b[:, 0] | (b[:, 1] << 8) | (b[:, 2] << 16)
        v = np.where(v & 0x800000, v - 0x1000000, v)          # sign-extend 24 -> 32 bit
        x = v.astype(np.float64) / 2**23
    else:
        raise SystemExit(f"unsupported sample width {width*8}-bit (need 16 or 24)")
    return x.reshape(-1, ch), sr, width


def write_wav(path, samples, sr, width):
    """samples float64 [-1,1] shape (frames, channels) -> PCM WAV at the given bit depth."""
    ch = samples.shape[1]
    if width == 2:
        q = np.clip(np.round(samples * 2**15), -2**15, 2**15 - 1).astype("<i2")
        raw = q.tobytes()
    elif width == 3:
        q = np.clip(np.round(samples * 2**23), -2**23, 2**23 - 1).astype(np.int32)
        q = q.reshape(-1) & 0xFFFFFF
        b = np.empty((q.size, 3), dtype=np.uint8)
        b[:, 0] = q & 0xFF
        b[:, 1] = (q >> 8) & 0xFF
        b[:, 2] = (q >> 16) & 0xFF
        raw = b.tobytes()
    else:
        raise SystemExit(f"unsupported sample width {width*8}-bit (need 16 or 24)")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(ch)
        w.setsampwidth(width)
        w.setframerate(sr)
        w.writeframes(raw)


# ---- detection --------------------------------------------------------------------------

def db_to_lin(db):
    return 10.0 ** (db / 20.0)


def peak_envelope(mono_abs, win):
    """Centred moving-max of |x| over `win` samples. Centred so the envelope crosses a
    threshold slightly BEFORE the true onset — the cut leads the transient, never chases
    it. Implemented as a max over `win` shifted views (win is tiny: ~1 ms)."""
    win = max(1, win)
    n = mono_abs.size
    pad = win // 2
    padded = np.concatenate([np.zeros(pad), mono_abs, np.zeros(win - 1 - pad)])
    out = np.zeros(n)
    for k in range(win):
        out = np.maximum(out, padded[k:k + n])
    return out


def find_segments(env, sr, floor_lin, peak_lin, min_silence, min_sound):
    """Runs of env>floor, merged across silence gaps shorter than `min_silence` frames,
    kept only if they peak above `peak_lin` and last at least `min_sound` frames.
    -> list of (start, end) frame indices on the envelope's own timebase."""
    above = env > floor_lin
    if not above.any():
        return []
    # contiguous [start, end) runs of `above`
    edges = np.diff(above.astype(np.int8))
    starts = list(np.where(edges == 1)[0] + 1)
    ends = list(np.where(edges == -1)[0] + 1)
    if above[0]:
        starts.insert(0, 0)
    if above[-1]:
        ends.append(above.size)
    runs = list(zip(starts, ends))

    # merge runs whose silent gap is shorter than min_silence
    merged = [runs[0]]
    for s, e in runs[1:]:
        if s - merged[-1][1] < min_silence:
            merged[-1] = (merged[-1][0], e)
        else:
            merged.append((s, e))

    return [(s, e) for s, e in merged
            if e - s >= min_sound and env[s:e].max() >= peak_lin]


def refine(seg, env, sr, floor_lin, pre, post, prev_end, next_start, total):
    """Turn an envelope run into the actual cut, tightening the start toward the onset and
    keeping a decay tail. Clamped so neighbouring clips never overlap."""
    s, e = seg
    # walk the start back through the sub-floor lead-in to the true emergence point
    onset = s
    while onset > prev_end and env[onset - 1] > floor_lin * 0.5:
        onset -= 1
    start = max(prev_end, onset - pre)
    end = min(next_start, e + post, total)
    return start, end


# ---- driver -----------------------------------------------------------------------------

def split(path, out_dir, args):
    x, sr, width = read_wav(path)
    frames, ch = x.shape
    mono = np.abs(x).max(axis=1)          # detect on the loudest channel

    env = peak_envelope(mono, int(sr * args.env_ms / 1000))
    floor_lin, peak_lin = db_to_lin(args.floor), db_to_lin(args.peak)
    segs = find_segments(env, sr,
                         floor_lin, peak_lin,
                         int(sr * args.min_silence / 1000),
                         int(sr * args.min_sound / 1000))
    if not segs:
        raise SystemExit("no sounds detected — try lowering --peak / --floor")

    pre, post = int(sr * args.pre / 1000), int(sr * args.post / 1000)
    out_width = width if args.bits == 0 else args.bits // 8   # 0 = keep source depth
    stem = path.stem
    out_dir.mkdir(parents=True, exist_ok=True)

    print(f"{path.name}: {frames/sr:.2f}s, {ch}ch, {width*8}-bit @ {sr} Hz -> "
          f"{len(segs)} sound(s), out {'1ch' if not args.keep_channels else f'{ch}ch'} "
          f"{out_width*8}-bit")
    # 'lead' = leading near-silence before the first audible sample (crosses --floor).
    # It is what THIS tool controls; it should stay at ~--pre, proving a tight cut.
    print(f"{'#':>3}  {'start':>8}  {'dur':>7}  {'peak':>7}  {'lead':>6}  file")

    # resolve each run into a cut, clamping the start to the previous clip's end so
    # neighbouring clips can never overlap
    clips = []
    prev_end = 0
    for i, seg in enumerate(segs):
        next_start = segs[i + 1][0] if i + 1 < len(segs) else frames
        start, end = refine(seg, env, sr, floor_lin, pre, post, prev_end, next_start, frames)
        clips.append((start, end))
        prev_end = end

    written = 0
    for i, (start, end) in enumerate(clips, 1):
        clip = x[start:end].copy()
        if not args.keep_channels and clip.shape[1] > 1:
            clip = clip.mean(axis=1, keepdims=True)          # downmix to mono (cook input)
        if args.normalize:
            pk = np.abs(clip).max()
            if pk > 0:
                clip *= db_to_lin(-1.0) / pk

        seg_mono = mono[start:end]
        audible = seg_mono >= floor_lin
        lead = np.argmax(audible) if audible.any() else 0
        dur_ms = (end - start) / sr * 1000
        lead_ms = lead / sr * 1000
        peak_db = 20 * np.log10(max(np.abs(clip).max(), 1e-9))
        name = f"{stem}_{i:02d}.wav"
        print(f"{i:>3}  {start/sr:>7.3f}s  {dur_ms:>6.1f}ms  {peak_db:>6.1f}dB  "
              f"{lead_ms:>4.1f}ms  {name}")
        if not args.dry_run:
            write_wav(out_dir / name, clip, sr, out_width)
            written += 1

    if args.dry_run:
        print("(dry run — nothing written)")
    else:
        print(f"wrote {written} file(s) to "
              f"{out_dir.relative_to(ROOT) if ROOT in out_dir.parents or out_dir == ROOT else out_dir}")


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("input", help="source WAV")
    ap.add_argument("--out", help="output dir (default: <input-stem>_split/ beside input)")
    ap.add_argument("--peak", type=float, default=-35.0, help="min region peak dBFS")
    ap.add_argument("--floor", type=float, default=-50.0, help="sound/silence edge dBFS")
    ap.add_argument("--min-silence", type=float, default=80.0, help="split gap (ms)")
    ap.add_argument("--min-sound", type=float, default=15.0, help="drop blips shorter (ms)")
    ap.add_argument("--pre", type=float, default=2.0, help="guard before onset (ms)")
    ap.add_argument("--post", type=float, default=60.0, help="decay tail kept (ms)")
    ap.add_argument("--env-ms", type=float, default=1.0, help="peak-envelope window (ms)")
    ap.add_argument("--bits", type=int, choices=(0, 16, 24), default=16,
                    help="output PCM bit depth (16 = cook input; 0 = keep source)")
    ap.add_argument("--keep-channels", action="store_true",
                    help="keep source channels (default: downmix to mono for the cook)")
    ap.add_argument("--normalize", action="store_true", help="peak-normalise each clip to -1 dBFS")
    ap.add_argument("--dry-run", action="store_true", help="detect + report, write nothing")
    a = ap.parse_args()

    src = Path(a.input)
    if not src.exists():
        ap.error(f"no such file: {src}")
    out_dir = Path(a.out) if a.out else src.with_name(f"{src.stem}_split")
    split(src, out_dir, a)


if __name__ == "__main__":
    main()
