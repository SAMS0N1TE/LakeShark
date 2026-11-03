"""Exercise frozen Studio's real compiler download and five app DLLs, no UI.

python host/installer/smoke_sim.py --exe build_app/dist/LakeShark.exe --download
Evidence and isolated USERPROFILE/LOCALAPPDATA remain in build_installer/sim-smoke.
Only System32 is on the tested process PATH. No Python, CMake or repo is passed.
"""
import argparse
import base64
import binascii
import hashlib
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import time
import urllib.parse
import urllib.request

from smoke_test import own_process_tree, stop

REPO = Path(__file__).resolve().parents[2]


class Screen:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=15)
        self.file = self.sock.makefile("rb")
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall((f"GET /ws HTTP/1.1\r\nHost: 127.0.0.1:{port}\r\n"
                           "Upgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\n"
                           f"Sec-WebSocket-Key: {key}\r\n\r\n").encode())
        assert b"101" in self.file.readline()
        while self.file.readline() != b"\r\n":
            pass
        self.send("token usb")
        assert self.recv()[1] == b"ok local"
        self.send("term web ack cells")
        op, data = self.recv()
        assert op == 1 and data.startswith(b"term: web begin"), data
        self.begin = data.decode()

    def send(self, text):
        data, mask = text.encode(), os.urandom(4)
        assert len(data) < 126
        self.sock.sendall(bytes([0x81, 0x80 | len(data)]) + mask +
                          bytes(b ^ mask[i % 4] for i, b in enumerate(data)))

    def recv(self):
        head = self.file.read(2)
        assert len(head) == 2, "simulator disconnected"
        op, n = head[0] & 15, head[1] & 127
        if n == 126:
            n = struct.unpack("!H", self.file.read(2))[0]
        elif n == 127:
            n = struct.unpack("!Q", self.file.read(8))[0]
        return op, self.file.read(n)

    def frame(self):
        parts = []
        while True:
            op, data = self.recv()
            assert op == 2 and data[0] == 0x1e, (op, data[:80])
            n = int(data[1:5], 16)
            assert len(data) == n + 10 and data[-5] == 0x1f
            body = data[5:5+n]
            assert binascii.crc_hqx(body, 0xffff) == int(data[-4:], 16)
            if not n:
                return b"".join(parts)
            parts.append(body)

    def close(self):
        self.file.close()
        self.sock.close()


