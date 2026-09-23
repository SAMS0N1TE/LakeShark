"""Fetch the public DSD-FME regression capture and replay it outside the source tree."""
import argparse
import array
import hashlib
import io
import os
from pathlib import Path
import re
import subprocess
import sys
import urllib.request
import wave
import zipfile

URL = "https://github.com/user-attachments/files/22107330/p25p1andp2.zip"
SHA256 = "136bdb57d08c8ed4a9dd7b5f8a2e7476f995b60aafb45de27a0189fd66ce253b"


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--replay", type=Path)
    args = parser.parse_args()
    repo = Path(__file__).resolve().parents[2]
    cache = args.cache.resolve()
    if cache == repo or repo in cache.parents:
        parser.error("keep the recording outside the source tree")
    cache.mkdir(parents=True, exist_ok=True)
    capture = cache / "p25p1andp2.bin"
    if not capture.exists():
        with urllib.request.urlopen(URL, timeout=60) as response:
            archive = zipfile.ZipFile(io.BytesIO(response.read()))
        names = [n for n in archive.namelist() if n.endswith("p25p1andp2.bin")]
        if len(names) != 1:
            raise RuntimeError("unexpected archive")
        capture.write_bytes(archive.read(names[0]))
    raw = capture.read_bytes()
    if hashlib.sha256(raw).hexdigest() != SHA256:
        raise RuntimeError("capture hash mismatch")
    executable = args.replay or repo / "bench/build" / ("p25p2_replay.exe" if sys.platform == "win32" else "p25p2_replay")
    pcm = cache / "phase2.s16"
    result = subprocess.run([str(executable), str(capture), str(pcm), "92715", "1f6", "01a", "0"],
                            text=True, capture_output=True, check=True)
    print(result.stdout.strip())
    counts = {k: int(v) for k, v in re.findall(r"(symbols|voice|audio|muted)=(\d+)", result.stdout)}
    if counts != {"symbols": 416245, "voice": 1368, "audio": 1214, "muted": 154}:
        raise RuntimeError(f"unexpected replay counts: {counts}")
    data = pcm.read_bytes()
    samples = array.array("h", data)
    if sys.byteorder != "little":
        samples.byteswap()
    if len(samples) != 1368 * 160 or not any(samples):
        raise RuntimeError("missing PCM")
    with wave.open(str(cache / "phase2.wav"), "wb") as output:
        output.setparams((1, 2, 8000, 0, "NONE", "not compressed"))
        output.writeframes(data)
    print(f"REPLAY OK: peak={max(abs(s) for s in samples)}, WAV={cache / 'phase2.wav'}")
    negative = cache / "wrong-system.s16"
    result = subprocess.run([str(executable), str(capture), str(negative), "12345", "123", "123", "0"],
                            text=True, capture_output=True)
    if result.returncode != 1 or "audio=0 " not in result.stdout or any(negative.read_bytes()):
        raise RuntimeError("wrong-system negative control produced audio")
    print("Wrong-system negative control: muted")
    # The same capture as a receiver of the other polarity delivers it: every
    # dibit's high bit flipped. OP25 answers the reversed sync by inverting
    # what follows; before the adapter did, this framed bursts and decoded
    # no voice at all.
    inverted = cache / "p25p1andp2-inverted.bin"
    inverted.write_bytes(bytes(b ^ 2 for b in raw))
    inverted_pcm = cache / "phase2-inverted.s16"
    result = subprocess.run([str(executable), str(inverted), str(inverted_pcm), "92715", "1f6", "01a", "0"],
                            text=True, capture_output=True, check=True)
    counts = {k: int(v) for k, v in re.findall(r"(symbols|voice|audio|muted)=(\d+)", result.stdout)}
    if counts != {"symbols": 416245, "voice": 1368, "audio": 1214, "muted": 154}:
        raise RuntimeError(f"inverted polarity replay counts: {counts}")
    if inverted_pcm.read_bytes() != data:
        raise RuntimeError("inverted polarity decoded different audio")
    if "polarity_flips=1 reversed=1" not in result.stdout:
        raise RuntimeError("inverted polarity was not reported")
    print("Inverted polarity: same voice, same PCM, one flip reported")
    # The same recording as IQ through the production front end: dsp_pipeline
    # in Phase II mode, the FLL, p25_p2_slice and the decoder, across carrier
    # offset, noise and echo (bench/tests/test_p25_phase2_iq.c).
    iq_test = repo / "bench/build" / ("test_p25_phase2_iq.exe" if sys.platform == "win32" else "test_p25_phase2_iq")
    result = subprocess.run([str(iq_test)], text=True, capture_output=True,
                            env={**os.environ, "LS_P25P2_CAPTURE": str(capture)})
    print(result.stdout.strip().splitlines()[-1] if result.stdout.strip() else result.stderr)
    if result.returncode != 0 or "0 failed" not in result.stdout:
        print(result.stdout)
        raise RuntimeError("Phase II from IQ fell short of its floors")
    print("Phase II from IQ: voice recovered through the front end, +-1600 Hz")


if __name__ == "__main__":
    main()
