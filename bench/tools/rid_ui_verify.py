"""Render and check the real lssim DRONES UI, with no hardware or audio."""
import os
from pathlib import Path
import re
import subprocess
from PIL import Image

root = Path(__file__).resolve().parents[2]
exe = root / "bench/build/lssim.exe"
out = root / "bench/out/rid"
out.mkdir(parents=True, exist_ok=True)

def render(name, args=(), fixture=True, nofix=False):
    env = os.environ.copy()
    env.pop("LSSIM_RID", None)
    env.pop("LSSIM_RID_NOFIX", None)
    if fixture:
        env["LSSIM_RID"] = "1"
    if nofix:
        env["LSSIM_RID_NOFIX"] = "1"
    target = out / (name + ".bmp")
    text = subprocess.check_output([str(exe), "drones", "-d", "-o", str(target), *args],
                                   cwd=root, env=env, text=True)
    (out / (name + ".txt")).write_text(text)
    pic = Image.open(target)
    if "-l" in args:
        pic = pic.rotate(-90, expand=True)
    pic.save(out / (name + ".png"))
    return text

portrait = render("portrait", ["-f", "8", "-A", "200"])
assert "5 active" in portrait and "56 m" in portrait and "LS-DEMO-001" in portrait
render("landscape", ["-l", "-f", "8", "-A", "200"])
detail = render("detail", ["-k", "d"])
assert "ABOVE TAKEOFF" in detail and "Operator GPS" in detail and "UNVERIFIED" in detail
assert "Auth ABSENT" in detail and "AIRBORNE" in detail
render("detail-landscape", ["-l", "-k", "d"])
row = re.search(r"^\s*(\d+) .*?> LS-DEMO-001", portrait, re.M)
assert row
tapped = render("touch-detail", ["-x", f"10:{row[1]}"])
assert "DRONE DETAIL" in tapped
back = render("touch-back", ["-x", f"10:{row[1]},+b"])
assert "HEARD DRONES" in back
options = render("options", ["-x", "+o"])
assert "DISPLAY" in options and "ALERTS" in options and "BACK" in options
display = render("options-display", ["-x", "+o,@enter"])
assert "SORT" in display and "UNITS" in display and "BACK" in display
imperial = render("imperial", ["-x", "+o,@enter,@down,@enter,@esc,@esc"])
assert " ft" in imperial and "HEARD DRONES" in imperial
rssi = render("sort-rssi", ["-x", "+o,@enter,@enter,@esc,@esc"])
assert "[RSSI]" in rssi
age = render("sort-age", ["-x", "+o,@enter,@enter,@enter,@esc,@esc"])
assert "[AGE]" in age
alerts = render("options-alerts", ["-x", "+o,@down,@enter"])
assert "NEW DRONE ALERT" in alerts and "OFF" in alerts
cleared = render("cleared", ["-x", "+o,@down,@down,@enter,@esc"])
assert "0 active" in cleared
empty = render("empty", fixture=False)
assert "Listening for Remote ID" in empty and "0 active" in empty
nofix = render("no-gps", nofix=True)
assert "-- / -42 dBm" in nofix
expired = render("expired", ["-f", "65", "-A", "1000"])
assert "0 active" in expired
render("sweep-start", ["-f", "1", "-A", "200"])
render("sweep-later", ["-f", "8", "-A", "200"])
assert (out / "sweep-start.bmp").read_bytes() != (out / "sweep-later.bmp").read_bytes()
render("tracks", ["-g", "bench/fixtures/route.gpx"])
print("PASS: portrait, landscape, details, touch/back, nested options, units,")
print("RSSI/age sort, alert default, clear, empty, no GPS, expiry, sweep, GPX tracks.")
