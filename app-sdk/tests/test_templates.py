"""Every lsdk template, out of the box: it builds for the board, runs in the
simulator, shows something in its first second, and says plainly when the
board lacks what it uses (a Nano has no SDR and no motion sensor).

    python app-sdk/tests/test_templates.py [OUTDIR]

Reads the simulator's own cell dump (lssim -d), so the checks are on the
words on the screen. Writes contact sheets (first second, a few seconds,
no radio) to OUTDIR when Pillow is here. Needs the RISC-V toolchain and the
bench's host tools, as lsdk does.
"""
import shutil
import sys
import tempfile
from pathlib import Path

SDK = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SDK))
import lsdk  # noqa: E402

# per template: what the screen says by frame 25 (one second) with the SIM
# radio, and with no radio at all (the Nano)
EXPECT = {
    "blank": (["HELLOS", "press HELLO"], ["HELLOS"]),
    "radio": (["SIM radio", "SIM  peak"], ["NO RADIO", "No radio here gives a spectrum", "RTL-SDR"]),
    "dots": (["rolling"], ["rolling"]),
    "3d": (["drag, or the arrow keys"], ["drag, or the arrow keys"]),
    "service": (["watching in the background", "nothing decoded yet"], ["watching in the background"]),
}
FAILS = []


def check(what, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + what + ("" if ok else "  " + str(detail)[-900:]), flush=True)
    if not ok:
        FAILS.append(what)


def screen(app, frames, no_radio=False, script=None):
    text = []
    out = lsdk.sim_frames(app, frames, script=script, no_radio=no_radio, text=text)
    return text[0], out


def sheet(paths, dest):
    try:
        from PIL import Image
    except ImportError:
        return
    ims = [Image.open(p) for p in paths]
    w = sum(i.width for i in ims)
    out = Image.new("RGB", (w, max(i.height for i in ims)))
    x = 0
    for i in ims:
        out.paste(i, (x, 0))
        x += i.width
    out.resize((w // 2, out.height // 2)).save(dest)
    print("wrote", dest)


def main():
    outdir = Path(sys.argv[1]) if len(sys.argv) > 1 else None
    work = Path(tempfile.mkdtemp(prefix="lsdk-templates-"))
    for t, (with_sim, without) in EXPECT.items():
        app = lsdk.new_app("tpl_" + t, t, work, "test")
        res = lsdk.build(app)
        check("%s builds for the board" % t, res.ok and not [p for p in res.problems if p["severity"] == "error"],
              res.problems or res.output)
        sb = lsdk.sim_build(app, say=lambda s: None)
        check("%s builds into the simulator" % t, sb.ok, sb.problems or sb.output[-1500:])
        if not sb.ok:
            continue
        txt, out = screen(app, 25)
        first = out / "last.png" if (out / "last.png").exists() else out / "last.bmp"
        keep = []
        if outdir:
            outdir.mkdir(parents=True, exist_ok=True)
            keep.append(shutil.copy(first, outdir / ("tpl-%s-1s%s" % (t, first.suffix))))
        for want in with_sim:
            check("%s says '%s' within a second" % (t, want), want in txt, txt[-2500:])
        txt, out = screen(app, 25, no_radio=True)
        for want in without:
            check("%s with no radio says '%s'" % (t, want), want in txt, txt[-2500:])
        if outdir:
            last = out / "last.png" if (out / "last.png").exists() else out / "last.bmp"
            keep.append(shutil.copy(last, outdir / ("tpl-%s-noradio%s" % (t, last.suffix))))
            sheet(keep, outdir / ("tpl-%s.png" % t))
    if "service" in EXPECT:                     # a decode reaches the service's tick
        app = work / "tpl_service"
        txt, _ = screen(app, 40, script="20:page=FIRE ON MAIN ST;22:page=THIS IS A TEST")
        check("service sees a page decode and matches TEST", "decodes seen  2" in txt and "matches       1" in txt, txt[-2500:])
    print("FAIL: %d" % len(FAILS) if FAILS else "PASS: all")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
