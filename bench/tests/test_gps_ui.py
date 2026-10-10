"""Exercise the production GPS screen and router in the real simulator."""
import pathlib
import re
import subprocess
import sys

sim = pathlib.Path(sys.argv[1]).resolve()
out = sim.parent / "gps-ui"
out.mkdir(exist_ok=True)

def render(name, *args):
    result = subprocess.run([str(sim), "gps", "-d", "-o", str(out / (name + ".bmp")), *args], capture_output=True, text=True, check=True)
    (out / (name + ".txt")).write_text(result.stdout, encoding="utf-8")
    return result.stdout

portrait = render("portrait")
assert "[ OPTIONS ]" in portrait
assert "A ID 2  1/14" in portrait
assert "B ID 5  2/14" in render("direction", "-K", "down")
for raw in portrait.splitlines():
    match = re.match(r"\s*(\d+) (.*)", raw)
    if match and re.search(r"B\s+5\s+092", match[2]):
        row = match[1]
        break
else:
    raise AssertionError("Satellite B table row missing")
assert "B ID 5  2/14" in render("touch-row", "-x", "3:" + row)
for raw in portrait.splitlines():
    match = re.match(r"\s*(\d+) (.*)", raw)
    if match and "[ PREV ] [ NEXT ]" in match[2]:
        row = match[1]
        break
assert "B ID 5  2/14" in render("touch-next", "-x", "12:" + row)
landscape = render("landscape", "-l")
assert "SATELLITES" in landscape
for mode in ((), ("-l",)):
    details = render("details" + ("-wide" if mode else ""), *mode, "-k", "v")
    assert "ANTENNA" in details and "C/N0 best" in details and "FIX TIME" in details
options = render("options", "-k", "o")
assert "GPS OPTIONS" in options and "RTL-SDR" not in options
assert "Export GPX" in options and "Minimum movement" in options
assert "Stop recording / cancel resume" in options
confirm = render("confirm", "-k", "o", "-x", "@down,@down,@down,@right")
assert "GPS > CONFIRM CLEAR" in confirm
back = render("back", "-k", "o", "-x", "@down,@down,@down,@right,@left")
assert "GPS OPTIONS" in back and "YES, clear" not in back
print("PASS: GPS portrait, landscape, details, arrows, touch, options and nested navigation")
