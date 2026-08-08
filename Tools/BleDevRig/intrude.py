"""#83 acceptance harness — THE THIRD DEVICE.

Drives BleRadio.exe as a hostile outsider against a live two-phone pair. From the phones'
point of view this PC is indistinguishable from a third phone walking into the room: the
radio scans for the game's service, connects unpaired, reads the device-id characteristic,
SUBSCRIBES to the datagram characteristic (hole 1, the hijack) and then writes datagrams at
a steady cadence (hole 2, the injection). Killing it at the end exercises hole 3 — an
outsider's departure must not end the pair's match.

Acceptance is read off the PHONES, never off this script: both must hold
`LOCKSTEP ... desync=0`, neither may lose its link, and each should log
`IGNORING subscribe from a non-bound central (#83)`.

Two things this exists to get right, both of which cost a run when done by hand:

* BleRadio's stdin pump calls Environment.Exit(0) on EOF, so launching it without a live
  stdin makes it die on startup — and the empty log reads as "the rig never found a peer"
  rather than "the rig was never alive".
* A linked phone stops advertising, so the radio can only get a handle if it is ALREADY
  scanning when the pair (re)forms. Start this BEFORE relaunching the apps.

    python Tools/BleDevRig/intrude.py --seconds 120            # RPS (default)
    python Tools/BleDevRig/intrude.py --service <uuid>         # chess = ...7370
"""
import argparse
import os
import random
import struct
import subprocess
import sys
import threading
import time

RPS_SERVICE = "4C55524D-4F54-4F52-4E00-5472616E7371"
HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    Ap = argparse.ArgumentParser()
    Ap.add_argument("--service", default=RPS_SERVICE)
    # The address of the phone to intrude on, hex, no separators (e.g. 6C2A1D01637F). Without it
    # the radio SCANS — and a linked phone advertises no more, so a scan can only ever find a phone
    # that has no partner yet, i.e. you become its partner instead of intruding on a pair. Learn
    # both addresses first with `BleScan.exe <service-uuid>` while the apps are up but unlinked.
    Ap.add_argument("--addr", default=None)
    # Seconds to hold the connection before subscribing. THIS is the reachable intrusion: connect
    # during the handshake window (when both phones advertise), stay silent while they link, then
    # subscribe onto the live pair. A linked phone is not connectable, so connecting late fails.
    Ap.add_argument("--subscribe-delay", type=int, default=0)
    Ap.add_argument("--seconds", type=int, default=120)
    Ap.add_argument("--write-every-ms", type=int, default=500)
    Ap.add_argument("--log", default=os.path.join(os.getcwd(), ".logs83", "rig.err"))
    Args = Ap.parse_args()

    os.makedirs(os.path.dirname(Args.log), exist_ok=True)
    Radio = os.path.join(HERE, "BleRadio.exe")
    if not os.path.exists(Radio):
        sys.exit("BleRadio.exe missing — run Tools/BleDevRig/build.ps1")

    Log = open(Args.log, "wb")
    # BleRadio's argv is positional (service, address, delay), so the address slot has to be
    # filled even when we only want a delay. An all-zero address means "no pin, scan normally".
    Argv = [Radio, Args.service]
    if Args.addr or Args.subscribe_delay:
        Argv.append(Args.addr or "000000000000")
    if Args.subscribe_delay:
        Argv.append(str(Args.subscribe_delay))
    Proc = subprocess.Popen(
        Argv,
        stdin=subprocess.PIPE,
        stdout=subprocess.DEVNULL,   # framed binary relay; nobody is listening
        stderr=Log,
    )
    print(f"[intrude] BleRadio pid={Proc.pid} svc={Args.service} -> {Args.log}", flush=True)

    Rand = random.Random(1337)
    Writes = 0
    Deadline = time.time() + Args.seconds
    try:
        while time.time() < Deadline and Proc.poll() is None:
            # One garbage datagram: 'D' + 4-byte LE length + random payload. The radio drops it
            # with "no link" until it has subscribed; after that it writes it to the phone, where
            # the peer binding decides whether the engine ever sees it.
            N = Rand.randint(16, 32)
            Payload = bytes(Rand.randrange(256) for _ in range(N))
            try:
                Proc.stdin.write(b"D" + struct.pack("<I", N) + Payload)
                Proc.stdin.flush()
            except (BrokenPipeError, OSError):
                break
            Writes += 1
            time.sleep(Args.write_every_ms / 1000.0)
    finally:
        Elapsed = int(Args.seconds - max(0.0, Deadline - time.time()))
        print(f"[intrude] wrote {Writes} garbage datagrams over {Elapsed}s", flush=True)
        try:
            Proc.stdin.close()
        except OSError:
            pass
        try:
            Proc.wait(timeout=3)
        except subprocess.TimeoutExpired:
            Proc.kill()
        Log.close()
        print(f"[intrude] radio log -> {Args.log}", flush=True)


if __name__ == "__main__":
    main()
