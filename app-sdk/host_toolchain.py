"""Pinned, per-user Windows host C compiler for frozen Studio simulation.

Official release metadata (including SHA-256):
https://api.github.com/repos/skeeto/w64devkit/releases/tags/v2.4.0
The verified console SFX extracts silently; no installer or PATH changes.
"""
import math
import subprocess
import sys
import threading

import toolchain

VERSION = "w64devkit-2.4.0"
URL = "https://github.com/skeeto/w64devkit/releases/download/v2.4.0/w64devkit-x64-2.4.0.7z.exe"
SHA256 = "80678c29a56982d7d3b342f12a0060157df5654c38d82d4adc40704070d6aafb"
SIZE = 38806674
STRIP = "strip.exe"
root = toolchain.root
_guard = threading.Lock()
_job = {"state": "idle", "received": 0, "total": SIZE, "message": ""}


def compiler_path(base=None):
    return (base or root() / VERSION) / "w64devkit" / "bin" / "gcc.exe"


def info():
    return {"version": VERSION, "url": URL, "sha256": SHA256, "size": SIZE,
            "mb": math.ceil(SIZE / 1_000_000), "installed": compiler_path().is_file(),
            "path": str(compiler_path())}


def extract(archive, dest):
    r = subprocess.run([str(archive), "-y", "-o" + str(dest)], capture_output=True,
                       creationflags=subprocess.CREATE_NO_WINDOW, timeout=300)
    if r.returncode:
        raise ValueError("Compiler extraction failed: " + r.stdout.decode(errors="replace")[-1000:])


def install(progress=lambda **kw: None):
    return toolchain.install(progress, package=sys.modules[__name__])


def status():
    with _guard:
        return dict(_job, **info())


def start():
    with _guard:
        if _job["state"] == "running":
            return dict(_job, **info())
        _job.update(state="running", received=0, message="Starting simulator compiler download")

    def update(**kw):
        with _guard:
            _job.update(kw)

    def worker():
        try:
            install(update)
            update(state="done", received=SIZE, message="Simulator compiler ready")
        except Exception as e:
            update(state="error", message=str(e))

    threading.Thread(target=worker, daemon=True, name="Simulator compiler installer").start()
    return status()
