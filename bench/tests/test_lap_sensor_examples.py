"""Headless app journeys through sensor acquisition, loss and saved navigation."""
import json
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import zlib

exe = Path(sys.argv[1]).resolve()
root = exe.parent / "sensor-example-journeys"
root.mkdir(exist_ok=True)
scenarios = [
    ("gamepad_tester", "imu,desktop", "1:device=100,4,1;2:gamepad=100,-32767,32767,1,2,32767,0,3,-1,1;3:contact=200,17,0,10,20;4:contact=200,42,0,30,40;5:contact=200,17,1,15,25;8:focus=0;10:focus=1;12:device=100,4,0;14:motionquality=6,-1;18:caps=desktop;25:stop"),
    ("gps_sky", "gps,sd,desktop", "1:gpsfix=42.3,-71.2,18;1:satellite=3,1,45,180,38,1;2:satellite=7,3,30,90,20,0;5:gpsantenna=2;8:gpslost;90:stop"),
    ("track_viewer", "sd,desktop", "8:key=w;16:key=p;24:key=t;32:stop"),
    ("gamepad_tester", "none", "8:stop"),
    ("gps_sky", "none", "8:stop"),
    ("track_viewer", "none", "8:stop"),
]
results = []
for index, (app, hardware, script) in enumerate(scenarios):
    cwd = root / (str(index) + "_" + app)
    (cwd / "sdcard/lakeshark").mkdir(parents=True, exist_ok=True)
    places = cwd / "sdcard/maps/installed"
    places.mkdir(parents=True, exist_ok=True)
    points = b"".join(struct.pack("<IiihBB", i, 423000000 + i * 100, -712000000 + i * 50, 18, 8, 1) for i in range(32))
    (cwd / "sdcard/lakeshark/track.log").write_bytes(struct.pack("<6I", 0x474f4c53, 1, 16, 32, 32, 0) + points)
    (cwd / "sdcard/maps/marks.txt").write_text("M 42.3 -71.2 0 Camp\nT 1793600000 saved\n")
    bits = bytearray(8192)
    bits[16] = bits[8176] = 1
    header = b"CCPLACE2" + struct.pack("<II", 0, zlib.crc32(bits)) + b"Dateline\0".ljust(64, b"\0")
    (places / "0000002a.ccpi").write_bytes(header + bits)
    env = dict(os.environ, LAPSIM=app, LAPSIM_HW=hardware, LAPSIM_SCRIPT=script, LAPSIM_CONSENT="grant")
    for key in ("LAPSIM_DLL", "LSSIM_FRAMES", "LAPSIM_PROFILE"):
        env.pop(key, None)
    run = subprocess.run([str(exe), "lap", "-A", "40", "-f", "100"], cwd=cwd, env=env,
                         capture_output=True, text=True, timeout=90,
                         creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
    log = run.stdout + run.stderr
    (cwd / "run.log").write_text(log)
    assert run.returncode == 0 and not re.search(r"simulator contract:.*(?:EINVAL|permission denied|flush failed)", log), log
    results.append(dict(app=app, hardware=hardware, status="PASS"))
    print("PASS", app, hardware)
(root / "results.json").write_text(json.dumps(results, indent=2) + "\n")
