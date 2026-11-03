"""Headless installer/Studio smoke test, with all test data in build_installer/.

python host/installer/smoke_test.py --installer build_installer/LakeShark-Setup-2.20.0.exe --compiler C:/.../riscv32-esp-elf-gcc.exe
Use --exe to test an already built exe, or --emulated for frozen code/assets under Python.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request

REPO = Path(__file__).resolve().parents[2]
FLAGS = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0


def run(cmd, **kw):
    print(subprocess.list2cmdline(list(map(str, cmd))), flush=True)
    return subprocess.run(list(map(str, cmd)), check=True, creationflags=FLAGS, **kw)


def stop(proc):
    if os.name == "nt":
        import ctypes
        if getattr(proc, "_smoke_job", None):
            ctypes.windll.kernel32.CloseHandle(proc._smoke_job)
        else:
            proc.terminate()
    else:
        proc.terminate()
    proc.wait(timeout=15)


def own_process_tree(proc):
    """Kill PyInstaller's child as well as its bootloader when the test finishes."""
    if os.name != "nt":
        return
    import ctypes
    from ctypes import wintypes as w

    class Basic(ctypes.Structure):
        _fields_ = [("process_time", ctypes.c_int64), ("job_time", ctypes.c_int64),
                    ("flags", w.DWORD), ("min_working", ctypes.c_size_t), ("max_working", ctypes.c_size_t),
                    ("active", w.DWORD), ("affinity", ctypes.c_size_t), ("priority", w.DWORD), ("scheduling", w.DWORD)]

    class IO(ctypes.Structure):
        _fields_ = [(n, ctypes.c_uint64) for n in ("read_ops", "write_ops", "other_ops", "read", "write", "other")]

    class Extended(ctypes.Structure):
        _fields_ = [("basic", Basic), ("io", IO)] + [(n, ctypes.c_size_t) for n in ("process_mem", "job_mem", "peak_process", "peak_job")]

    k = ctypes.windll.kernel32
    k.CreateJobObjectW.restype = w.HANDLE
    k.SetInformationJobObject.argtypes = [w.HANDLE, ctypes.c_int, ctypes.c_void_p, w.DWORD]
    k.AssignProcessToJobObject.argtypes = [w.HANDLE, w.HANDLE]
    k.CloseHandle.argtypes = [w.HANDLE]
    job = k.CreateJobObjectW(None, None)
    limits = Extended()
    limits.basic.flags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    if not job or not k.SetInformationJobObject(job, 9, ctypes.byref(limits), ctypes.sizeof(limits)):
        raise ctypes.WinError()
    if not k.AssignProcessToJobObject(job, int(proc._handle)):
        k.CloseHandle(job)
        raise ctypes.WinError()
    proc._smoke_job = job


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    src = ap.add_mutually_exclusive_group(required=True)
    src.add_argument("--installer", type=Path)
    src.add_argument("--exe", type=Path)
    src.add_argument("--emulated", action="store_true")
    ap.add_argument("--compiler", required=True, type=Path)
    ap.add_argument("--port", type=int, default=8811)
    args = ap.parse_args()
    compiler = args.compiler.resolve()
    assert compiler.is_file(), compiler
    out = REPO / "build_installer"
    out.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="smoke-", dir=out) as tmp:
        tmp = Path(tmp)
        dest = tmp / "installed"
        env = dict(os.environ, USERPROFILE=str(tmp / "profile"), LOCALAPPDATA=str(tmp / "local"),
                   IDF_TOOLS_PATH=str(tmp / "no-idf"), LAP_CC=str(tmp / "missing-gcc.exe"),
                   PATH=str(Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32"))
        env.pop("PYTHONPATH", None)
        (tmp / "profile").mkdir()
        installed = False
        try:
            if args.installer:
                run([args.installer.resolve(), "/VERYSILENT", "/SUPPRESSMSGBOXES", "/CURRENTUSER",
                     "/DIR=" + str(dest), '/TASKS=', "/LOG=" + str(tmp / "install.log")])
                installed = True
                exe = dest / "LakeShark.exe"
                print("PASS silent per-user installation; compiler task disabled")
            else:
                exe = args.exe.resolve() if args.exe else None
            if args.emulated:
                launcher = tmp / "frozen_test.py"
                launcher.write_text("import runpy,sys\nsys.path.insert(0,%r)\nsys.frozen=True\nsys._MEIPASS=%r\n"
                                    "runpy.run_path(%r,run_name='__main__')\n" %
                                    (str(REPO / "host/lsterm"), str(REPO / "build_app"), str(REPO / "host/lsterm/lsapp.py")), encoding="utf-8")
                cmd = [sys.executable, str(launcher)]
                print("EMULATED frozen imports/assets: this does not verify a PyInstaller executable")
            else:
                cmd = [str(exe)]
            base = "http://127.0.0.1:%d" % args.port

            def get(path):
                with urllib.request.urlopen(base + path, timeout=30) as r:
                    return r.read()

            def api(name, body=None):
                req = urllib.request.Request(base + "/api/" + name,
                                             data=json.dumps(body).encode() if body is not None else None,
                                             headers={"X-Studio": "1", "Content-Type": "application/json"})
                with urllib.request.urlopen(req, timeout=90) as r:
                    return json.load(r)

            def launch():
                command = cmd + ["--studio-only", "--port", str(args.port)]
                print(subprocess.list2cmdline(command), flush=True)
                p = subprocess.Popen(command, env=env, cwd=tmp, creationflags=FLAGS,
                                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                try:
                    own_process_tree(p)
                    deadline = time.monotonic() + 60
                    while time.monotonic() < deadline:
                        if p.poll() is not None:
                            raise AssertionError("Studio exited: %d" % p.returncode)
                        try:
                            if b"LakeShark Studio" in get("/"):
                                return p
                        except (OSError, urllib.error.URLError):
                            time.sleep(.2)
                    raise AssertionError("Studio did not start")
                except BaseException:
                    stop(p)
                    raise

            proc = launch()
            try:
                state = api("state")
                assert state["frozen"]
                assert state["workspace"] == str(tmp / "profile/Documents/LakeShark Apps")
                for asset in ("/static/studio.js", "/static/studio.css", "/static/vendor/codemirror.js", "/screen/screen.html", "/screen/screen.js"):
                    assert len(get(asset)) > 100
                js = get("/static/studio.js").decode()
                for asset in re.findall(r"['\"]((?:\./)?vendor/[^'\"]+)['\"]", js):
                    assert len(get("/static/" + asset.removeprefix("./"))) > 0
                assert api("api")["entries"]
                assert api("ai/config")["tools"]
                assert api("compiler")["mb"] == 706
                print("PASS Studio page, static/screen assets, API docs, AI contract and compiler status")
                proj = api("new", {"name": "studio-smoke", "template": "blank", "author": "Smoke test"})
                assert (Path(proj["path"]) / "main.c").is_file()
                q = "?p=" + urllib.parse.quote(proj["path"])
                result = api("build" + q, {})
                assert not result["ok"] and result["error_code"] == "compiler_missing", result
                print("PASS template creation; clean missing-compiler build response:", result["output"])
                ai = api("ai/run", {"tool": "build", "args": {"project": proj["path"]}})
                assert not ai["result"]["ok"] and "compiler" in ai["result"]["problems"][0]["message"].lower(), ai
                try:
                    api("term", {"p": proj["path"], "line": "lsdk build"})
                    raise AssertionError("frozen terminal must not spawn Python")
                except urllib.error.HTTPError as e:
                    assert "source SDK and Python" in json.load(e)["message"]
                sim = api("run" + q, {})
                assert sim["error_code"] == "host_compiler_missing", sim
                print("PASS frozen terminal message and simulator compiler download prompt; AI builds in-process")
                bundled = Path(state["sdk"]) / "sim/lssim.exe"
                run([bundled, "lap", "-f", "1", "-o", str(tmp / "sim.bmp")], env=env, cwd=tmp,
                    capture_output=True, timeout=30)
                assert (tmp / "sim.bmp").stat().st_size > 100
                print("PASS bundled lssim and DLLs run with only System32 on PATH")
            finally:
                stop(proc)
            env["LAP_CC"] = str(compiler)
            proc = launch()
            try:
                result = api("build" + q, {})
                assert result["ok"], result
                assert result["manifest"]["id"] == "studio-smoke", result
                assert (Path(proj["path"]) / "build/studio-smoke.lap").is_file()
                print("PASS build with installed compiler: %d bytes; %s" % (result["size"], result["manifest"]["id"]))
            finally:
                stop(proc)
            # An unreachable local proxy checks the headless download flag's error handling
            # without downloading 706 MB or contacting Espressif during this smoke test.
            offline = dict(env, HTTPS_PROXY="http://127.0.0.1:1", NO_PROXY="")
            command = cmd + ["--get-compiler"]
            print(subprocess.list2cmdline(command), flush=True)
            failed = subprocess.run(command, env=offline, cwd=tmp, creationflags=FLAGS,
                                    stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=30)
            assert failed.returncode == 1, failed.returncode
            report = json.loads((tmp / "local/LakeShark/toolchain/install-result.json").read_text())
            assert report["state"] == "error" and report["message"], report
            print("PASS --get-compiler reports a download failure without a GUI")
        finally:
            if installed:
                run([dest / "unins000.exe", "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART"], timeout=60)
                deadline = time.monotonic() + 20
                while (dest / "LakeShark.exe").exists() and time.monotonic() < deadline:
                    time.sleep(.2)
                assert not (dest / "LakeShark.exe").exists()
                print("PASS silent uninstall")


if __name__ == "__main__":
    main()
