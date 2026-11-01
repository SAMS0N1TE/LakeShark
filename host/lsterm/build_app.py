"""Build LakeShark.exe (lsapp.py) with PyInstaller: one windowed program with
the screen page inside. Output: <repo>/build_app/dist/LakeShark.exe.

  python host/lsterm/build_app.py

Needs pyinstaller, pywebview (with pythonnet and bottle) and pyserial in the
Python that runs it. The exe uses Windows' own WebView2 (Edge's), so nothing
else is installed on the PC it runs on."""
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
WEB = os.path.join(REPO, "components", "apps", "tui", "web")
SDK = os.path.join(REPO, "app-sdk")
OUT = os.path.join(REPO, "build_app")

# Only what the page loads at run time: not the generators next to it.
PAGE = ["screen.html", "screen.js", "xterm-headless.js", "lsfont.bin.gz", "dejavu-mono.woff2",
        "dejavu-mono-bold.woff2", "icon-180.png", "icon-192.png", "icon-512.png",
        "DEJAVU-LICENSE.txt", "XTERM-LICENSE.txt"]


EXCLUDE = ["PySide6", "PyQt5", "PyQt6", "qtpy", "webview.platforms.qt", "webview.platforms.gtk",
           "webview.platforms.cocoa", "webview.platforms.android", "webview.platforms.cef", "webview.platforms.mshtml",
           "numpy", "scipy", "pandas", "matplotlib", "PIL", "tkinter", "IPython", "jedi", "zmq", "lxml",
           "cryptography", "sqlite3", "pytest"]


def main():
    os.makedirs(OUT, exist_ok=True)
    stage = os.path.join(OUT, "web")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    for name in PAGE:
        shutil.copy2(os.path.join(WEB, name), stage)
    # Studio runs from this checkout with this Python (lsapp.studio_open).
    # --release: no paths from this PC; the exe finds Studio beside itself.
    with open(os.path.join(stage, "build.json"), "w", encoding="utf-8") as f:
        json.dump({} if "--release" in sys.argv else {"repo": REPO, "python": sys.executable}, f)
    cmd = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--onefile", "--windowed",
           "--name", "LakeShark", "--icon", os.path.join(WEB, "icon-192.png"),
           "--distpath", os.path.join(OUT, "dist"), "--workpath", os.path.join(OUT, "work"),
           "--specpath", OUT, "--paths", HERE, "--paths", SDK,
           "--hidden-import", "lsweb", "--hidden-import", "lsdesk", "--hidden-import", "lapboard",
           "--hidden-import", "serial.tools.list_ports",
           # Windows' WebView2 only: pywebview's Qt/GTK backends would drag in PySide6 and more.
           "--hidden-import", "webview.platforms.winforms", "--hidden-import", "webview.platforms.edgechromium",
           *sum((["--exclude-module", m] for m in EXCLUDE), []),
           "--add-data", stage + os.pathsep + "web",
           os.path.join(HERE, "lsapp.py")]
    subprocess.run(cmd, check=True)
    exe = os.path.join(OUT, "dist", "LakeShark.exe")
    print("built", exe, "%.1f MB" % (os.path.getsize(exe) / 1e6))


if __name__ == "__main__":
    main()
