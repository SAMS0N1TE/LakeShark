"""LakeShark Studio: an IDE for board apps, in the browser, on this computer.

    python app-sdk/lsdk.py studio [--port 8790] [--no-browser] [--workspace DIR]

Serves the page (web/) on 127.0.0.1 only. The run panel is the board's own
web screen (components/apps/tui/web/screen.js) fed by `lssim lap --feed`, so
an app looks exactly as it will on the board. The desktop bridge
(host/lsterm/lsweb.py) mounts the same thing at /studio/ and lends it the
board's port; on its own, Studio reaches the board through that bridge when
it runs, else over USB serial.

Every request that changes something needs the header X-Studio: 1 and a
Host of 127.0.0.1 or localhost, so another web page cannot drive it.
"""
import base64
import hashlib
import json
import math
import os
import re
import shutil
import socket
import struct
import sys
import threading
import time
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib.parse import parse_qs, unquote, urlparse

HERE = Path(sys._MEIPASS) / "app-sdk" / "studio" if getattr(sys, "frozen", False) else Path(__file__).resolve().parent
SDK = HERE.parent
REPO = SDK.parent
WEB = HERE / "web"
SCREEN = REPO / "components" / "apps" / "tui" / "web"
if getattr(sys, "frozen", False):
    SCREEN = Path(sys._MEIPASS) / "web"
sys.path.insert(0, str(SDK))
import apidoc  # noqa: E402
import lapboard  # noqa: E402
import lsdk  # noqa: E402
import lsstore  # noqa: E402

TYPES = {".html": "text/html; charset=utf-8", ".js": "text/javascript; charset=utf-8", ".css": "text/css",
         ".png": "image/png", ".woff2": "font/woff2", ".gz": "application/octet-stream", ".txt": "text/plain; charset=utf-8",
         ".json": "application/json", ".svg": "image/svg+xml"}
EDITABLE = {".c", ".h", ".json", ".md", ".txt", ".obj", ".py"}
GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


def default_workspace():
    if getattr(sys, "frozen", False):
        return Path(os.environ.get("USERPROFILE") or Path.home()) / "Documents" / "LakeShark Apps"
    return Path(os.environ.get("LSDK_WORKSPACE") or Path.home() / "LakeSharkApps")


# ------------------------------------------------------------------ websocket

def ws_send(sock, data, text=False):
    head = bytes([0x81 if text else 0x82])
    n = len(data)
    head += bytes([n]) if n < 126 else bytes([126]) + struct.pack(">H", n) if n < 65536 else bytes([127]) + struct.pack(">Q", n)
    sock.sendall(head + data)


def ws_recv(sock):
    def exact(n):
        b = b""
        while len(b) < n:
            got = sock.recv(n - len(b))
            if not got:
                return None
            b += got
        return b
    h = exact(2)
    if not h:
        return None
    op, n = h[0] & 15, h[1] & 127
    if n == 126:
        n = struct.unpack(">H", exact(2))[0]
    elif n == 127:
        n = struct.unpack(">Q", exact(8))[0]
    mask = exact(4) if h[1] & 128 else b"\0\0\0\0"
    data = bytearray(exact(n) or b"")
    for i in range(len(data)):
        data[i] ^= mask[i % 4]
    return op, bytes(data)


# ------------------------------------------------------------------ the logs

class Log:
    """What the sim and the board said, numbered, for the page to poll."""

    def __init__(self, keep=2000):
        self.lines, self.seq, self.keep = [], 0, keep
        self.cv = threading.Condition()

    def add(self, src, text):
        with self.cv:
            self.seq += 1
            self.lines.append((self.seq, src, text[:400]))
            del self.lines[:-self.keep]
            self.cv.notify_all()

    def since(self, n, wait=0.0):
        with self.cv:
            if wait and self.seq <= n:
                self.cv.wait(wait)
            return self.seq, [{"n": s, "src": src, "text": t} for s, src, t in self.lines if s > n]


# ------------------------------------------------------------------ the sim

