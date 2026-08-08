#!/usr/bin/env python3
"""Build a big, resounding STINGER from a small percussive impact.

A game's rare, game-ending event (chess checkmate) wants a sound that is unmistakably
bigger than the hundreds of ordinary ones around it. Recording one is a studio problem;
this instead *derives* it from an impact clip that is already in the content set, so the
stinger keeps the same wood-and-room character as every other sound in the game and
needs no new source material.

Three layers, summed:

  1. IMPACT   the source clip verbatim — the transient, so the stinger still lands
              instantly (the same responsiveness rule AudioSplit exists for).
  2. BODY     the same clip resampled DOWN an octave (`--body-rate`), which stretches it
              in time as well as pitch: a bigger, slower version of the same object.
  3. TONE     a struck low chord — damped sinusoids on a root and its harmonics, each
              with its own decay, so the high partials die first the way a real
              resonating body's do. This is what makes it *ring* rather than just thud.

Then a Schroeder reverb (4 parallel combs -> 2 series allpasses) puts all three in one
room, a high-pass drops the sub-bass no phone speaker can move, a tanh saturator trades
the transient's peak for body, and the result is faded out and peak-normalised.

Those last two steps are the ones that decide whether the stinger actually SOUNDS bigger
on the device. Peak-normalising the raw mix puts the whole -1 dBFS budget on one sharp
transient and on inaudible sub-bass, which measures loud and plays back *quieter than an
ordinary move* through a phone speaker. See the README for the numbers.

Everything is deterministic: same input + same flags => byte-identical output, and the
defaults ARE the recipe for the committed chess checkmate. Output is the audio cook's
canonical input format — 16-bit PCM mono at the source rate.

Pure host-side content tool (numpy only) — never linked into the app or its build; same
category as Tools/AudioSplit and Tools/ImageConvert.

Usage:
  python make-stinger.py Games/Chess/Content/Audio/Capture1.wav \
      -o Games/Chess/Content/Audio/Checkmate.wav        # reproduces the committed asset
  python make-stinger.py IMPACT.wav -o OUT.wav --root 110 --length 1400 --drive 4
"""
import argparse
import wave
from pathlib import Path

import numpy as np


# ---- WAV I/O (16-bit PCM mono in, 16-bit PCM mono out) -----------------------------

def read_wav(path):
    """-> (mono float64 [-1,1], sample_rate)."""
    with wave.open(str(path), "rb") as w:
        ch, width, sr, n = (w.getnchannels(), w.getsampwidth(),
                            w.getframerate(), w.getnframes())
        raw = w.readframes(n)
    if width != 2:
        raise SystemExit(f"{path}: need 16-bit PCM (got {width*8}-bit) — run it through AudioSplit first")
    x = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 2**15
    return x.reshape(-1, ch).mean(axis=1), sr


def write_wav(path, x, sr):
    q = np.clip(np.round(x * 2**15), -2**15, 2**15 - 1).astype("<i2")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sr)
        w.writeframes(q.tobytes())


# ---- layers ------------------------------------------------------------------------

def resample(x, rate):
    """Play `x` back at `rate` (0.5 = an octave down and twice as long), linear-interp."""
    n = int(len(x) / rate)
    pos = np.arange(n) * rate
    i0 = np.floor(pos).astype(int)
    i1 = np.minimum(i0 + 1, len(x) - 1)
    f = pos - i0
    return x[i0] * (1 - f) + x[i1] * f


def struck_tone(sr, n, root, partials, decays, gains, fade_in_ms=3.0):
    """A struck, resonating low chord: one damped sinusoid per partial. Each partial
    gets its own decay so the bright ones fall away first and the fundamental hangs on —
    which is what the ear reads as a big body ringing rather than a synth note."""
    t = np.arange(n) / sr
    out = np.zeros(n)
    for mult, dec, g in zip(partials, decays, gains):
        out += g * np.sin(2 * np.pi * root * mult * t) * np.exp(-t / dec)
    # Short fade-in: a sinusoid starting at full amplitude is a click.
    k = max(1, int(sr * fade_in_ms / 1000))
    out[:k] *= np.linspace(0.0, 1.0, k)
    return out


def schroeder_reverb(x, sr, wet, rt60):
    """4 parallel feedback combs into 2 series allpasses — the classic cheap room. Just
    enough to glue the three layers into one space; not a hall.

    Comb feedback is solved from the wanted RT60 rather than guessed: a comb of delay d
    loses 20*log10(g) dB per pass, so g = 10^(-3d/RT60) puts it 60 dB down after RT60."""
    comb_ms = (29.7, 37.1, 41.1, 43.7)      # mutually prime-ish, so no flutter echo
    allpass_ms = (5.0, 1.7)

    acc = np.zeros(len(x))
    for ms in comb_ms:
        d = max(1, int(sr * ms / 1000))
        g = 10.0 ** (-3.0 * (ms / 1000.0) / rt60)
        y = np.copy(x)
        for i in range(d, len(y)):
            y[i] += g * y[i - d]
        acc += y / len(comb_ms)

    for ms in allpass_ms:
        d = max(1, int(sr * ms / 1000))
        g = 0.7
        y = np.zeros(len(acc))
        for i in range(len(acc)):          # y[n] = -g*x[n] + x[n-d] + g*y[n-d]
            y[i] = -g * acc[i]
            if i >= d:
                y[i] += acc[i - d] + g * y[i - d]
        acc = y

    return (1.0 - wet) * x + wet * acc


