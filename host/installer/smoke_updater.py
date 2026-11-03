"""Probe the frozen updater and its assets with --studio-only; never open a GUI."""
import argparse
import json
import os
from pathlib import Path
import re
import socket
import subprocess
import tempfile
import time
from urllib.error import HTTPError, URLError
from urllib.request import Request, urlopen
from smoke_test import own_process_tree, stop

REPO = Path(__file__).resolve().parents[2]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--exe", type=Path, default=REPO / "build_app/dist/LakeShark.exe")
    a = ap.parse_args()
    root = Path(tempfile.mkdtemp(prefix="updater-frozen-", dir=REPO / "build_installer"))
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        port = sock.getsockname()[1]
    env = dict(os.environ, USERPROFILE=str(root), LOCALAPPDATA=str(root / "local"),
               LSDK_STUDIO_SETTINGS=str(root / "studio.json"),
               LSDK_UPDATER_TEST_URL="http://127.0.0.1:1/latest.json")
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    log = (root / "process.log").open("w", encoding="utf-8")
    proc = subprocess.Popen([str(a.exe.resolve()), "--studio-only", "--port", str(port)],
                            env=env, creationflags=flags, stdout=log, stderr=subprocess.STDOUT)
    own_process_tree(proc)
    base = "http://127.0.0.1:%d/" % port
    def get(route):
        with urlopen(base + route, timeout=2) as response:
            return response.read()
    def post(action, data):
        with urlopen(Request(base + "api/updater/" + action, json.dumps(data).encode(),
                             headers={"X-Studio": "1", "Content-Type": "application/json"}), timeout=7) as response:
            return json.load(response)
    try:
        end = time.monotonic() + 30
        while True:
            try:
                status = json.loads(get("api/updater/status"))
                break
            except (URLError, OSError):
                if proc.poll() is not None or time.monotonic() > end:
                    raise RuntimeError("frozen server failed to start, exit=%r" % proc.poll())
                time.sleep(0.2)
        assert status["frozen"] is True
        expected = re.search(r'^#define AppVersion "([^"]+)"',
                             (REPO / "host/installer/lakeshark.iss").read_text("utf-8"), re.M)[1]
        assert status["version"] == expected, status
        assert get("").count(b'<script src="static/updater.js">') == 1
        assert b"Check for updates" in get("static/updater.js")
        assert b"LS_DESKTOP_UPDATER" in get("screen/updater.js")
        assert b'data-updater-desktop="true"' in get("static/desktop.html")
        assert post("settings", {"automatic": False})["automatic"] is False
        assert json.loads((root / "updater.json").read_text("utf-8"))["automatic"] is False
        assert "checked" not in post("check", {"manual": False})
        try:
            post("install", {"relaunch": "studio"})
            raise AssertionError("install without an available update was accepted")
        except HTTPError as e:
            assert e.code == 400
            assert b"newer version" in e.read()
            e.close()
        (root / "results.json").write_text(json.dumps({"passed": True, "status": status,
            "checks": ["frozen baked version", "Studio updater asset", "Desktop updater asset",
                       "network Desktop wrapper", "settings saved", "automatic off", "no-update install refused"]}, indent=2), "utf-8")
        print("PASS: frozen updater smoke; evidence", root)
    finally:
        stop(proc)
        log.close()


if __name__ == "__main__":
    main()