class Sim:
    """One lssim --feed at a time, and the one page that watches it."""

    def __init__(self, log):
        self.log = log
        self.proc = None
        self.begin = None
        self.app = None
        self.landscape = False
        self.no_radio = False
        self.lock = threading.Lock()
        self.input_lock = threading.Lock()
        self.viewer = None                  # (socket, send lock)
        self.generation = 0

    def start(self, app, landscape=False, no_radio=False):
        self.stop()
        with self.lock:
            self.generation += 1
            self.app, self.landscape, self.no_radio = Path(app), landscape, no_radio
            self.begin = None
            self.proc = lsdk.sim_feed_process(app, landscape, no_radio)
            threading.Thread(target=self._pump, args=(self.proc, self.generation), daemon=True).start()
        end = time.time() + 10
        while self.begin is None and time.time() < end and self.proc.poll() is None:
            time.sleep(0.05)
        return self.begin is not None

    def stop(self):
        with self.lock:
            p, self.proc = self.proc, None
            v, self.viewer = self.viewer, None
        if v:
            try:
                v[0].shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        if p and p.poll() is None:
            try:
                p.stdin.write(b"quit\n")
                p.stdin.flush()
                p.wait(2)
            except Exception:
                p.kill()

    def running(self):
        return self.proc is not None and self.proc.poll() is None

    def say(self, line):
        with self.input_lock:
            p = self.proc
            if p and p.poll() is None:
                try:
                    p.stdin.write(line.encode() + b"\n")
                    p.stdin.flush()
                except OSError:
                    pass

    def touch(self, event):
        phase, x, y = event.get("phase"), event.get("x"), event.get("y")
        if phase not in ("down", "move", "up", "cancel") or any(
                type(v) is not int or not 0 <= v <= 65535 for v in (x, y)):
            raise ValueError("touch needs down/move/up/cancel and integer grid pixels x, y")
        self.say("touch %s %d %d" % (phase, x, y))

    def tilt(self, event):
        phase = event.get("phase", "move")
        if phase in ("release", "reset", "shake"):
            self.say("step " + {"release": "release", "reset": "level", "shake": "shake"}[phase])
        elif phase == "move":
            x, y = event.get("x"), event.get("y")
            if any(type(v) not in (int, float) or not math.isfinite(v) or abs(v) > 45 for v in (x, y)):
                raise ValueError("tilt needs finite x, y degrees in -45..45")
            self.say("step motion=%.4f,%.4f" % (x, y))
        else:
            raise ValueError("tilt needs move/release/reset/shake")

    def _pump(self, proc, gen):
        buf, text = bytearray(), bytearray()
        while True:
            data = proc.stdout.read1(65536) if hasattr(proc.stdout, "read1") else os.read(proc.stdout.fileno(), 65536)
            if not data:
                break
            buf += data
            while buf:
                i = buf.find(b"\x1e")
                if i < 0:
                    text += buf
                    buf.clear()
                    break
                text += buf[:i]
                del buf[:i]
                if len(buf) < 5:
                    break
                try:
                    n = int(buf[1:5], 16)
                except ValueError:
                    del buf[:1]
                    continue
                if len(buf) < 10 + n:
                    break
                frame = bytes(buf[:10 + n])
                del buf[:10 + n]
                v = self.viewer
                if v and gen == self.generation:
                    try:
                        with v[1]:
                            ws_send(v[0], frame)
                    except OSError:
                        self.viewer = None
            *done, rest = bytes(text).split(b"\n")
            text = bytearray(rest)
            for raw in done:
                line = raw.decode("utf-8", "replace").strip()
                if line.startswith("term: web begin"):
                    self.begin = line
                elif line:
                    self.log.add("sim", line)
        if gen == self.generation:
            self.log.add("sim", "the simulator stopped (exit %s)" % proc.poll())

    def session(self, sock):
        """The run panel's screen.js on its WebSocket, answered as lsweb answers."""
        lock = threading.Lock()

        def send(data, text=False):
            with lock:
                ws_send(sock, data, text)
        try:
            while True:
                m = ws_recv(sock)
                if not m or m[0] == 8:
                    break
                if m[0] != 1:
                    continue
                t = m[1].decode(errors="replace")
                if t.startswith(("token ", "login ")):
                    send(b"ok local", True)
                elif t.startswith("term web ack"):
                    if not self.running() or not self.begin:
                        break                            # nothing running: the page tries again
                    self.say("touch cancel 0 0")          # a replaced viewer may have held a finger
                    self.tilt({"phase": "release"})
                    old, self.viewer = self.viewer, (sock, lock)
                    if old and old[0] is not sock:
                        try:
                            old[0].shutdown(socket.SHUT_RDWR)
                        except OSError:
                            pass
                    send(self.begin.encode(), True)
                    self.say("redraw")
                elif t == "term caps":
                    send(b"term: caps", True)            # VT input: 'term in' bytes
                elif t.startswith("term in "):
                    self.say("in " + re.sub(r"[^0-9a-fA-F]", "", t[8:])[:1024])
                elif t.startswith("term touch "):
                    m = re.fullmatch(r"term touch (down|move|up|cancel) (\d{1,5}) (\d{1,5})", t)
                    if m and int(m[2]) <= 65535 and int(m[3]) <= 65535:
                        self.touch({"phase": m[1], "x": int(m[2]), "y": int(m[3])})
                elif t.startswith("term tilt "):
                    try:
                        self.tilt(json.loads(t[10:]))
                    except (ValueError, TypeError, AttributeError):
                        pass
                elif t == "term redraw":
                    self.say("redraw")
        except OSError:
            pass
        finally:
            if self.viewer and self.viewer[0] is sock:
                self.say("touch cancel 0 0")
                self.tilt({"phase": "release"})
                self.viewer = None


# ------------------------------------------------------------------ the board

class BoardSide:
    """How Studio reaches the board, and its console log when asked."""

    def __init__(self, log, provider=None):
        self.log = log
        self.provider = provider            # lsweb lends its own link when it mounts Studio
        self.tailing = False
        self.tail_thread = None
        self.job = {"state": "idle"}
        self.link_cache = None

    def link(self):
        if self.provider:
            return self.provider()
        return lapboard.link()

    def describe(self):
        try:
            lk = self.link()
            return {"ok": True, "kind": lk.kind, "text": lk.describe()}
        except Exception as e:
            return {"ok": False, "text": str(e)}

    def set_tail(self, on):
        if on and not self.tailing:
            self.tailing = True
            self.tail_thread = threading.Thread(target=self._tail, daemon=True)
            self.tail_thread.start()
        elif not on:
            self.tailing = False

    def _tail(self):
        try:
            lk = self.link()
            self.log.add("board", "-- the board's console through %s" % lk.describe())
            lk.tail(lambda line: self.log.add("board", line), lambda: not self.tailing)
        except Exception as e:
            self.log.add("board", "-- board log: %s" % e)
        self.tailing = False

    def install(self, app):
        """Build, then install, in the background; progress in self.job."""
        if self.job.get("state") == "running":
            return False
        self.job = {"state": "running", "sent": 0, "total": 0, "message": "building", "app": str(app)}
        threading.Thread(target=self._install, args=(app,), daemon=True).start()
        return True

    def _install(self, app):
        was_tailing = self.tailing
        try:
            res = lsdk.build(app)
            if not res.ok:
                self.job = {"state": "error", "message": "the build failed: see Problems", "problems": res.problems}
                return
            data = Path(res.lap).read_bytes()
            lk = self.link()
            if lk.kind == "serial" and was_tailing:     # one opener of the port at a time
                self.tailing = False
                time.sleep(0.8)
            self.job.update(total=len(data), message="connecting to the board", via=lk.describe(),
                            id=res.manifest["id"])

            def progress(sent, total, text):
                self.job.update(sent=sent, total=total, message=text)
            name, manifest, reply = lapboard.install(lk, data, progress)
            self.job.update(state="done", message="installed as " + name, reply=reply, sent=len(data))
            self.log.add("board", "-- %s: %s" % (name, reply))
        except Exception as e:
            self.job.update(state="error", message=str(e))
        finally:
            if was_tailing and not self.tailing:
                self.set_tail(True)


