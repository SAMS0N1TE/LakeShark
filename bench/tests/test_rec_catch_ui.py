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

# Channel selection: WATCH key, slice strip, CHANNELS list, presets, touch.
ADD_ONLY = "@down,@down,@right,@enter,@enter"          # OPTIONS > Channels > Add preset > 433.92
ADD_433 = ADD_ONLY + ",@esc,@esc"
for wide in (False, True):
    mode = ["-l"] if wide else []
    sfx = "-wide" if wide else ""
    idle = render("watch-all" + sfx, *mode, "-k", "6")
    assert "WATCH ALL" in idle and "432.72" in idle and "435.12" in idle and "H watch" in idle
    opts = render("watch-options" + sfx, *mode, "-k", "6o")
    assert "Watch" in opts and "Channels" in opts and "Only 433.92" in opts and "0 of 8" in opts
    chan = render("watch-channels" + sfx, *mode, "-k", "6o", "-x", "@down,@down,@right")
    assert "CATCH > CHANNELS" in chan and "Add preset" in chan and "Add frequency" in chan
    assert "Edit channel" in chan and "No channels yet" in chan
    picker = render("watch-preset" + sfx, *mode, "-k", "6o", "-x", "@down,@down,@right,@enter")
    # Only presets inside the 433.92 slice are offered.
    assert "ADD PRESET" in picker and "433.92 MHz" in picker and "315.00" not in picker and "915.00" not in picker
    added = render("watch-added" + sfx, *mode, "-k", "6o", "-x", "@down,@down,@right,@enter,@enter")
    assert "Edit channel" in added and "No channels yet" not in added
    on = render("watch-chosen" + sfx, *mode, "-k", "6o", "-x", ADD_433)
    assert "WATCH CHOSEN 1  ignored 0" in on and "[" in on and "]" in on and "CHOSEN 1" in on
    assert "Waiting for bursts in chosen channels" in on
    only = render("watch-only" + sfx, *mode, "-k", "6o", "-x", "@down,@down,@down,@enter,@esc")
    assert "WATCH CHOSEN 1" in only
    key = render("watch-key" + sfx, *mode, "-k", "6h")
    assert "CHOSEN is empty: watching ALL" in key        # no channel yet: visible note, falls back to ALL
    key2 = render("watch-key2" + sfx, *mode, "-k", "6hh")
    assert "WATCH ALL" in key2
    pick = render("watch-edit-pick" + sfx, *mode, "-k", "6o", "-x", ADD_ONLY + ",@down,@down,@enter")
    assert "EDIT CHANNEL" in pick and "433.920 MHz" in pick and "+/-100 kHz" in pick
    keypad = render("watch-width-pad" + sfx, *mode, "-k", "6o", "-x", ADD_ONLY + ",@down,@down,@enter,@enter")
    assert "Width, 0 removes" in keypad and "200" in keypad
    narrow = render("watch-narrow" + sfx, *mode, "-k", "6o", "-x", ADD_ONLY + ",@down,@down,@enter,@enter,+50,@enter,@enter")
    assert "EDIT CHANNEL" in narrow and "+/-25 kHz" in narrow
    gone = render("watch-remove" + sfx, *mode, "-k", "6o", "-x", ADD_ONLY + ",@down,@down,@enter,@enter,+0,@enter")
    assert "CATCH > CHANNELS" in gone and "No channels yet" in gone

home = render("watch-home")
col, row = cell_of(home, "CATCH")
page = render("watch-page", "-x", "%d:%d" % (col, row))
wcol, wrow = cell_of(page, "WATCH", last=False)               # the bar key
tapped = render("watch-tap", "-x", "%d:%d,%d:%d" % (col, row, wcol, wrow))
assert "CHOSEN is empty: watching ALL" in tapped
scol, srow = cell_of(page, "WATCH ALL")                        # the slice strip: opens CHANNELS
strip = render("watch-strip", "-x", "%d:%d,%d:%d" % (col, row, scol, srow + 2))
assert "CHANNELS OPTIONS" in strip and "Add preset" in strip
print("PASS CATCH tab portrait, landscape, track context, touch, tabs, RADIO, layered OPTIONS and channel selection")
