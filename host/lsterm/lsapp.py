"""LakeShark Desktop as one program: the USB bridge (lsweb) and its screen in a
window of its own (WebView2 through pywebview), instead of a browser tab next
to a Python process. Studio opens as a second window of the same program.

  python lsapp.py            from the source tree
  LakeShark.exe              built with: python build_app.py

Closing the window stops the board's screen feed and quits. A second start
while one runs just opens another window on the running bridge."""
import os
import sys
import threading

FROZEN = getattr(sys, "frozen", False)
HERE = getattr(sys, "_MEIPASS", os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)

import lsdesk  # noqa: E402
import lsweb  # noqa: E402

def _built():
    """Where the exe was built from (build_app.py writes it): the repo and its Python."""
    try:
        import json
        with open(os.path.join(HERE, "web", "build.json"), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


if FROZEN:
    # The page ships inside the program; Studio needs the SDK, its compilers and a
    # real Python, so it runs as its own process from the SDK folder (studio_url).
    lsweb.WEB = os.path.join(HERE, "web")
    _cfg, _b = lsdesk.load(), _built()
    lsweb.SDK = _cfg.get("sdk") or (os.path.join(_b["repo"], "app-sdk") if _b.get("repo") else "")
    STUDIO_PYTHON = _cfg.get("python") or _b.get("python")
    # A downloaded exe: put it in a LakeShark checkout and Studio is found there,
    # with the Python on PATH.
    _beside = os.path.join(os.path.dirname(sys.executable), "app-sdk")
    if not os.path.isfile(os.path.join(lsweb.SDK or "", "lsdk.py")) and os.path.isfile(os.path.join(_beside, "lsdk.py")):
        lsweb.SDK = _beside
    if not STUDIO_PYTHON:
        import shutil
        STUDIO_PYTHON = shutil.which("python") or shutil.which("py")

# Our window, not a browser's: a covered window keeps drawing and acking, so
# the board never ends the feed for a page that went quiet behind Studio.
os.environ["WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS"] = " ".join(lsweb.KEEP_AWAKE)

import webview  # noqa: E402

TITLE = "LakeShark"
STUDIO_PORT = 8790
NO_STUDIO = ("<body style='background:#000;color:#ccc;font:16px monospace;padding:2em'>"
             "<h3 style='color:#5ff'>LakeShark Studio is not set up on this PC</h3>"
             "<p>Studio builds apps from the LakeShark source. Two steps:</p>"
             "<p>1. Install Python 3.9 or newer, with \"Add to PATH\" ticked.<br>"
             "2. Clone github.com/SAMS0N1TE/LakeShark and put LakeShark.exe in that folder.</p>"
             "<p>Or point <b>\"sdk\"</b> (the app-sdk folder) and <b>\"python\"</b> at them in "
             ".lsdesk.json in your user folder.</p></body>")


def studio_open(_url):
    """OPEN STUDIO from the desk: from source, the bridge's own /studio/; from the exe,
    Studio as its own Python process (it reaches the board through this bridge)."""
    if not FROZEN:
        webview.create_window("LakeShark Studio", _url, width=1500, height=900, background_color="#000000")
        return
    import subprocess
    import time
    lsdk = os.path.join(lsweb.SDK or "", "lsdk.py")
    if not (STUDIO_PYTHON and os.path.isfile(STUDIO_PYTHON) and os.path.isfile(lsdk)):
        webview.create_window("LakeShark Studio", html=NO_STUDIO, width=900, height=400, background_color="#000000")
        return
    if not lsdesk.running(STUDIO_PORT):
        subprocess.Popen([STUDIO_PYTHON, lsdk, "studio", "--port", str(STUDIO_PORT), "--no-browser"],
                         cwd=lsweb.SDK, creationflags=getattr(subprocess, "CREATE_NO_WINDOW", 0))
        for _ in range(60):                       # its first start imports the SDK: a few seconds
            if lsdesk.running(STUDIO_PORT):
                break
            time.sleep(0.25)
    webview.create_window("LakeShark Studio", "http://127.0.0.1:%d/" % STUDIO_PORT,
                          width=1500, height=900, background_color="#000000")


def main():
    cfg = lsdesk.load()
    host = lsdesk.desktop_host(cfg)
    if host:                                      # a board on the network serves its own page
        url, owner = lsdesk.board_url(host), False
    else:
        port = int(cfg.get("port", lsdesk.PORT))
        url = "http://127.0.0.1:%d/" % port
        owner = not lsdesk.running(port)
        if owner:
            server = lsweb.serve(lsdesk.finder(cfg), port, int(cfg.get("baud", 4000000)))
            threading.Thread(target=server.serve_forever, daemon=True).start()
            lsdesk.log("bridge on", url, "(app)")
    lsweb.STUDIO_WINDOW = studio_open
    webview.create_window(TITLE, url, width=1500, height=860, min_size=(640, 400), background_color="#000000")
    os.makedirs(os.path.join(lsdesk.HOME, "app"), exist_ok=True)
    # Its own profile: the screen's settings (font, fit, pixels) stay put between runs.
    webview.start(private_mode=False, storage_path=os.path.join(lsdesk.HOME, "app"))
    lsdesk.log("window closed (app)")
    if owner and lsweb.BOARD and lsweb.BOARD.up.is_set():
        lsweb.BOARD.stop()
    os._exit(0)                                   # the bridge's threads hold the port: leave at once


if __name__ == "__main__":
    main()
