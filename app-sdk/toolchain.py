"""Per-user Espressif compiler installer. Standard library only; no PATH changes.

Pinned from ESP-IDF v5.5.4 tools/tools.json (riscv32-esp-elf, win64).
Upstream: https://github.com/espressif/esp-idf/blob/v5.5.4/tools/tools.json
"""
import contextlib
import hashlib
import json
import math
import os
from pathlib import Path
import shutil
import stat
import sys
import tempfile
import threading
import time
import urllib.request
import zipfile

VERSION = "esp-14.2.0_20260121"
URL = ("https://github.com/espressif/crosstool-NG/releases/download/" + VERSION +
       "/riscv32-esp-elf-14.2.0_20260121-x86_64-w64-mingw32.zip")
SHA256 = "c2734714e59f684b74d1cb278f75b4ae9ec546aa1e55d73786ef039410e4a8f7"
SIZE = 705866147
_guard = threading.Lock()
_job = {"state": "idle", "received": 0, "total": SIZE, "message": ""}


def root():
    return Path(os.environ.get("LOCALAPPDATA") or Path.home() / "AppData" / "Local") / "LakeShark" / "toolchain"


def compiler_path(base=None):
    return Path(base or root() / VERSION) / "riscv32-esp-elf" / "bin" / "riscv32-esp-elf-gcc.exe"


def info():
    return {"version": VERSION, "url": URL, "sha256": SHA256, "size": SIZE,
            "mb": math.ceil(SIZE / 1_000_000), "installed": compiler_path().is_file(),
            "path": str(compiler_path())}


@contextlib.contextmanager
def install_lock(base):
    """An OS lock is released even if the app crashes during a download."""
    with (base / ".install.lock").open("a+b") as f:
        if f.tell() == 0:
            f.write(b"\0")
            f.flush()
        if os.name == "nt":
            import msvcrt
            deadline = time.monotonic() + 1800
            while True:
                try:
                    f.seek(0)
                    msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
                    break
                except OSError:
                    if time.monotonic() > deadline:
                        raise ValueError("Another compiler installation is still running. Try again later.")
                    time.sleep(0.5)
            try:
                yield
            finally:
                f.seek(0)
                msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            import fcntl
            fcntl.flock(f, fcntl.LOCK_EX)
            try:
                yield
            finally:
                fcntl.flock(f, fcntl.LOCK_UN)


def install(progress=lambda **kw: None, *, package=None):
    """Download, verify, extract privately, then atomically expose the compiler."""
    spec = package or sys.modules[__name__]
    base = spec.root()
    base.mkdir(parents=True, exist_ok=True)
    progress(state="running", message="Waiting for the compiler installer")
    with install_lock(base):
        if spec.compiler_path().is_file():
            return spec.compiler_path()
        with tempfile.TemporaryDirectory(prefix=".download-", dir=base) as tmp:
            tmp = Path(tmp)
            archive = tmp / ("compiler.exe" if hasattr(spec, "extract") else "compiler.zip")
            digest, received = hashlib.sha256(), 0
            progress(state="running", message="Downloading compiler", received=0, total=spec.SIZE)
            req = urllib.request.Request(spec.URL, headers={"User-Agent": "LakeShark/2.20.0"})
            with urllib.request.urlopen(req, timeout=60) as response, archive.open("wb") as out:
                while True:
                    chunk = response.read(1024 * 1024)
                    if not chunk:
                        break
                    received += len(chunk)
                    if received > spec.SIZE:
                        raise ValueError("Compiler download has an unexpected size. Please retry.")
                    digest.update(chunk)
                    out.write(chunk)
                    progress(received=received, message="Downloading compiler")
            if received != spec.SIZE or digest.hexdigest() != spec.SHA256:
                raise ValueError("Compiler download failed SHA-256 verification. Please retry.")
            extracted = tmp / "unpacked"
            extracted.mkdir()
            progress(message="SHA-256 verified; extracting compiler")
            if hasattr(spec, "extract"):
                spec.extract(archive, extracted)
            else:
                extract_zip(archive, extracted, progress)
            cc = spec.compiler_path(extracted)
            if not cc.is_file() or not cc.with_name(spec.STRIP if hasattr(spec, "STRIP") else "riscv32-esp-elf-strip.exe").is_file():
                raise ValueError("Compiler archive did not contain the expected tools")
            target = base / spec.VERSION
            if target.exists():
                shutil.rmtree(target)
            extracted.rename(target)
    return spec.compiler_path()


def extract_zip(archive, extracted, progress):
    with zipfile.ZipFile(archive) as z:
        members = z.infolist()
        for i, member in enumerate(members):
            # Reject path traversal, Windows drive/ADS names and symlinks.
            rel = Path(member.filename.replace("\\", "/"))
            if (rel.is_absolute() or ".." in rel.parts or ":" in member.filename or
                    stat.S_ISLNK(member.external_attr >> 16)):
                raise ValueError("Unsafe path in compiler archive")
            z.extract(member, extracted)
            if i % 100 == 0:
                progress(message="Extracting compiler (%d%%)" % (100 * i // max(1, len(members))))


def status():
    with _guard:
        return dict(_job, **info())


def start():
    with _guard:
        if _job["state"] == "running":
            return dict(_job, **info())
        _job.update(state="running", received=0, message="Starting compiler download")

    def update(**kw):
        with _guard:
            _job.update(kw)

    def worker():
        try:
            install(update)
            update(state="done", received=SIZE, message="Compiler ready")
        except Exception as e:
            update(state="error", message=str(e))
    threading.Thread(target=worker, daemon=True, name="Compiler installer").start()
    return status()


def main():
    """Headless installer task; errors also go to a file for the windowed exe."""
    try:
        install(lambda **kw: print(kw.get("message", ""), flush=True) if sys.stdout else None)
        result, code = {"state": "done", "message": "Compiler ready", **info()}, 0
    except Exception as e:
        result, code = {"state": "error", "message": str(e)}, 1
    base = root()
    base.mkdir(parents=True, exist_ok=True)
    (base / "install-result.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
    if sys.stdout:
        print(json.dumps(result))
    return code


if __name__ == "__main__":
    sys.exit(main())