def grid(raw):
    """Decode the same full G/P/C/X records consumed by screen.js."""
    assert raw[0] == ord("G")
    cols, rows = struct.unpack_from("<HH", raw, 1)
    cells, at = [None] * (cols * rows), 8
    while at < len(raw):
        t, at = raw[at], at + 1
        if t == ord("P"):
            at += 32
            continue
        assert t in (ord("C"), ord("X")), (t, at)
        first, n = struct.unpack_from("<HH", raw, at)
        at += 4
        size = 3 if t == ord("C") else 8
        for i in range(n):
            cells[first+i] = struct.unpack_from("<H", raw, at)[0]
            at += size
    assert all(c is not None for c in cells)
    # Exclude the board chrome; require visible content inside the app area.
    body = cells[cols*3:cols*(rows-3)]
    visible = sum(c not in (0, 32) for c in body)
    assert visible > 20, visible
    text = "\n".join("".join(chr(c) for c in cells[y*cols:(y+1)*cols]) for y in range(rows))
    return {"cols": cols, "rows": rows, "visible_app_cells": visible,
            "bytes": len(raw), "sha256": hashlib.sha256(raw).hexdigest()}, text


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--exe", type=Path, required=True)
    ap.add_argument("--download", action="store_true", help="Download if absent; never use an existing host GCC")
    ap.add_argument("--port", type=int, default=8811)
    ap.add_argument("--evidence", type=Path, default=REPO / "build_installer/sim-smoke")
    args = ap.parse_args()
    evidence = args.evidence.resolve()
    evidence.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, USERPROFILE=str(evidence / "profile"), LOCALAPPDATA=str(evidence / "local"),
               PATH=str(Path(os.environ.get("SystemRoot", "C:/Windows")) / "System32"),
               IDF_TOOLS_PATH=str(evidence / "no-idf"), LAP_CC=str(evidence / "no-board-gcc.exe"))
    for k in ("PYTHONPATH", "PYTHONHOME", "LSDK_HOST_PATH", "LAPSIM_DLL", "LAPSIM_SCRIPT",
              "GCC_EXEC_PREFIX", "COMPILER_PATH", "LIBRARY_PATH", "CPATH", "C_INCLUDE_PATH"):
        env.pop(k, None)
    (evidence / "profile").mkdir(exist_ok=True)
    base = "http://127.0.0.1:%d" % args.port

    def api(name, body=None):
        req = urllib.request.Request(base + "/api/" + name,
                                     data=body if isinstance(body, bytes) else json.dumps(body).encode() if body is not None else None,
                                     headers={"X-Studio": "1", "Content-Type": "application/json"})
        with urllib.request.urlopen(req, timeout=90) as r:
            return json.load(r)

    proc = subprocess.Popen([str(args.exe.resolve()), "--studio-only", "--port", str(args.port)],
                            env=env, cwd=evidence, creationflags=subprocess.CREATE_NO_WINDOW,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    own_process_tree(proc)
    report = {"exe": str(args.exe.resolve()), "exe_bytes": args.exe.stat().st_size, "PATH": env["PATH"]}
    try:
        deadline = time.monotonic() + 60
        while True:
            try:
                state = api("state")
                break
            except OSError:
                assert time.monotonic() < deadline and proc.poll() is None, "Studio did not start"
                time.sleep(.2)
        assert state["frozen"]
        report["sdk"] = state["sdk"]
        compiler = api("host-compiler")
        assert compiler["mb"] == 39
        if not compiler["installed"]:
            assert args.download, "Run with --download for the real verified download"
            proj = api("new", {"name": "missing-" + str(int(time.time())), "template": "blank"})
            missing = api("run?p=" + urllib.parse.quote(proj["path"]), {})
            assert missing["error_code"] == "host_compiler_missing", missing
            report["missing_compiler"] = missing
            compiler = api("host-compiler", {})
            deadline = time.monotonic() + 600
            last = ""
            while compiler["state"] == "running":
                msg = compiler["message"]
                if msg != last:
                    print(msg, flush=True)
                    last = msg
                assert time.monotonic() < deadline, compiler
                time.sleep(.5)
                compiler = api("host-compiler")
            assert compiler["state"] == "done", compiler
            report["real_download_verified"] = True
            (evidence / "download-result.json").write_text(json.dumps(compiler, indent=2), encoding="utf-8")
        assert Path(compiler["path"]).is_file()
        assert Path(env["LOCALAPPDATA"]) in Path(compiler["path"]).parents
        report["compiler"] = compiler
        receipt = evidence / "download-result.json"
        if receipt.exists():
            report["verified_download"] = json.loads(receipt.read_text(encoding="utf-8"))
        results = {}
        for template in ("blank", "radio", "dots", "3d", "service"):
            name = "sim-" + template + "-" + str(int(time.time()))
            proj = api("new", {"name": name, "template": template, "author": "Frozen sim smoke"})
            query = "?p=" + urllib.parse.quote(proj["path"])
            run = api("run" + query, {"landscape": False})
            assert run["ok"] and run["started"], (template, run, api("logs"))
            screen = Screen(args.port)
            frames, texts = [], []
            try:
                for i in range(6):
                    raw = screen.frame()
                    f, text = grid(raw)
                    frames.append(f)
                    texts.append(text)
                    (evidence / (template + "-%d.cells" % i)).write_bytes(raw)
                    if i == 0:
                        if template == "blank":
                            api("sim", {"step": "key=h"})
                        elif template == "3d":
                            api("sim", {"step": "key=@right"})
                        elif template == "service":
                            api("sim", {"step": "page=TEST frozen service"})
                unique = len({f["sha256"] for f in frames})
                assert unique > 1, (template, frames)
                if template == "blank":
                    assert any("HELLOS" in text for text in texts), texts
                if template == "service":
                    assert "watching in the background" in texts[-1], texts
                    assert "TEST frozen service" in texts[-1], texts
                (evidence / (template + ".txt")).write_text(texts[-1], encoding="utf-8")
                results[template] = {"build_seconds": run["seconds"], "begin": screen.begin,
                                     "frames": frames, "unique_frames": unique, "path": proj["path"]}
                print("PASS %s: %d frames, %d unique, %dx%d, %d visible app cells" %
                      (template, len(frames), unique, f["cols"], f["rows"], f["visible_app_cells"]), flush=True)
            finally:
                screen.close()
            api("stop", {})
            if template == "blank":
                # Edit through Studio, compile multiple C files, diagnose an error,
                # and replace a previously loaded DLL. This cannot use a built-in app.
                source = api("file" + query + "&path=main.c")["text"]
                source = source.replace('#include "lakeshark_app.h"',
                                        '#include "lakeshark_app.h"\nextern const char *smoke_label(void);')
                source = source.replace('"HELLOS"', 'smoke_label()')
                api("file" + query + "&path=main.c", source.encode())
                api("file" + query + "&path=extra.c", b'#error frozen diagnostic check\n')
                bad = api("run" + query, {})
                assert not bad["ok"] and any("frozen diagnostic check" in p["message"] for p in bad["problems"]), bad
                api("file" + query + "&path=extra.c", b'const char *smoke_label(void) { return "DLL RELOAD"; }\n')
                rebuilt = api("run" + query, {})
                assert rebuilt["ok"] and rebuilt["started"], rebuilt
                screen = Screen(args.port)
                try:
                    _, text = grid(screen.frame())
                    assert "DLL RELOAD" in text, text
                    (evidence / "blank-rebuilt.txt").write_text(text, encoding="utf-8")
                finally:
                    screen.close()
                api("stop", {})
                report["custom_multifile_rebuild_and_diagnostics"] = True
                print("PASS edited app, multiple C files, compiler diagnostics and DLL reload", flush=True)
        report["templates"] = results
        report["logs"] = api("logs")
        (evidence / "results.json").write_text(json.dumps(report, indent=2), encoding="utf-8")
        print("PASS frozen simulator; evidence:", evidence, flush=True)
    finally:
        stop(proc)


if __name__ == "__main__":
    main()
