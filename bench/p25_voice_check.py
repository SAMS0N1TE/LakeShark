#!/usr/bin/env python3
"""Replay real P25 captures through the production voice path and refuse a
change that plays less of them than before.

    python bench/p25_voice_check.py                 # check every capture
    python bench/p25_voice_check.py --update        # accept the current result
    python bench/p25_voice_check.py --wav out/      # and write what it heard

Captures live in bench/captures/p25/ (or $LS_P25_CAPTURES) and never in git:
they are somebody's radio traffic. Each is the pair 'p25 capture save' writes
on the board - <name>.iq (u8 IQ at 240 kHz) and <name>.txt (rate, demod
gain) - fetched with the Wi-Fi file page. <name>.expect records what the
decoder recovered the last time a person listened and agreed.

P25 voice broke repeatedly from changes that were not about P25: a decoder
that passes every synthetic case can still mute a third of a real call. This
measures the thing the operator hears - IMBE frames that reach the speaker,
out of the frames on the air - on traffic that really happened, so the next
regression is a number on the bench instead of a report from the field.

With no captures present it says so and passes: the bench must still run on
a checkout that has none.
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "build", "p25_voice_replay.exe" if os.name == "nt" else "p25_voice_replay")
DEFAULT_DIR = os.path.join(HERE, "captures", "p25")

# A frame's worth of slack: sync timing on the host is deterministic, but a
# harmless change to the demodulator may move one hunt by a symbol.
SLACK_FRAMES = 9


def sidecar(path):
    meta = {}
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            if "=" in line:
                k, v = line.strip().split("=", 1)
                meta[k] = v
    return meta


def replay(iq, meta, wav=None):
    if meta.get("rate", "240000") != "240000":
        raise RuntimeError("%s: rate %s, the replay needs 240000" % (iq, meta.get("rate")))
    cmd = [TOOL, iq, meta.get("demod_gain", "-9000")]
    if wav:
        cmd.append(wav)
    out = subprocess.run(cmd, capture_output=True, text=True, check=False)
    m = re.search(r"^P25VOICE (.*)$", out.stdout, re.M)
    if out.returncode or not m:
        raise RuntimeError("%s: replay failed\n%s%s" % (iq, out.stdout[-800:], out.stderr[-800:]))
    fields = {}
    for kv in m.group(1).split():
        k, v = kv.split("=", 1)
        fields[k] = float(v) if "." in v else int(v)
    return fields


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=os.environ.get("LS_P25_CAPTURES", DEFAULT_DIR))
    ap.add_argument("--update", action="store_true", help="record the current result as the baseline")
    ap.add_argument("--wav", metavar="DIR", help="write each capture's decoded voice as a WAV")
    a = ap.parse_args()

    names = sorted(f[:-3] for f in os.listdir(a.dir) if f.endswith(".iq")) if os.path.isdir(a.dir) else []
    if not names:
        print("p25 voice: no captures in %s - nothing to check" % a.dir)
        return 0
    if not os.path.exists(TOOL):
        print("p25 voice: %s is not built" % TOOL)
        return 1
    if a.wav:
        os.makedirs(a.wav, exist_ok=True)

    failed = 0
    for name in names:
        base = os.path.join(a.dir, name)
        meta = sidecar(base + ".txt") if os.path.exists(base + ".txt") else {}
        wav = os.path.join(a.wav, name + ".wav") if a.wav else None
        got = replay(base + ".iq", meta, wav)
        line = ("%-28s played %5d of %5d IMBE frames (%5.1f%%)  missed-sync %3d LDUs  "
                "held %4d released %4d discarded %4d muted %4d" %
                (name, got["imbe_played"], got["imbe_on_air"], got["played_pct"], got["ldu_missed"],
                 got["held"], got["released"], got["discarded"], got["muted"]))
        expect_path = base + ".expect"
        if a.update or not os.path.exists(expect_path):
            with open(expect_path, "w", encoding="utf-8") as fh:
                json.dump({"imbe_played": got["imbe_played"], "imbe_on_air": got["imbe_on_air"],
                           "replay": got}, fh, indent=1)
            print("  new  " + line)
            continue
        want = json.load(open(expect_path, encoding="utf-8"))["imbe_played"]
        if got["imbe_played"] + SLACK_FRAMES < want:
            failed += 1
            print("  FAIL " + line + "   baseline %d" % want)
        else:
            gain = got["imbe_played"] - want
            print("  ok   " + line + ("   (+%d over baseline - run --update to keep it)" % gain
                                     if gain > SLACK_FRAMES else ""))
    if failed:
        print("p25 voice: %d capture(s) now play less voice than their baseline" % failed)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
