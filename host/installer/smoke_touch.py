"""Headless touch regression through Studio's real API and cell feed.

python host/installer/smoke_touch.py --checkout
python host/installer/smoke_touch.py --exe build_app/dist/LakeShark.exe
Uses the compiler already installed by smoke_sim.py; never downloads anything.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import urllib.parse
import urllib.request

from smoke_sim import REPO, Screen, grid
from smoke_test import own_process_tree, stop


PROBE = r'''
#include "lakeshark_app.h"
#include <stdio.h>
static int taps, col, row, swipe, keys;
static void draw(void) {
    char line[100]; lap_touch_t t;
    lap_clear(LAP_ATTR(LAP_WHITE, LAP_BLACK));
    lap_put_str(1, 1, "TOUCH PROBE: pixel finger and completed gestures", 7);
    snprintf(line, sizeof(line), "TAPS=%d CELL=%d,%d SWIPE=%d KEYS=%d", taps,col,row,swipe,keys);
    lap_put_str(1, 3, line, 7);
    if (lap_touch(&t)) {
        snprintf(line, sizeof(line), "DOWN=%d XY=%d,%d FROM=%d,%d", t.down,t.x,t.y,t.x0,t.y0);
        lap_put_str(1, 5, line, 7);
    }
    int s=lap_swipe(); if(s) swipe=s;
}
static bool touch(int c,int r) { taps++;col=c;row=r;return true; }
static bool key(lap_key_t k,char c) { if(k==LAP_KEY_CHAR && c=='h') keys++;return true; }
LAP_APP(.id="touch-probe",.name="Touch probe",.sub="Headless touch regression",
        .version="0.1",.author="Smoke test",.draw=draw,.touch=touch,.key=key);
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--checkout", action="store_true")
    src.add_argument("--exe", type=Path)
    ap.add_argument("--port", type=int, default=8813)
    ap.add_argument("--evidence", type=Path)
    ap.add_argument("--local-appdata", type=Path, default=REPO / "build_installer/sim-smoke-verified/local")
    args = ap.parse_args()
    mode = "checkout" if args.checkout else "frozen"
    out = (args.evidence or REPO / ("build_installer/touch-" + mode)).resolve()
    out.mkdir(parents=True, exist_ok=True)
    profile = out / "profile"
    profile.mkdir(exist_ok=True)
    env = dict(os.environ, USERPROFILE=str(profile), LOCALAPPDATA=str(args.local_appdata.resolve()),
               LSDK_STUDIO_SETTINGS=str(out / "settings.json"))
    for k in ("PYTHONPATH", "PYTHONHOME", "LAPSIM_DLL", "LAPSIM_SCRIPT", "LAPSIM_RADIO"):
        env.pop(k, None)
    if args.checkout:
        cmd = [sys.executable, str(REPO / "app-sdk/lsdk.py"), "studio", "--no-browser",
               "--workspace", str(out / "apps"), "--port", str(args.port)]
    else:
        env["PATH"] = str(Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32")
        cmd = [str(args.exe.resolve()), "--studio-only", "--port", str(args.port)]
    base = "http://127.0.0.1:%d" % args.port

    def api(name, body=None):
        req = urllib.request.Request(base + "/api/" + name,
                data=body if isinstance(body, bytes) else json.dumps(body).encode() if body is not None else None,
                headers={"X-Studio": "1", "Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=180) as r:
            return json.load(r)

    def touch(phase, x, y):
        assert api("sim/touch", {"phase": phase, "x": x, "y": y})["ok"]

    def wait_frame(screen, predicate, label, seconds=25):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            raw = screen.frame()
            info, text = grid(raw)
            if predicate(text):
                (out / (label + ".cells")).write_bytes(raw)
                (out / (label + ".txt")).write_text(text, encoding="utf-8")
                return raw, info, text
        raise AssertionError((label, text))

    results = []
    with (out / "studio.log").open("w", encoding="utf-8") as log:
        proc = subprocess.Popen(cmd, cwd=REPO, env=env, stdout=log, stderr=log,
                                creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        own_process_tree(proc)
        try:
            deadline = time.monotonic() + 60
            while True:
                try:
                    state = api("state")
                    break
                except OSError:
                    assert time.monotonic() < deadline and proc.poll() is None, "Studio did not start"
                    time.sleep(.2)
            assert state["frozen"] == bool(args.exe), state
            with urllib.request.urlopen(base + "/screen/screen.html") as r:
                assert b'/static/sim-touch.js' in r.read()
            with urllib.request.urlopen(base + "/static/sim-touch.js") as r:
                assert b'term touch' in r.read()

            for template in ("blank", "dots", "probe"):
                proj = api("new", {"name": "touch-" + template + "-" + str(time.time_ns()),
                                   "template": "blank" if template == "probe" else template})
                query = "?p=" + urllib.parse.quote(proj["path"])
                if template == "probe":
                    api("file" + query + "&path=main.c", PROBE.encode())
                for landscape in (False, True):
                    label = template + ("-landscape" if landscape else "-portrait")
                    run = api("run" + query, {"landscape": landscape})
                    assert run["ok"] and run["started"], (label, run, api("logs"))
                    screen = Screen(args.port)
                    try:
                        before, info, text = wait_frame(screen, lambda t: True, label + "-before")
                        cols, rows = info["cols"], info["rows"]
                        # G includes the actual font geometry; backing bitmap size is irrelevant.
                        cw, ch = before[6], before[7]
                        if template == "blank":
                            lines = text.splitlines()
                            y = next(y for y in range(rows-1, rows//2, -1) if "HELLO" in lines[y])
                            x = lines[y].index("HELLO") + 1
                            touch("down", x*cw+cw//2, y*ch+ch//2)
                            touch("up", x*cw+cw//2, y*ch+ch//2)
                            after, _, _ = wait_frame(screen, lambda t: "last one" in t, label + "-tap")
                            assert after != before
                        elif template == "dots":
                            rest, _, _ = wait_frame(screen, lambda t: "at rest" in t, label + "-rest", 35)
                            x, y = cols*cw//4, rows*ch//2
                            touch("down", x, y)
                            for dx in (40, 90, 140):
                                touch("move", x+dx, y)
                                time.sleep(.06)
                            touch("up", x+160, y)
                            after, _, _ = wait_frame(screen, lambda t: "rolling" in t, label + "-swipe")
                            assert after != rest
                        else:
                            # Locate the app's own row 1, preserving chrome offsets.
                            app_y = next(y for y, line in enumerate(text.splitlines()) if "TOUCH PROBE:" in line) - 1
                            app_x = text.splitlines()[app_y+1].index("TOUCH PROBE:") - 1
                            x, y = (app_x+5)*cw+3, (app_y+8)*ch+4
                            fx, fy = 5*cw+3, 8*ch+4
                            touch("down", x, y)
                            wait_frame(screen, lambda t: f"DOWN=1 XY={fx},{fy} FROM={fx},{fy}" in t, label + "-down")
                            touch("move", x+7, y+9)  # move within a cell too
                            wait_frame(screen, lambda t: f"DOWN=1 XY={fx+7},{fy+9}" in t, label + "-move")
                            touch("move", x+120, y+40)
                            wait_frame(screen, lambda t: f"DOWN=1 XY={fx+120},{fy+40}" in t, label + "-drag")
                            touch("up", x+150, y+40)
                            wait_frame(screen, lambda t: "TAPS=0 CELL=0,0 SWIPE=4" in t and "DOWN=0" in t, label + "-swipe")
                            touch("down", x, y)
                            touch("up", x, y)
                            wait_frame(screen, lambda t: "TAPS=1 CELL=5,8" in t, label + "-tap")
                            touch("down", x, y)
                            touch("move", x, y+100)
                            touch("cancel", x, y+100)
                            wait_frame(screen, lambda t: "DOWN=0" in t and "TAPS=1 CELL=5,8 SWIPE=4" in t, label + "-cancel")
                            # Exercise the ordered WebSocket channel used by the pointer UI too.
                            screen.send(f"term touch down {x} {y}")
                            screen.send(f"term touch up {x} {y}")
                            screen.send("term in 68")  # keyboard remains usable
                            wait_frame(screen, lambda t: "TAPS=2 CELL=5,8 SWIPE=4 KEYS=1" in t, label + "-socket-key")
                            touch("down", x, y)
                            wait_frame(screen, lambda t: "DOWN=1" in t, label + "-before-reconnect")
                            replacement = Screen(args.port)
                            screen.close()
                            screen = replacement
                            wait_frame(screen, lambda t: "DOWN=0" in t and "TAPS=2 CELL=5,8 SWIPE=4 KEYS=1" in t,
                                       label + "-reconnect")
                        results.append({"case": label, "cols": cols, "rows": rows,
                                        "cell": [cw, ch], "before_sha256": hashlib.sha256(before).hexdigest(),
                                        "build_seconds": run["seconds"]})
                        print("PASS", mode, label, flush=True)
                    finally:
                        screen.close()
                        api("stop", {})
            (out / "results.json").write_text(json.dumps(results, indent=2), encoding="utf-8")
            print("PASS touch; evidence:", out, flush=True)
        finally:
            stop(proc)


if __name__ == "__main__":
    main()
