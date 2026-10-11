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

for wide in (False, True):
    mode = ["-l"] if wide else []
    suffix = "-wide" if wide else ""
    text = render("idle" + suffix, *mode, "-K", "tab,tab")
    assert "STOPPED 433.920000" in text and "unavailable in simulator" in text
    assert "Waiting for real bursts" in text and "BACK" in text
    opts = render("options" + suffix, *mode, "-K", "tab,tab", "-k", "o")
    assert "CATCH OPTIONS" in opts and "Band" in opts
    band = render("band" + suffix, *mode, "-K", "tab,tab", "-k", "o", "-x", "@right")
    assert "CATCH > BAND" in band and "Preset" in band and "Custom centre" in band
    back = render("back" + suffix, *mode, "-K", "tab,tab", "-k", "b")
    assert "Waiting for real bursts" not in back
    track = render("track" + suffix, *mode, "-g", str(route), "-K", "tab,tab")
    assert "STOPPED 433.920000" in track

touch = render("touch", "-x", "28:8")
assert "Waiting for real bursts" in touch
touch_back = render("touch-back", "-x", "28:8,45:13")
assert "Waiting for real bursts" not in touch_back
radio = render("radio", "-K", "tab,tab", "-k", "r")
assert "RTL-SDR" in radio
print("PASS CATCH portrait, landscape, track context, touch, BACK, RADIO and layered OPTIONS")
