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
    lsweb.WEB = os.path.join(HERE, "web")
    lsweb.SDK = os.path.join(HERE, "app-sdk")
    sys.path.insert(0, lsweb.SDK)
    sys.path.insert(0, os.path.join(lsweb.SDK, "studio"))

# Our window, not a browser's: a covered window keeps drawing and acking, so
# the board never ends the feed for a page that went quiet behind Studio.
os.environ["WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS"] = " ".join(lsweb.KEEP_AWAKE)

TITLE = "LakeShark"
STUDIO_PORT = 8790
_studio_lock = threading.Lock()
_studio_server = None
_studio_windows = 0


def studio_server(port=STUDIO_PORT):
    """Bind synchronously so startup failures reach the caller, then serve in-process."""
    global _studio_server
    import server
    from pathlib import Path
    with _studio_lock:
        if _studio_server is None:
            workspace = Path(os.environ.get("USERPROFILE") or Path.home()) / "Documents" / "LakeShark Apps"
            _studio_server = server.serve(port, workspace)
            threading.Thread(target=_studio_server.serve_forever, daemon=True, name="Studio").start()
    return _studio_server


def studio_open(_url):
    """OPEN STUDIO from the desk: from source, the bridge's own /studio/; from the exe,
    Studio inside the frozen process (it reaches the board through this bridge)."""
    import webview
    global _studio_windows
    if not FROZEN:
        webview.create_window("LakeShark Studio", _url, width=1500, height=900, background_color="#000000")
        return
    studio_server()
    import server
    window = webview.create_window("LakeShark Studio", "http://127.0.0.1:%d/" % STUDIO_PORT,
                                   width=1500, height=900, background_color="#000000")
    with _studio_lock:
        _studio_windows += 1
        server.UPDATE_RELAUNCH = "studio"

    def closed():
        global _studio_windows
        with _studio_lock:
            _studio_windows -= 1
            server.UPDATE_RELAUNCH = "studio" if _studio_windows else None
    window.events.closed += closed


class DesktopHostApi:
    """Only the visible viewer exposes these two bounded host operations."""
    def __init__(self):
        self.window = None

    def open_url(self, url):
        import webbrowser
        try:
            return 1 if webbrowser.open(lsweb.desktop_url(url), new=2) else -5
        except (ValueError, OSError):
            return -5

    def save_file(self, name, encoded):
        import webview
        import tempfile
        try:
            data = lsweb.desktop_file(name, encoded)
            selected = self.window.create_file_dialog(webview.FileDialog.SAVE, save_filename=name)
            if not selected:
                return -8
            path = selected[0] if isinstance(selected, (list, tuple)) else selected
            temporary = None
            try:
                with tempfile.NamedTemporaryFile(dir=os.path.dirname(os.path.abspath(path)), delete=False) as f:
                    temporary = f.name
                    f.write(data)
                    f.flush()
                    os.fsync(f.fileno())
                os.replace(temporary, path)
                temporary = None
            finally:
                if temporary is not None:
                    os.unlink(temporary)
            return 1
        except (ValueError, OSError):
            return -5


def main():
    if "--get-compiler" in sys.argv or "--studio-only" in sys.argv:
        import argparse
        ap = argparse.ArgumentParser()
        ap.add_argument("--get-compiler", action="store_true")
        ap.add_argument("--studio-only", action="store_true")
        ap.add_argument("--port", type=int, default=STUDIO_PORT)
        a = ap.parse_args()
        if not FROZEN:
            sys.path.insert(0, os.path.join(HERE, "..", "..", "app-sdk"))
            sys.path.insert(0, os.path.join(HERE, "..", "..", "app-sdk", "studio"))
        if a.get_compiler:
            import toolchain
            return toolchain.main()
        srv = studio_server(a.port)
        try:
            # Keep the daemon server alive without importing or starting a GUI.
            threading.Event().wait()
        except KeyboardInterrupt:
            srv.shutdown()
        return 0
    import webview
    studio_alone = "--studio" in sys.argv         # the LakeShark Studio shortcut
    cfg = lsdesk.load()
    host = lsdesk.desktop_host(cfg)
    if host:                                      # a board on the network serves its own page
        url, owner = lsdesk.board_url(host), False
        if FROZEN and not studio_alone:
            # Keep update controls local even when the board supplies the screen.
            studio_server()
            url = "http://127.0.0.1:%d/static/desktop.html?board=%s" % (STUDIO_PORT, host)
    else:
        port = int(cfg.get("port", lsdesk.PORT))
        url = "http://127.0.0.1:%d/" % port
        owner = not lsdesk.running(port)
        if owner:                                 # Studio alone still needs it to reach the board
            server = lsweb.serve(lsdesk.finder(cfg), port, int(cfg.get("baud", 4000000)))
            threading.Thread(target=server.serve_forever, daemon=True).start()
            lsdesk.log("bridge on", url, "(app)")
    lsweb.STUDIO_WINDOW = studio_open
    if studio_alone:
        studio_open(url + "studio/")
    else:
        host_api = DesktopHostApi()
        host_api.window = webview.create_window(TITLE, url, width=1500, height=860, min_size=(640, 400), background_color="#000000", js_api=host_api)
    os.makedirs(os.path.join(lsdesk.HOME, "app"), exist_ok=True)
    # Its own profile: the screen's settings (font, fit, pixels) stay put between runs.
    webview.start(private_mode=False, storage_path=os.path.join(lsdesk.HOME, "app"))
    lsdesk.log("window closed (app)")
    if owner and lsweb.BOARD and lsweb.BOARD.up.is_set():
        lsweb.BOARD.stop()
    os._exit(0)                                   # the bridge's threads hold the port: leave at once


if __name__ == "__main__":
    sys.exit(main())