# ------------------------------------------------------------------ Studio

TEXT = EDITABLE | {".mtl", ".ini", ".cfg", ".csv", ".toml", ".yml", ".yaml", ".gitignore", ".sh", ".cmd"}
IMAGES = {".png", ".bmp", ".jpg", ".jpeg", ".gif", ".svg", ".webp"}
SKIP = {"build", ".git", "__pycache__", ".studio-trash"}
LSDK_WORDS = {"new", "build", "sim", "install", "logs", "info", "rm", "list", "snippets", "help", "whoami", "submissions"}


def settings_path():
    """Per user, outside every repository: Studio's own settings (the AI's too)."""
    return Path(os.environ.get("LSDK_STUDIO_SETTINGS") or Path.home() / ".lakeshark" / "studio.json")


_updater = None
_updater_lock = threading.Lock()
UPDATE_RELAUNCH = None  # lsapp sets this when its Studio window has been opened.


def updater():
    global _updater
    with _updater_lock:
        if _updater is None:
            import updates
            _updater = updates.Updater(settings_path())
    return _updater


class Studio:
    def __init__(self, workspace=None, provider=None, mounted=False):
        self.workspace = Path(workspace or default_workspace()).resolve()
        self.workspace.mkdir(parents=True, exist_ok=True)
        self.log = Log()
        self.sim = Sim(self.log)
        self.board = BoardSide(self.log, provider)
        self.mounted = mounted
        self.busy = threading.Lock()
        self.api = None
        self.seen = time.time()
        self.term = None
        self.trash = []                     # [(trash path, original path)], newest last
        self.meta_lock = threading.Lock()
        self.meta = self.load_meta()
        self.store_device = None            # the GitHub device code being waited on (never sent to the page)
        import ai
        self.ai = ai.Assistant(self)

    # ---- the workspace's memory: recent projects, pins

    def meta_file(self):
        return self.workspace / ".studio.json"

    def load_meta(self):
        try:
            m = json.loads(self.meta_file().read_text(encoding="utf-8"))
        except (OSError, ValueError):
            m = {}
        m.setdefault("recent", [])
        if "project" in m and not m["recent"]:          # Studio 1's one remembered app
            p = self.workspace / m.pop("project")
            if p.is_dir():
                m["recent"].append({"path": str(p), "opened": time.time(), "pinned": False})
        return m

    def save_meta(self):
        with self.meta_lock:
            try:
                tmp = self.meta_file().with_suffix(".tmp")
                tmp.write_text(json.dumps(self.meta, indent=1), encoding="utf-8")
                tmp.replace(self.meta_file())
            except OSError:
                pass

    def touch_recent(self, path):
        path = str(Path(path).resolve())
        rows = [r for r in self.meta["recent"] if r["path"] != path]
        old = next((r for r in self.meta["recent"] if r["path"] == path), None)
        rows.insert(0, {"path": path, "opened": time.time(), "pinned": bool(old and old.get("pinned"))})
        self.meta["recent"] = rows[:40]
        self.save_meta()

    def recent(self):
        out = []
        for r in self.meta["recent"]:
            p = Path(r["path"])
            out.append({"path": r["path"], "name": p.name, "opened": r.get("opened", 0), "pinned": bool(r.get("pinned")),
                        "exists": p.is_dir(), "thumb": (p / "build" / "studio-thumb.png").exists(),
                        "inside": self.workspace in p.parents})
        listed = {r["path"] for r in out}
        for name in self.projects():                    # every app in the workspace shows, opened or not
            p = self.workspace / name
            if str(p.resolve()) not in listed:
                out.append({"path": str(p.resolve()), "name": name, "opened": 0, "pinned": False, "exists": True,
                            "thumb": (p / "build" / "studio-thumb.png").exists(), "inside": True})
        out.sort(key=lambda r: (not r["pinned"], -r["opened"]))
        return out

    def projects(self):
        return sorted(d.name for d in self.workspace.iterdir() if d.is_dir() and lsdk.lap.project_sources(d))

    def known(self, path):
        """A project Studio may touch: in the workspace, or one the owner opened."""
        if not path:
            raise ValueError("no project given")
        p = Path(path)
        if not p.is_absolute():
            p = self.workspace / path
        p = p.resolve()
        if not p.is_dir():
            raise ValueError("no such app folder: %s" % p)
        if self.workspace not in p.parents and not any(Path(r["path"]) == p for r in self.meta["recent"]):
            raise ValueError("open it first (File > Open folder): %s" % p)
        return p

    # ---- files

    def tree(self, proj):
        def walk(d):
            items = []
            for p in sorted(d.iterdir(), key=lambda x: (not x.is_dir(), x.name.lower())):
                if p.name in SKIP:
                    continue
                rel = p.relative_to(proj).as_posix()
                if p.is_dir():
                    items.append({"path": rel, "name": p.name, "dir": True, "children": walk(p)})
                else:
                    ext = p.suffix.lower() or p.name.lower()
                    items.append({"path": rel, "name": p.name, "size": p.stat().st_size,
                                  "kind": "image" if ext in IMAGES else "text" if ext in TEXT else "other"})
            return items
        return walk(proj)

    def safe(self, proj, rel, text_only=False, must_exist=False):
        p = (proj / rel).resolve()
        if p != proj and proj not in p.parents:
            raise ValueError("not a file of this app: " + rel)
        if p.parts and any(part in SKIP for part in p.relative_to(proj).parts[:1]) and rel:
            raise ValueError("Studio keeps out of " + rel)
        if text_only and (p.suffix.lower() or p.name.lower()) not in TEXT:
            raise ValueError("not a text file: " + rel)
        if must_exist and not p.exists():
            raise ValueError("no such file: " + rel)
        return p

    def free_name(self, p):
        """p, or 'name (2).ext' when p is taken: nothing is ever overwritten."""
        if not p.exists():
            return p
        for k in range(2, 1000):
            q = p.with_name("%s (%d)%s" % (p.stem, k, p.suffix))
            if not q.exists():
                return q
        raise ValueError("too many copies of " + p.name)

    def fs(self, proj, j):
        op = j.get("op")
        if op == "newfile":
            p = self.safe(proj, j["path"], text_only=True)
            if p.exists():
                raise ValueError("%s already exists" % j["path"])
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(j.get("text", ""), encoding="utf-8", newline="\n")
            return {"ok": True, "path": p.relative_to(proj).as_posix()}
        if op == "mkdir":
            p = self.free_name(self.safe(proj, j["path"]))
            p.mkdir(parents=True)
            return {"ok": True, "path": p.relative_to(proj).as_posix()}
        if op in ("rename", "move"):
            moved = []
            for src, dst in j["pairs"]:
                a, b = self.safe(proj, src, must_exist=True), self.safe(proj, dst)
                if b.exists():
                    raise ValueError("%s already exists" % dst)
                if a in b.parents:
                    raise ValueError("cannot move a folder into itself")
                b.parent.mkdir(parents=True, exist_ok=True)
                a.rename(b)
                moved.append([src, b.relative_to(proj).as_posix()])
            return {"ok": True, "moved": moved}
        if op == "duplicate":
            out = []
            for rel in j["paths"]:
                a = self.safe(proj, rel, must_exist=True)
                b = self.free_name(a.with_name(a.stem + " copy" + a.suffix))
                (shutil.copytree if a.is_dir() else shutil.copy2)(a, b)
                out.append(b.relative_to(proj).as_posix())
            return {"ok": True, "paths": out}
        if op == "delete":                              # to Studio's trash, with undo
            stamp = time.strftime("%Y%m%d-%H%M%S")
            batch = []
            for rel in j["paths"]:
                a = self.safe(proj, rel, must_exist=True)
                dest = self.workspace / ".studio-trash" / stamp / proj.name / rel
                dest = self.free_name(dest)
                dest.parent.mkdir(parents=True, exist_ok=True)
                shutil.move(str(a), str(dest))
                batch.append((str(dest), str(a)))
            self.trash.append(batch)
            return {"ok": True, "trashed": len(batch), "undo": True}
        if op == "undo":
            if not self.trash:
                raise ValueError("nothing to undo")
            batch = self.trash.pop()
            back = []
            for dest, orig in batch:
                o = self.free_name(Path(orig))
                o.parent.mkdir(parents=True, exist_ok=True)
                shutil.move(dest, str(o))
                back.append(str(o))
            return {"ok": True, "restored": back}
        raise ValueError("no such file operation: %s" % op)

    def search(self, proj, q, case=False, regex=False):
        if not q:
            return {"hits": []}
        rx = re.compile(q if regex else re.escape(q), 0 if case else re.I)
        hits = []
        for p in sorted(proj.rglob("*")):
            rel = p.relative_to(proj)
            if p.is_dir() or rel.parts[0] in SKIP or (p.suffix.lower() or p.name.lower()) not in TEXT:
                continue
            for n, line in enumerate(p.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                m = rx.search(line)
                if m:
                    hits.append({"path": rel.as_posix(), "line": n, "col": m.start() + 1, "text": line.strip()[:200]})
                    if len(hits) >= 500:
                        return {"hits": hits, "more": True}
        return {"hits": hits}

    def clang_format(self):
        found = shutil.which("clang-format")
        if found:
            return found
        for pat in (r"C:\Espressif\tools\esp-clang\*\esp-clang\bin\clang-format.exe",
                    r"C:\Program Files\LLVM\bin\clang-format.exe",
                    r"C:\Program Files\JetBrains\CLion*\bin\clang\win\x64\bin\clang-format.exe"):
            hits = sorted(__import__("glob").glob(pat))
            if hits:
                return hits[-1]
        return None

    def format(self, text, name):
        cf = self.clang_format()
        if cf and name.endswith((".c", ".h")):
            style = "{BasedOnStyle: LLVM, IndentWidth: 4, ColumnLimit: 110, SortIncludes: Never}"
            import subprocess
            r = subprocess.run([cf, "--style=" + style, "--assume-filename=" + name], input=text, capture_output=True,
                               text=True, timeout=20)
            if r.returncode == 0:
                return {"text": r.stdout, "by": "clang-format"}
        lines = [l.rstrip().replace("\t", "    ") for l in text.splitlines()]
        while lines and not lines[-1]:
            lines.pop()
        return {"text": "\n".join(lines) + "\n", "by": "basic (trailing spaces, tabs, one newline at the end)"}

    def git(self, proj, rel):
        import subprocess
        try:
            top = subprocess.run(["git", "-C", str(proj), "rev-parse", "--show-toplevel", "--abbrev-ref", "HEAD"],
                                 capture_output=True, text=True, timeout=5)
        except (OSError, subprocess.SubprocessError):
            return {"repo": False}
        if top.returncode:
            return {"repo": False}
        branch = top.stdout.split()[-1] if top.stdout.split() else ""
        hunks = []
        if rel:
            d = subprocess.run(["git", "-C", str(proj), "diff", "-U0", "--no-color", "--", rel], capture_output=True,
                               text=True, timeout=10, errors="replace")
            for m in re.finditer(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@", d.stdout, re.M):
                old_n = int(m.group(2) or 1)
                new_at, new_n = int(m.group(3)), int(m.group(4) if m.group(4) is not None else 1)
                kind = "del" if new_n == 0 else "add" if old_n == 0 else "mod"
                hunks.append({"kind": kind, "from": max(1, new_at), "to": max(1, new_at + max(new_n, 1) - 1)})
            st = subprocess.run(["git", "-C", str(proj), "status", "--porcelain", "--", rel], capture_output=True,
                                text=True, timeout=5)
            if st.stdout.startswith("??"):
                hunks = [{"kind": "add", "from": 1, "to": 1 << 20}]
        return {"repo": True, "branch": branch, "hunks": hunks}

    def reveal(self, p):
        import subprocess
        if os.name == "nt":
            subprocess.Popen(["explorer", "/select,", str(p)] if p.is_file() else ["explorer", str(p)])
        elif sys.platform == "darwin":
            subprocess.Popen(["open", "-R", str(p)])
        else:
            subprocess.Popen(["xdg-open", str(p if p.is_dir() else p.parent)])

    # ---- the terminal: lsdk, nothing else

    def run_term(self, proj, line):
        if getattr(sys, "frozen", False):
            raise ValueError("The SDK terminal requires the source SDK and Python. Use Studio's Build, Run and Install commands; the AI panel also runs tools directly.")
        import shlex
        import subprocess
        words = shlex.split(line, posix=os.name != "nt")
        if words and words[0] in ("lsdk", "lsdk.py", "python"):
            words = words[1:] if words[0] != "python" else words[2:]
        if not words or words[0] not in LSDK_WORDS:
            raise ValueError("the terminal runs lsdk: " + ", ".join(sorted(LSDK_WORDS)))
        if words[0] == "help":
            words = ["--help"]
        if self.term and self.term.poll() is None:
            raise ValueError("a command is still running (Stop it first)")
        self.log.add("term", "$ lsdk " + " ".join(words))
        flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
        self.term = subprocess.Popen([sys.executable, "-u", str(SDK / "lsdk.py")] + words, cwd=str(proj or self.workspace),
                                     stdout=subprocess.PIPE, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL,
                                     text=True, errors="replace", creationflags=flags)

        def pump(p):
            for l in p.stdout:
                self.log.add("term", l.rstrip("\n"))
            self.log.add("term", "-- exit %s" % p.wait())
        threading.Thread(target=pump, args=(self.term,), daemon=True).start()
        return {"ok": True}

    # ---- state

    def state(self):
        return {"workspace": str(self.workspace), "recent": self.recent(), "projects": self.projects(),
                "templates": [{"name": k, "text": v[0]} for k, v in lsdk.TEMPLATE_INFO.items()],
                "sim": {"running": self.sim.running(), "landscape": self.sim.landscape, "no_radio": self.sim.no_radio,
                        "app": self.sim.app.name if self.sim.app else None,
                        "path": str(self.sim.app) if self.sim.app else None},
                "tailing": self.board.tailing, "job": self.board.job, "mounted": self.mounted,
                "clang_format": bool(self.clang_format()), "sdk": str(SDK), "python": sys.executable,
                "frozen": bool(getattr(sys, "frozen", False))}

    def project_info(self, proj):
        return {"path": str(proj), "name": proj.name, "id": lsdk.source_id(proj), "tree": self.tree(proj)}

    # ---- requests

    def handle(self, h, method, path):
        """Serve one request; path is below the mount point. True when handled."""
        u = urlparse(path)
        route, q = u.path, {k: v[0] for k, v in parse_qs(u.query).items()}
        if route in ("", "/", "/index.html"):
            return self.file(h, WEB / "index.html")
        if route == "/ws":
            if not self.local(h, origin=True):
                return self.reply(h, 403, {"ok": False, "message": "not from this computer"})
            return self.websocket(h)
        if route.startswith("/static/"):
            return self.file(h, (WEB / route[8:]).resolve(), base=WEB)
        if route.startswith("/screen/"):
            return self.screen_file(h, route[8:] or "screen.html")
        if route.startswith("/api/"):
            if not self.local(h, write=method != "GET"):
                return self.reply(h, 403, {"ok": False, "message": "Studio answers only its own page on this computer"})
            self.seen = time.time()
            if route == "/api/raw":
                return self.raw(h, q)
            try:
                return self.reply(h, 200, self.call(h, method, route[5:], q))
            except (ValueError, KeyError, lsdk.LsdkError, lapboard.BoardError, OSError) as e:
                return self.reply(h, 400, {"ok": False, "message": str(e)})
        return False

    def local(self, h, write=False, origin=False):
        host = (h.headers.get("Host") or "").split(":")[0]
        if host not in ("127.0.0.1", "localhost"):
            return False
        if write and h.headers.get("X-Studio") != "1":
            return False
        if origin:
            o = h.headers.get("Origin")
            if o and urlparse(o).hostname not in ("127.0.0.1", "localhost"):
                return False
        return True

    def body(self, h):
        n = int(h.headers.get("Content-Length") or 0)
        if n > 16 * 1024 * 1024:
            raise ValueError("too big")
        return h.rfile.read(n) if n else b""

    def jbody(self, h):
        return json.loads(self.body(h).decode() or "{}")

    def call(self, h, method, name, q):
        P = lambda: self.known(q.get("p"))                          # noqa: E731
        if name.startswith("updater/"):
            if not self.local(h, write=method != "GET", origin=True):
                raise ValueError("updater answers only its local page")
            action = name[8:]
            expected = "GET" if action == "status" else "POST"
            if method != expected:
                raise ValueError("updater requires " + expected)
            update = updater()
            if action == "status":
                return update.status()
            if action == "check":
                return update.check(manual=self.jbody(h).get("manual") is True)
            if action == "settings":
                return update.settings(self.jbody(h).get("automatic"))
            if action == "install":
                data = self.jbody(h)
                return update.start(UPDATE_RELAUNCH or data.get("relaunch", "studio"))
            raise ValueError("unknown updater action")
        if name == "state":
            return self.state()
        if name in ("compiler", "host-compiler"):
            if name == "host-compiler":
                import host_toolchain as compiler
            else:
                import toolchain as compiler
            return compiler.start() if method == "POST" else compiler.status()
        if name == "project":
            p = P()
            self.touch_recent(p)
            return self.project_info(p)
        if name == "open":                                          # any folder with an app in it
            p = Path(self.jbody(h)["path"]).expanduser().resolve()
            if not p.is_dir() or not lsdk.lap.project_sources(p):
                raise ValueError("no app there (a folder with .c files): %s" % p)
            self.touch_recent(p)
            return self.project_info(p)
        if name == "new":
            j = self.jbody(h)
            with self.busy:                                         # new_app never touches a folder in use
                folder = lsdk.new_app(j["name"], j.get("template", "blank"), self.workspace, j.get("author") or None)
            self.touch_recent(folder)
            return self.project_info(folder)
        if name == "recent":
            j = self.jbody(h)
            path = str(Path(j["path"]).resolve())
            if j.get("remove"):
                self.meta["recent"] = [r for r in self.meta["recent"] if r["path"] != path]
            else:
                for r in self.meta["recent"]:
                    if r["path"] == path:
                        r["pinned"] = bool(j.get("pinned"))
            self.save_meta()
            return {"ok": True, "recent": self.recent()}
        if name == "tree":
            return self.project_info(P())
        if name == "file":
            proj = P()
            p = self.safe(proj, q.get("path", ""), text_only=True)
            if method == "GET":
                return {"path": q["path"], "text": p.read_text(encoding="utf-8", errors="replace")}
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(self.body(h).decode("utf-8"), encoding="utf-8", newline="\n")
            return {"ok": True, "path": q["path"]}
        if name == "fs":
            return self.fs(P(), self.jbody(h))
        if name == "upload":                                        # a file dropped from Explorer
            proj = P()
            dest = self.free_name(self.safe(proj, q["path"]))
            dest.parent.mkdir(parents=True, exist_ok=True)
            dest.write_bytes(self.body(h))
            return {"ok": True, "path": dest.relative_to(proj).as_posix()}
        if name == "reveal":
            proj = P()
            self.reveal(self.safe(proj, q.get("path", ""), must_exist=True) if q.get("path") else proj)
            return {"ok": True}
        if name == "search":
            return self.search(P(), q.get("q", ""), q.get("case") == "1", q.get("regex") == "1")
        if name == "format":
            j = self.jbody(h)
            return self.format(j.get("text", ""), j.get("path", "x.c"))
        if name == "git":
            return self.git(P(), q.get("path"))
        if name == "build":
            proj = P()
            t = time.time()
            with self.busy:
                res = lsdk.build(proj)
            out = self.rel(proj, res.to_json())
            out["seconds"] = round(time.time() - t, 2)
            return out
        if name == "run":
            j = self.jbody(h)
            proj = P()
            t = time.time()
            with self.busy:
                self.sim.stop()
                res = lsdk.sim_build(proj, say=lambda s: self.log.add("sim", "-- " + s))
                out = self.rel(proj, res.to_json())
                if res.ok:
                    out["started"] = self.sim.start(proj, bool(j.get("landscape")), bool(j.get("no_radio")))
                    self.log.add("sim", "-- %s running in lssim (%s%s)" % (proj.name, "landscape" if j.get("landscape") else "portrait",
                                                                          ", no radio" if j.get("no_radio") else ""))
            out["seconds"] = round(time.time() - t, 2)
            return out
        if name == "stop":
            self.sim.stop()
            return {"ok": True}
        if name == "sim/touch":
            self.sim.touch(self.jbody(h))
            return {"ok": True}
        if name == "sim/tilt":
            self.sim.tilt(self.jbody(h))
            return {"ok": True}
        if name == "sim":
            spec = self.jbody(h).get("step", "")
            if not re.fullmatch(r"[a-z]+(=[@\w.,\- ]*)?", spec):
                raise ValueError("not a sim step: " + spec)
            self.sim.say("step " + spec)
            return {"ok": True}
        if name in ("thumb", "shot"):
            proj = P()
            if method == "GET":
                f = proj / "build" / "studio-thumb.png"
                return {"png": "data:image/png;base64," + base64.b64encode(f.read_bytes()).decode() if f.exists() else None}
            data = self.body(h).decode()
            png = base64.b64decode(data.split(",", 1)[1])
            if name == "thumb":
                f = proj / "build" / "studio-thumb.png"
            else:
                f = proj / "build" / "shots" / ("%s-%s.png" % (proj.name, time.strftime("%Y%m%d-%H%M%S")))
            f.parent.mkdir(parents=True, exist_ok=True)
            f.write_bytes(png)
            return {"ok": True, "file": str(f)}
        if name == "install":
            return {"ok": self.board.install(P())}
        if name == "job":
            j = dict(self.board.job)
            if j.get("app"):
                self.rel(Path(j["app"]), j)
            return j
        if name == "board":
            return self.board.describe()
        if name == "board/open":
            app_id = lsdk.source_id(P())
            return {"ok": True, "lines": lapboard.open_on_board(self.board.link(), app_id)}
        if name == "board/tail":
            self.board.set_tail(bool(self.jbody(h).get("on")))
            return {"ok": True, "tailing": self.board.tailing}
        if name == "logs":
            seq, lines = self.log.since(int(q.get("since", 0)), float(q.get("wait", 0)))
            return {"seq": seq, "lines": lines}
        if name == "term":
            j = self.jbody(h)
            if j.get("stop"):
                if self.term and self.term.poll() is None:
                    self.term.kill()
                return {"ok": True}
            return self.run_term(self.known(j["p"]) if j.get("p") else None, j.get("line", ""))
        if name == "api":
            if not self.api:
                entries = apidoc.entries()
                import api_registry
                self.api = {"entries": entries, "sections": apidoc.readme_sections(),
                            "api_version": ".".join(map(str, api_registry.version(apidoc.SDK))),
                            "simulator": [{"name": e["name"], **e["simulator"]} for e in entries if e.get("exported")]}
            return self.api
        if name.startswith("ai/"):
            return self.ai.call(name[3:], method, self.jbody(h) if method != "GET" else q)
        if name.startswith("store/"):
            return self.store_call(h, method, name[6:], q)
        raise ValueError("no such call: " + name)

    # ---- the community store: Studio holds the sign-in, the page never sees it

    def store_call(self, h, method, name, q):
        if name == "me":
            saved = lsstore.load_login()
            out = {"signed_in": bool(saved), "login": (saved or {}).get("login") or None, "api": lsstore.api_base(),
                   "waiting": bool(self.store_device)}
            if saved:
                try:
                    me = lsstore.Client(token=saved["token"]).me()
                    out.update({k: me.get(k) for k in ("login", "github_id", "account_age_days", "owned_ids", "can_submit")})
                except lsstore.StoreError as e:
                    out["error"] = str(e)
            return out
        if method == "GET" and name not in ("form", "submissions"):
            raise ValueError("no such call: store/" + name)
        if name == "login":
            s = lsstore.Client().device_start()
            if not s.get("device_code") or not s.get("user_code"):
                raise lsstore.StoreError("the store did not give a sign-in code")
            interval = max(1, int(s.get("interval") or 5))
            expires = int(s.get("expires_in") or 900)
            self.store_device = {"code": s["device_code"], "interval": interval, "until": time.time() + expires}
            return {"user_code": s["user_code"], "url": s.get("verification_uri") or lsstore.DEVICE_PAGE,
                    "interval": interval, "expires_in": expires}
        if name == "poll":
            d = self.store_device
            if not d:
                return {"state": "none"}
            if time.time() > d["until"]:
                self.store_device = None
                return {"state": "expired"}
            client = lsstore.Client()
            state, token = lsstore.poll_state(client.device_poll(d["code"]))
            if state == "slow_down":
                d["interval"] += 5
            elif state == "done":
                client.token, login = token, ""
                try:
                    login = client.me().get("login", "")
                except lsstore.StoreError:
                    pass
                lsstore.save_login(token, login)
                self.store_device = None
                return {"state": "done", "login": login}
            elif state in ("expired", "denied"):
                self.store_device = None
            return {"state": state, "interval": d["interval"]}
        if name == "logout":
            self.store_device = None
            return {"ok": True, "was": lsstore.clear_login()}
        if name == "submissions":
            subs = lsstore.Client.signed_in().submissions() or []
            return {"submissions": [dict(s, words=lsstore.STATE_WORDS.get(s.get("state"), s.get("state") or "?"),
                                         link=lsstore.STATUS_LINK) for s in subs]}
        proj = self.known(q.get("p"))
        if name == "form":
            return self.store_form(proj)
        if name == "save":
            self.store_merge(proj, self.jbody(h))
            return self.store_form(proj)
        if name == "license":                                       # an MIT LICENSE when there is none
            f = proj / "LICENSE"
            if f.exists():
                raise ValueError("this app already has a LICENSE")
            f.write_text(lsstore.mit_license(lsdk._git_name() or os.environ.get("LSDK_AUTHOR") or "the author"),
                         encoding="utf-8", newline="\n")
            return self.store_form(proj)
        if name == "publish":
            meta = self.store_merge(proj, self.jbody(h))
            errors, missing = lsstore.check_meta(meta, lsdk.source_id(proj))
            if errors or missing:
                return {"ok": False, "stage": "app.json", "errors": errors, "missing": missing}
            client = lsstore.Client.signed_in()
            with self.busy:
                res = lsdk.build(proj)
            if not res.ok:
                return {"ok": False, "stage": "build", "errors": ["the build failed: see Problems"],
                        "problems": self.rel(proj, res.to_json())["problems"]}
            if res.manifest["id"] != meta["id"]:
                return {"ok": False, "stage": "build", "errors": ["the built app's id is '%s' but app.json says '%s'" %
                                                                  (res.manifest["id"], meta["id"])]}
            try:
                data, left = lsstore.bundle(proj, meta)
            except lsstore.StoreError as e:
                return {"ok": False, "stage": "bundle", "errors": [l.strip() for l in str(e).splitlines()[1:]] or [str(e)]}
            r = client.submit(data)
            self.log.add("term", "-- sent %s %s to the store (%d KB): %s" % (meta["id"], res.manifest["version"],
                                                                            (len(data) + 1023) // 1024, lsstore.link_for(r)))
            return {"ok": True, "pr": r.get("pr"), "link": lsstore.link_for(r), "left_out": left, "size": len(data),
                    "version": res.manifest["version"]}
        raise ValueError("no such call: store/" + name)

    def store_form(self, proj):
        sid = lsdk.source_id(proj)
        meta = lsstore.read_meta(proj)
        if not meta.get("id") and sid:
            meta["id"] = sid
        errors, missing = lsstore.check_meta(meta, sid)
        lic = proj / "LICENSE"
        text = lic.read_text(encoding="utf-8", errors="replace") if lic.exists() else ""
        return {"meta": lsstore.schema2(meta, sid), "source_id": sid, "errors": errors, "missing": missing,
                "has_license": lic.exists(), "license_guess": lsstore.guess_license(text) if text else None,
                "lists": {"categories": lsstore.CATEGORIES, "licenses": [x for x in lsstore.LICENSES if x not in lsstore.ASSET_ONLY],
                          "third_licenses": lsstore.LICENSES, "ai_uses": lsstore.AI_USES, "ai_text": lsstore.AI_TEXT,
                          "third_uses": lsstore.THIRD_USES, "declarations": lsstore.DECLARATIONS}}

    def store_merge(self, proj, j):
        """The form's answers into app.json (schema 2); returns the new meta."""
        sid = lsdk.source_id(proj)
        meta = lsstore.schema2(lsstore.read_meta(proj), sid)
        for k in ("category", "description", "license", "source_url"):
            if isinstance(j.get(k), str):
                meta[k] = j[k].strip()
        ai = j.get("ai")
        if isinstance(ai, dict):
            if isinstance(ai.get("use"), str):
                meta["ai"]["use"] = ai["use"]
            if isinstance(ai.get("tools"), list):
                meta["ai"]["tools"] = [str(t).strip() for t in ai["tools"] if str(t).strip()][:20]
            if isinstance(ai.get("extent"), str):
                meta["ai"]["extent"] = ai["extent"].strip()
        if isinstance(j.get("third_party"), list):
            meta["third_party"] = [{k: str(t.get(k, "")).strip() for k in ("name", "license", "url", "use")}
                                   for t in j["third_party"][:50] if isinstance(t, dict) and str(t.get("name", "")).strip()]
        if isinstance(j.get("references"), list):
            meta["references"] = [str(r).strip() for r in j["references"][:50] if str(r).strip()]
        if isinstance(j.get("declarations"), dict):
            for k, _ in lsstore.DECLARATIONS:
                meta["declarations"][k] = j["declarations"].get(k) is True
        lsstore.write_meta(proj, meta)
        return meta

    def rel(self, proj, j):
        """Problem paths relative to the app, as the project tree names them."""
        for p in j.get("problems", []) or []:
            try:
                p["path"] = Path(p["file"]).resolve().relative_to(Path(proj).resolve()).as_posix()
            except (ValueError, OSError, KeyError):
                p["path"] = None
        if j.get("lap"):
            j["lap"] = Path(j["lap"]).name
        return j

    # ---- replies

    def reply(self, h, code, obj):
        body = json.dumps(obj).encode()
        h.send_response(code)
        h.send_header("Content-Type", "application/json")
        h.send_header("Content-Length", str(len(body)))
        h.send_header("Cache-Control", "no-store")
        h.end_headers()
        h.wfile.write(body)
        return True

    def raw(self, h, q):
        try:
            proj = self.known(q.get("p"))
            p = self.safe(proj, q.get("path", ""), must_exist=True)
        except ValueError:
            h.send_error(404)
            return True
        if p.suffix.lower() not in IMAGES:
            h.send_error(415)
            return True
        return self.file(h, p)

    def file(self, h, path, base=None, body=None):
        path = Path(path)
        if base and base.resolve() not in path.resolve().parents:
            h.send_error(404)
            return True
        if body is None:
            if not path.is_file():
                h.send_error(404)
                return True
            body = path.read_bytes()
        h.send_response(200)
        h.send_header("Content-Type", TYPES.get(path.suffix.lower(), "application/octet-stream"))
        h.send_header("Content-Length", str(len(body)))
        h.send_header("Cache-Control", "no-store")
        h.end_headers()
        h.wfile.write(body)
        return True

    def screen_file(self, h, name):
        p = (SCREEN / name).resolve()
        if SCREEN.resolve() not in p.parents or not p.is_file():
            h.send_error(404)
            return True
        body = None
        if name == "screen.html":           # signed in, on the sim's socket, and never the board's
            ws = ("/studio" if self.mounted else "") + "/ws"
            body = p.read_bytes().replace(
                b'<script src="screen.js">',
                b"<script>localStorage.setItem('lakeshark-token','usb');window.LS_WS_PATH='" + ws.encode() +
                b"'</script><script src=\"screen.js\">", 1)
            # Installed after screen.js so the sim can use its live grid/font
            # geometry, in the embedded panel and the popped-out viewer alike.
            touch = ("/studio" if self.mounted else "") + "/static/sim-touch.js"
            tilt = ("/studio" if self.mounted else "") + "/static/sim-tilt.js"
            body = body.replace(b"</body>", ('<script src="%s"></script><script src="%s"></script></body>' % (tilt, touch)).encode())
        return self.file(h, p, body=body)

    def websocket(self, h):
        key = h.headers.get("Sec-WebSocket-Key", "")
        accept = base64.b64encode(hashlib.sha1((key + GUID).encode()).digest()).decode()
        h.send_response(101)
        h.send_header("Upgrade", "websocket")
        h.send_header("Connection", "Upgrade")
        h.send_header("Sec-WebSocket-Accept", accept)
        h.end_headers()
        sock = h.connection
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.sim.session(sock)
        h.close_connection = True
        return True


# ------------------------------------------------------------------ standalone

STUDIO = None


class Handler(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if not STUDIO.handle(self, "GET", self.path):
            self.send_error(404)

    def do_POST(self):
        if not STUDIO.handle(self, "POST", self.path):
            self.send_error(404)

    do_PUT = do_POST


def serve(port=8790, workspace=None):
    global STUDIO
    STUDIO = Studio(workspace)
    return ThreadingHTTPServer(("127.0.0.1", port), Handler)


def main(port=8790, browser=True, workspace=None):
    srv = serve(port, workspace)
    url = "http://127.0.0.1:%d/" % srv.server_address[1]
    print("LakeShark Studio: %s  (apps in %s; Ctrl-C stops)" % (url, STUDIO.workspace), flush=True)
    if browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        STUDIO.sim.stop()
    return 0


if __name__ == "__main__":
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=8790)
    ap.add_argument("--no-browser", action="store_true")
    ap.add_argument("--workspace")
    a = ap.parse_args()
    sys.exit(main(a.port, not a.no_browser, a.workspace))
