"""Render quality states with the production screen and exercise its tap key."""
import os
from pathlib import Path
import subprocess
import sys

exe = str(Path(sys.argv[1]).resolve())
out = Path(os.environ.get('LSSIM_TEST_OUT',Path(__file__).resolve().parents[1] / "build-blocks/compass-quality"))
out.mkdir(parents=True,exist_ok=True)
for state in ("GOOD", "DISTURBED", "UNCALIBRATED"):
    env = dict(os.environ, LSSIM_COMPASS_QUALITY=state)
    env.pop("LSSIM_TURN", None)
    for landscape in (False, True):
        orientation = "landscape" if landscape else "portrait"
        args = [exe, "compass", "--tft", "-d"] + (["-l"] if landscape else [])
        def run(extra, name):
            return subprocess.check_output(args + extra + ["-o", str(out / name)], env=env, text=True)
        grid = run([], f"{state.lower()}-{orientation}.bmp")
        assert f"FIELD {state}" in grid, grid
        if state != "GOOD":
            assert "RECALIBRATE" in grid
            if state == "DISTURBED":
                assert "magnet or metal nearby," in grid and "or recalibrate (K)" in grid
            tap = "73:25" if landscape else "41:61"
            changed = run(["-A", "40", "-x", tap + ",~30"], f"tap-{state.lower()}-{orientation}.bmp")
            assert "CALIBRAT" in changed.upper() and "FIELD " + state not in changed, changed
        print(f"PASS {state} {orientation}")


