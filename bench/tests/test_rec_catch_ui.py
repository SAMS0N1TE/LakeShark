"""Run the production REC renderer, keys, touch, OPTIONS and track context."""
import pathlib
import subprocess
import sys

sim = pathlib.Path(sys.argv[1]).resolve()
out = sim.parent / "catch-ui"
out.mkdir(exist_ok=True)
route = pathlib.Path(__file__).resolve().parent.parent / "fixtures" / "route.gpx"

def render(name, *args):
    r = subprocess.run([str(sim), "rec", "-d", "-o", str(out / (name + ".bmp")), *args],
                       capture_output=True, text=True, check=True)
    (out / (name + ".txt")).write_text(r.stdout, encoding="utf-8")
    return r.stdout

def cell_of(text, word, last=True):
    """Column and row of `word` in a -d dump (the lowest one by default)."""
    hit = None
    for line in text.splitlines():
        head = line[:4].strip()
        if not head.isdigit():
            continue
        col = line.find(word, 4)
        if col >= 0:
            hit = (col - 4 + len(word) // 2, int(head))
            if not last:
                break
    assert hit, word
    return hit

for wide in (False, True):
    mode = ["-l"] if wide else []
    suffix = "-wide" if wide else ""
    text = render("idle" + suffix, *mode, "-k", "6")
    assert "STOPPED 433.920000" in text and "unavailable in simulator" in text
    assert "Waiting for real bursts" in text and "BACK HOME" not in text
    assert "RADIO" in text and "OPTIONS" in text
    opts = render("options" + suffix, *mode, "-k", "6o")
    assert "CATCH OPTIONS" in opts and "Band" in opts
    band = render("band" + suffix, *mode, "-k", "6o", "-x", "@right")
    assert "CATCH > BAND" in band and "Preset" in band and "Custom centre" in band
    back = render("back" + suffix, *mode, "-k", "6", "-x", "@esc")
    assert "Waiting for real bursts" not in back and "READ /" in back
    track = render("track" + suffix, *mode, "-g", str(route), "-k", "6")
    assert "STOPPED 433.920000" in track

home = render("home")
col, row = cell_of(home, "CATCH")
touch = render("touch", "-x", "%d:%d" % (col, row))
assert "Waiting for real bursts" in touch
col, row = cell_of(touch, "READ")
touch_back = render("touch-back", "-x", "%d:%d,%d:%d" % (cell_of(home, "CATCH") + (col, row)))
assert "Waiting for real bursts" not in touch_back
radio = render("radio", "-k", "6r")
assert "RTL-SDR" in radio
print("PASS CATCH tab portrait, landscape, track context, touch, tabs, RADIO and layered OPTIONS")
