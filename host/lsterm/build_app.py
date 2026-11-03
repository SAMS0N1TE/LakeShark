"""Build LakeShark.exe (lsapp.py) with PyInstaller: one windowed program with
the screen page inside. Output: <repo>/build_app/dist/LakeShark.exe.

  python host/lsterm/build_app.py

Needs pyinstaller, pywebview (with pythonnet and bottle) and pyserial in the
Python that runs it. The exe uses Windows' own WebView2 (Edge's), so nothing
else is installed on the PC it runs on."""
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.normpath(os.path.join(HERE, "..", ".."))
WEB = os.path.join(REPO, "components", "apps", "tui", "web")
SDK = os.path.join(REPO, "app-sdk")
OUT = os.path.join(REPO, "build_app")
MINGW = os.environ.get("LSDK_MINGW", r"C:\mingw64\bin")

# Only what the page loads at run time: not the generators next to it.
PAGE = ["screen.html", "screen.js", "updater.js", "xterm-headless.js", "lsfont.bin.gz", "dejavu-mono.woff2",
        "dejavu-mono-bold.woff2", "icon-180.png", "icon-192.png", "icon-512.png",
        "DEJAVU-LICENSE.txt", "XTERM-LICENSE.txt"]


EXCLUDE = ["PySide6", "PyQt5", "PyQt6", "qtpy", "webview.platforms.qt", "webview.platforms.gtk",
           "webview.platforms.cocoa", "webview.platforms.android", "webview.platforms.cef", "webview.platforms.mshtml",
           "numpy", "scipy", "pandas", "matplotlib", "PIL", "tkinter", "IPython", "jedi", "zmq", "lxml",
           "cryptography", "sqlite3", "pytest"]


def stage_sdk():
    """Allowlist runtime SDK assets; never ship apps, tests or Python caches."""
    dest = os.path.join(OUT, "app-sdk")
    shutil.rmtree(dest, ignore_errors=True)
    os.makedirs(dest)
    for name in ("lsdk.py", "lap.py", "lapboard.py", "lsstore.py", "apidoc.py", "api_registry.py", "toolchain.py",
                 "host_toolchain.py", "frozen_sim.py", "exports.txt", "README.md"):
        shutil.copy2(os.path.join(SDK, name), dest)
    for name in ("include", "templates", "studio", "lib", "docs"):
        shutil.copytree(os.path.join(SDK, name), os.path.join(dest, name),
                        ignore=shutil.ignore_patterns("__pycache__", "*.pyc", "tests", "test_*"))
    os.makedirs(os.path.join(dest, "tools"), exist_ok=True)
    shutil.copy2(os.path.join(SDK, "tools", "sim_alloc.h"), os.path.join(dest, "tools"))
    # Studio's AI helper implements its contract directly; it does not import mcp/.
    return dest