def highpass(x, sr, f0, q=0.7071):
    """RBJ biquad high-pass, run as a plain difference equation (no circular wrap).

    This is not a tone choice, it is a HEADROOM choice. The body layer and the reverb's
    combs pile a lot of energy below ~80 Hz, which no phone speaker can move — but the
    peak normaliser still spends the whole -1 dBFS budget on it, pushing the part you
    can actually hear down with it. Removing the inaudible bottom lets the audible band
    come up by several dB at the same peak."""
    w0 = 2.0 * np.pi * f0 / sr
    alpha = np.sin(w0) / (2.0 * q)
    c = np.cos(w0)
    b = np.array([(1 + c) / 2, -(1 + c), (1 + c) / 2])
    a = np.array([1 + alpha, -2 * c, 1 - alpha])
    b /= a[0]
    a /= a[0]
    y = np.zeros(len(x))
    for n in range(len(x)):
        y[n] = (b[0] * x[n]
                + (b[1] * x[n - 1] + a[1] * -y[n - 1] if n >= 1 else 0.0)
                + (b[2] * x[n - 2] + a[2] * -y[n - 2] if n >= 2 else 0.0))
    return y


def build(src, args):
    x, sr = read_wav(src)
    n = int(sr * args.length / 1000)

    def pad(v):
        out = np.zeros(n)
        m = min(len(v), n)
        out[:m] = v[:m]
        return out

    impact = pad(x) * args.impact_gain
    body = pad(resample(x, args.body_rate)) * args.body_gain
    tone = struck_tone(
        sr, n, args.root,
        partials=(1.0, 1.5, 2.0, 3.0, 4.0),
        decays=(args.tone_decay, args.tone_decay * 0.8, args.tone_decay * 0.62,
                args.tone_decay * 0.45, args.tone_decay * 0.32),
        gains=(1.0, 0.55, 0.42, 0.22, 0.12),
    ) * args.tone_gain

    mix = impact + body + tone
    mix = schroeder_reverb(mix, sr, args.wet, args.rt60)
    if args.highpass > 0:
        mix = highpass(mix, sr, args.highpass)

    # Soft-saturate before normalising. The impact's transient is one very tall, very
    # short spike; left alone it owns the entire -1 dBFS budget and holds the ring — the
    # part that makes the sound RESOUND — several dB below where it should sit. Rounding
    # the spike off with a tanh curve is the same trade a mastering limiter makes: a
    # little of the click's edge, for a much bigger body.
    if args.drive > 0:
        peak = np.abs(mix).max()
        if peak > 0:
            mix = np.tanh(args.drive * mix / peak) / np.tanh(args.drive)

    # Fade the tail to true zero — a stinger that stops mid-ring sounds broken, and any
    # residual DC at the cut is an audible click.
    k = max(1, int(sr * args.fade_out / 1000))
    mix[-k:] *= np.linspace(1.0, 0.0, k) ** 2

    peak = np.abs(mix).max()
    if peak > 0:
        mix *= 10.0 ** (args.peak_db / 20.0) / peak

    rms = np.sqrt((mix**2).mean())
    print(f"{src.name} -> {args.length:.0f} ms stinger  "
          f"peak {20*np.log10(np.abs(mix).max()):.1f} dBFS  rms {20*np.log10(rms):.1f} dBFS  @ {sr} Hz")
    return mix, sr


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("input", help="source impact WAV (16-bit PCM)")
    ap.add_argument("-o", "--out", required=True, help="output WAV")
    ap.add_argument("--length", type=float, default=1100.0, help="total length (ms)")
    ap.add_argument("--root", type=float, default=165.0, help="tone fundamental (Hz)")
    ap.add_argument("--tone-decay", type=float, default=0.85, help="fundamental decay time constant (s)")
    ap.add_argument("--body-rate", type=float, default=0.5, help="playback rate of the body layer")
    ap.add_argument("--impact-gain", type=float, default=1.0)
    ap.add_argument("--body-gain", type=float, default=0.70)
    ap.add_argument("--tone-gain", type=float, default=0.35)
    ap.add_argument("--wet", type=float, default=0.26, help="reverb wet mix 0..1")
    ap.add_argument("--rt60", type=float, default=0.45, help="reverb decay time (s)")
    ap.add_argument("--highpass", type=float, default=150.0,
                    help="drop sub-bass a phone speaker cannot move, in Hz (0 = off)")
    ap.add_argument("--drive", type=float, default=3.0,
                    help="tanh saturation before normalising — trades transient peak for body (0 = off)")
    ap.add_argument("--fade-out", type=float, default=140.0, help="tail fade (ms)")
    ap.add_argument("--peak-db", type=float, default=-1.0, help="peak-normalise target (dBFS)")
    a = ap.parse_args()

    src = Path(a.input)
    if not src.exists():
        ap.error(f"no such file: {src}")
    mix, sr = build(src, a)
    out = Path(a.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    write_wav(out, mix, sr)
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