def stage_sim(sdk):
    """Follow PE imports recursively, taking runtime DLLs from MinGW, not PATH."""
    bdir = os.path.join(OUT, "sim-host")
    env = dict(os.environ, PATH=MINGW + os.pathsep + os.environ.get("PATH", ""))
    subprocess.run([os.path.join(MINGW, "cmake.exe"), "-G", "Ninja", "-S", os.path.join(REPO, "bench"),
                    "-B", bdir, "-DLS_SANITIZE=OFF", "-DLSSIM_DYNAMIC_APPS=ON", "-DLAPSIM_EXTRA_APPS=",
                    "-DCMAKE_C_COMPILER=" + os.path.join(MINGW, "gcc.exe"),
                    "-DCMAKE_CXX_COMPILER=" + os.path.join(MINGW, "g++.exe")], check=True, env=env)
    subprocess.run([os.path.join(MINGW, "cmake.exe"), "--build", bdir, "--target", "lssim", "-j", "8"],
                   check=True, env=env)
    dest = os.path.join(sdk, "sim")
    os.makedirs(dest)
    src = os.path.join(bdir, "lssim.exe")
    shutil.copy2(os.path.join(bdir, "liblssim.dll.a"), os.path.join(dest, "liblssim.a"))
    system = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32")
    pending, seen = [src], set()
    manifest = {}
    while pending:
        binary = pending.pop()
        name = os.path.basename(binary)
        if name.lower() in seen:
            continue
        seen.add(name.lower())
        shutil.copy2(binary, os.path.join(dest, name))
        r = subprocess.run([os.path.join(MINGW, "objdump.exe"), "-p", binary], check=True,
                           capture_output=True, text=True)
        imports = re.findall(r"DLL Name:\s*(\S+)", r.stdout)
        manifest[name] = imports
        for dep in imports:
            candidates = [os.path.join(os.path.dirname(src), dep), os.path.join(MINGW, dep)]
            found = next((p for p in candidates if os.path.isfile(p)), None)
            if found:
                pending.append(found)
            elif dep.lower().startswith(("api-ms-win-", "ext-ms-win-")) or os.path.isfile(os.path.join(system, dep)):
                continue
            else:
                raise SystemExit("Unresolved simulator DLL: " + dep)
    with open(os.path.join(dest, "dependencies.json"), "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2)


def app_icon():
    icon = os.path.join(OUT, "LakeShark.ico")
    try:
        from PIL import Image
        with Image.open(os.path.join(WEB, "icon-192.png")) as im:
            im.save(icon, format="ICO", sizes=[(16, 16), (32, 32), (48, 48), (64, 64), (128, 128)])
    except ImportError:
        # Windows Vista+ accepts PNG entries in ICO files; useful on minimal build hosts.
        import struct
        with open(os.path.join(WEB, "icon-192.png"), "rb") as f:
            png = f.read()
        with open(icon, "wb") as f:
            f.write(struct.pack("<HHHBBBBHHII", 0, 1, 1, 192, 192, 0, 0, 1, 32, len(png), 22))
            f.write(png)
    return icon


def main():
    os.makedirs(OUT, exist_ok=True)
    stage = os.path.join(OUT, "web")
    shutil.rmtree(stage, ignore_errors=True)
    os.makedirs(stage)
    for name in PAGE:
        shutil.copy2(os.path.join(WEB, name), stage)
    sdk = stage_sdk()
    stage_sim(sdk)
    version = re.search(r'^#define AppVersion "([^"]+)"',
                        open(os.path.join(REPO, "host", "installer", "lakeshark.iss"), encoding="utf-8").read(), re.M)[1]
    # --release contains no source checkout or Python interpreter paths.
    with open(os.path.join(stage, "build.json"), "w", encoding="utf-8") as f:
        json.dump({"version": version} | ({} if "--release" in sys.argv else {"repo": REPO, "python": sys.executable}), f)
    cmd = [sys.executable, "-m", "PyInstaller", "--noconfirm", "--clean", "--onefile", "--windowed",
           "--name", "LakeShark", "--icon", app_icon(),
           "--distpath", os.path.join(OUT, "dist"), "--workpath", os.path.join(OUT, "work"),
           "--specpath", OUT, "--paths", HERE, "--paths", SDK, "--paths", os.path.join(SDK, "studio"),
           "--hidden-import", "lsweb", "--hidden-import", "lsdesk", "--hidden-import", "lapboard",
           "--hidden-import", "serial.tools.list_ports",
           *sum((["--hidden-import", m] for m in ("lsdk", "lap", "lsstore", "apidoc", "api_registry", "toolchain",
                                                 "host_toolchain", "frozen_sim", "server", "ai")), []),
           # Windows' WebView2 only: pywebview's Qt/GTK backends would drag in PySide6 and more.
           "--hidden-import", "webview.platforms.winforms", "--hidden-import", "webview.platforms.edgechromium",
           *sum((["--exclude-module", m] for m in EXCLUDE), []),
           "--add-data", stage + os.pathsep + "web",
           "--add-data", sdk + os.pathsep + "app-sdk",
           os.path.join(HERE, "lsapp.py")]
    env = dict(os.environ, PYINSTALLER_CONFIG_DIR=os.path.join(OUT, "pyinstaller-cache"))
    subprocess.run(cmd, check=True, env=env)
    exe = os.path.join(OUT, "dist", "LakeShark.exe")
    print("built", exe, "%.1f MB" % (os.path.getsize(exe) / 1e6))


if __name__ == "__main__":
    main()
