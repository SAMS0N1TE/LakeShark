#!/usr/bin/env python3
"""LakeShark's dependency-light MCP stdio server. stdout is protocol only."""
import argparse
import base64
import contextlib
import io
import json
import math
import os
from pathlib import Path, PureWindowsPath
import re
import struct
import subprocess
import sys
import time
import zlib

SDK = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SDK))
import apidoc
import lapboard
import lsdk

VERSIONS = ("2024-11-05", "2025-03-26", "2025-06-18", "2025-11-25")
MAX_FILE = 1024 * 1024
GUARDED = ["format/formatting/mkfs/erase", "sd rm/remove/delete", "crumb clear / crash clear",
           "flash/flashing/ota/update/bootloader", "tx/transmit/send/PTT/arm, say, mesh dm/advert commands",
           "cellreport target/test", "app rm/remove/delete", "reset/reboot/factory"]


def string(**kw):
    return dict(type="string", minLength=1, maxLength=4096, **kw)


def integer(lo, hi, **kw):
    return dict(type="integer", minimum=lo, maximum=hi, **kw)


def obj(props, required=()):
    return {"type": "object", "properties": props, "required": list(required), "additionalProperties": False}


APP = string(description="App folder name directly inside the configured workspace.")
EVENT = obj({
    "type": string(enum=["key", "tap", "down", "up", "tilt", "swipe", "rx", "release_all"]),
    "frame": integer(1, 2000, description="Sim only: frame offset from this input call, default sequential."),
    "key": {"type": "string", "minLength": 1, "maxLength": 32},
    "x": integer(0, 32767), "y": integer(0, 32767),
    "col": integer(0, 32767), "row": integer(0, 32767),
    "ax": {"type": "number", "minimum": -100, "maximum": 100},
    "ay": {"type": "number", "minimum": -100, "maximum": 100},
    "az": {"type": "number", "minimum": -100, "maximum": 100},
    "direction": string(enum=["U", "D", "L", "R"]), "count": integer(0, 100000),
}, ["type"])
EVENTS = {"type": "array", "items": EVENT, "minItems": 1, "maxItems": 100}


def tool(name, description, props=None, required=()):
    return {"name": name, "description": description, "inputSchema": obj(props or {}, required)}


TOOLS = [
    tool("sdk_docs", "Search README sections and API signatures/doc comments (case-insensitive words).", {"query": string()}, ["query"]),
    tool("sdk_api", "Exact, case-sensitive API symbol signature and documentation from SDK headers.", {"symbol": string()}, ["symbol"]),
    tool("new_app", "Create an app from an SDK template inside the workspace.", {"name": APP, "template": string(enum=list(lsdk.TEMPLATE_INFO), default="blank")}, ["name"]),
    tool("list_apps", "List local workspace apps, metadata and relative files."),
    tool("read_file", "Read UTF-8 text inside an app folder (max 1 MiB).", {"app": APP, "path": string()}, ["app", "path"]),
    tool("write_file", "Write UTF-8 text inside an app folder (max 1 MiB).", {"app": APP, "path": string(), "content": {"type": "string", "maxLength": MAX_FILE}}, ["app", "path", "content"]),
    tool("build", "Build the board .lap; return ok, problems {file,line,col,severity,message}, lap, size and output.", {"app": APP}, ["app"]),
    tool("sim_run", "Build and run lssim; return final PNG and log. Resets replay history.", {
        "app": APP, "frames": integer(1, 2000, default=60), "script": {"type": "string", "maxLength": 16384},
        "orientation": string(enum=["portrait", "landscape"], default="portrait")}, ["app"]),
    tool("sim_input", "Replay the prior sim plus keys/taps/tilt/swipe/rx; return new final PNG and log.", {"app": APP, "events": EVENTS}, ["app", "events"]),
    tool("install", "Build then upload via desktop bridge if present, else serial; optionally open. Returns progress and result.", {"app": APP, "open": {"type": "boolean", "default": True}}, ["app"]),
    tool("board_console", "Run one console line. confirm=true required for: " + "; ".join(GUARDED) + ". No command separators or control characters.", {"line": string(), "confirm": {"type": "boolean", "default": False}}, ["line"]),
    tool("board_screen", "Capture tui png; validate byte count and CRC-32; return MCP PNG image."),
    tool("board_input", "Inject key/tap/down/up/swipe/release_all via term ev; return screenshot. Board cannot synthesize tilt or rx.", {"events": EVENTS}, ["events"]),
    tool("board_logs", "Last n available bridge lines, or up to 2 seconds of direct serial logs; optional [app ID] filter.", {"app": APP, "n": integer(1, 1000, default=100)}),
]
SCHEMAS = {t["name"]: t["inputSchema"] for t in TOOLS}
RESOURCES = {"lakeshark://sdk/README.md": SDK / "README.md"}
RESOURCES.update({"lakeshark://sdk/include/" + p.name: p for p in sorted((SDK / "include").glob("*.h"))})


def validate(value, schema, path="arguments"):
    """Validate the JSON Schema subset used above without third-party packages."""
    kind = schema["type"]
    valid = {"string": lambda: isinstance(value, str),
             "integer": lambda: type(value) is int, "number": lambda: type(value) in (int, float) and math.isfinite(value),
             "boolean": lambda: type(value) is bool, "object": lambda: isinstance(value, dict),
             "array": lambda: isinstance(value, list)}[kind]()
    if not valid:
        raise ValueError(path + " must be " + kind)
    if "enum" in schema and value not in schema["enum"]:
        raise ValueError(path + " must be one of " + str(schema["enum"]))
    for low, high, size in (("minLength", "maxLength", len(value) if kind == "string" else None),
                            ("minItems", "maxItems", len(value) if kind == "array" else None),
                            ("minimum", "maximum", value if kind in ("number", "integer") else None)):
        if size is not None and ((low in schema and size < schema[low]) or (high in schema and size > schema[high])):
            raise ValueError(path + " is outside allowed bounds")
    if kind == "object":
        props = schema["properties"]
        if set(value) - set(props):
            raise ValueError(path + ": unknown fields " + str(sorted(set(value) - set(props))))
        if set(schema.get("required", [])) - set(value):
            raise ValueError(path + ": missing " + str(sorted(set(schema["required"]) - set(value))))
        for k, v in value.items():
            validate(v, props[k], path + "." + k)
    elif kind == "array":
        for v in value:
            validate(v, schema["items"], path + "[]")


def result(data, images=(), error=False):
    return {"content": [{"type": "text", "text": json.dumps(data, ensure_ascii=False)}] +
            [{"type": "image", "mimeType": "image/png", "data": base64.b64encode(p).decode("ascii")} for p in images],
            "isError": error}


def required(event, *fields):
    missing = set(fields) - set(event)
    if missing:
        raise ValueError(event["type"] + " requires " + ", ".join(sorted(missing)))


def key_code(key, sim=False):
    names = ["up", "down", "left", "right", "enter", "esc", "tab", "backspace"]
    if key.startswith("@"):
        word = key[1:].lower()
        if word not in (names[:5] + ["tab"] if sim else names):
            raise ValueError("unsupported named key " + key)
        return names.index(word) + 2, 0
    if len(key) != 1 or not 32 <= ord(key) <= 255 or key in ";":
        raise ValueError("key must be a printable single-byte character or @named key")
    return 1, ord(key)


def event_script(events, base):
    steps, last = [], base
    for i, e in enumerate(events):
        frame = base + e.get("frame", i * 4 + 1)
        typ = e["type"]
        if typ == "key":
            required(e, "key")
            key_code(e["key"], sim=True)
            spec = "key=" + e["key"]
        elif typ in ("tap", "down"):
            required(e, "x", "y")
            spec = "down=%d,%d" % (e["x"], e["y"])
            if typ == "tap":
                steps.append((frame + 2, "up"))
                last = max(last, frame + 2)
        elif typ == "up":
            spec = "up"
        elif typ == "tilt":
            required(e, "ax", "ay")
            xyz = (e["ax"], e["ay"], e.get("az", 1))
            if not any(xyz):
                raise ValueError("tilt vector cannot be zero")
            spec = "tilt=%g,%g,%g" % xyz
        elif typ == "swipe":
            required(e, "direction")
            spec = "swipe=" + e["direction"]
        elif typ == "rx":
            required(e, "count")
            spec = "rx=%d" % e["count"]
        else:
            raise ValueError("unsupported sim event " + typ)
        steps.append((frame, spec))
        last = max(last, frame)
    return ";".join("%d:%s" % s for s in sorted(steps, key=lambda s: s[0])), last + 4


def wire_events(events):
    """24-byte wire records from ls_input_ev.h, never native struct padding."""
    records = []
    def add(kind, e, key=0, ch=0, pressed=False):
        x, y = e.get("x", 0), e.get("y", 0)
        records.append(struct.pack("<7B5hHBI", 1, kind, 0, 0, 0, int(pressed), 1,
                                   e.get("col", x // 10), e.get("row", y // 17), x, y, 0, key, ch, 0))
    for e in events:
        typ = e["type"]
        if "frame" in e:
            raise ValueError("frame is sim-only")
        if typ == "key":
            required(e, "key")
            k, c = key_code(e["key"])
            add(0, e, k, c)
            add(1, e, k, c)
        elif typ in ("tap", "down", "up"):
            required(e, "x", "y")
            if typ != "up":
                add(3, e, pressed=True)
            if typ != "down":
                add(4, e)
        elif typ == "release_all":
            add(6, e)
        elif typ == "swipe":
            required(e, "x", "y", "direction")
            dx, dy = {"U": (0, -64), "D": (0, 64), "L": (-64, 0), "R": (64, 0)}[e["direction"]]
            end = dict(e, x=max(0, min(32767, e["x"] + dx)), y=max(0, min(32767, e["y"] + dy)))
            # Derive destination cells anew unless caller explicitly supplies them.
            end.pop("col", None)
            end.pop("row", None)
            add(3, e, pressed=True)
            add(2, end, pressed=True)
            add(4, end)
        else:
            raise ValueError("board cannot synthesize " + typ)
    return records


class LakeShark:
    def __init__(self, workspace):
        self.workspace = Path(workspace).resolve()
        self.replays = {}

    def app(self, name, exists=True):
        if not re.fullmatch(r"[a-z0-9_.-]{1,31}", name) or name in (".", ".."):
            raise ValueError("app must be a folder name of 1-31 lowercase letters, digits, _ . -")
        p = self.workspace / name
        if p.is_symlink() or p.resolve() != p:
            raise ValueError("app path escapes workspace")
        if exists and (not p.is_dir() or not any(p.glob("*.c"))):
            raise ValueError("app does not exist or has no .c files")
        return p

    def file(self, app, path):
        root = self.app(app)
        # Reject Windows drive/UNC/ADS paths even when running on POSIX.
        if not path or "\\" in path or ":" in path or PureWindowsPath(path).is_absolute() or PureWindowsPath(path).is_reserved() or Path(path).is_absolute():
            raise ValueError("path must be app-relative with forward slashes")
        if any(p in ("..", ".git") for p in Path(path).parts):
            raise ValueError("path traversal and .git are forbidden")
        p = root / path
        if p.resolve() == root or root not in p.resolve().parents:
            raise ValueError("path escapes app folder")
        return p

    def safe_tree(self, app):
        # Builds and simulator output also write inside the app; check links first.
        for p in app.rglob("*"):
            if app not in p.resolve().parents:
                raise ValueError("app contains a link outside its folder: " + str(p))
            if p.is_file() and p.stat().st_nlink > 1:
                raise ValueError("app contains a hard-linked file: " + str(p))

    def link(self):
        return lapboard.link()

    def screen(self, lk=None):
        lines = (lk or self.link()).rpc(["tui png mcp"], r"^tui: (?:png end|no |could not|not )", timeout=30)
        start = next((i for i, s in enumerate(lines) if s.startswith("tui: png begin mcp ")), None)
        end = next((i for i, s in enumerate(lines) if s.startswith("tui: png end mcp ")), None)
        if start is None or end is None or end <= start:
            raise ValueError("board PNG capture did not finish: " + "\n".join(lines[-5:]))
        m = re.search(r" (\d+) bytes crc ([0-9a-fA-F]{8})", lines[end])
        data = base64.b64decode("".join(s[1:] for s in lines[start + 1:end] if s.startswith("~")), validate=True)
        if not m or not data.startswith(b"\x89PNG\r\n\x1a\n") or len(data) != int(m[1]) or zlib.crc32(data) & 0xffffffff != int(m[2], 16):
            raise ValueError("board PNG size, signature or CRC mismatch")
        return result({"bytes": len(data), "source": lk.kind if lk else "board"}, [data])

    def simulate(self, app, frames, script, orientation):
        self.safe_tree(app)
        if not 1 <= frames <= 2000:
            raise ValueError("replay exceeds 2000 frames; start a new sim_run")
        log = []
        built = lsdk.sim_build(app, say=log.append)
        if not built.ok:
            return result(built.to_json(), error=True)
        work = lsdk.sim_workdir(app)
        env = lsdk.sim_env(app, script)
        env["LAPSIM"] = lsdk.source_id(app) or app.name
        # Inherited capture/feed settings must not produce thousands of files or binary stdout.
        for k in ("LSSIM_FRAMES", "LSSIM_FEED_OUT", "LAPSIM_SCRIPT"):
            env.pop(k, None)
        if script:
            env["LAPSIM_SCRIPT"] = script
        output = self.file(app.name, "build/sim/mcp-last.bmp")
        output.unlink(missing_ok=True)
        args = [str(lsdk.sim_copy(app)), "lap", "-A", "40", "-f", str(frames), "-o", str(output)]
        if orientation == "landscape":
            args.append("-l")
        run = subprocess.run(args, cwd=work, env=env, capture_output=True, text=True, errors="replace", timeout=120)
        log.extend([built.output, run.stdout, run.stderr])
        if run.returncode or not output.exists():
            return result({"ok": False, "log": "\n".join(log)[-64000:], "returncode": run.returncode}, error=True)
        try:
            from PIL import Image
        except ImportError:
            raise ValueError("PNG screenshots need Pillow: python -m pip install Pillow")
        png = io.BytesIO()
        with Image.open(output) as im:
            im.save(png, format="PNG")
        self.replays[app.name] = (frames, script, orientation)
        return result({"ok": True, "frames": frames, "script": script, "orientation": orientation,
                       "log": "\n".join(log)[-64000:], "replay": True}, [png.getvalue()])

    def call(self, name, a, progress=lambda *args: None):
        validate(a, SCHEMAS[name])
        if name == "sdk_docs":
            words = a["query"].lower().split()
            match = lambda s: all(w in s.lower() for w in words)
            sections = [s for s in apidoc.readme_sections() if match(s["title"] + " " + s["markdown"])]
            api = [e for e in apidoc.entries() if match(" ".join(e.values()))]
            return result({"sections": sections[:8], "api": api[:60], "total_sections": len(sections), "total_api": len(api)})
        if name == "sdk_api":
            matches = [e for e in apidoc.entries() if e["name"] == a["symbol"]]
            return result({"matches": matches}, error=not matches)
        if name == "new_app":
            dest = self.app(a["name"], exists=False)
            if dest.exists():
                raise ValueError("app folder already exists")
            return result({"app": a["name"], "path": str(lsdk.new_app(a["name"], a.get("template", "blank"), self.workspace))})
        if name == "list_apps":
            apps = []
            if self.workspace.exists():
                for p in sorted(self.workspace.iterdir()):
                    try:
                        root = self.app(p.name)
                        self.safe_tree(root)
                    except ValueError:
                        continue
                    apps.append({"app": p.name, "id": lsdk.source_id(root),
                                 "files": sorted(f.relative_to(root).as_posix() for f in root.rglob("*") if f.is_file() and "build" not in f.relative_to(root).parts)})
            return result({"workspace": str(self.workspace), "apps": apps})
        if name in ("read_file", "write_file"):
            p = self.file(a["app"], a["path"])
            if name == "read_file":
                if p.stat().st_size > MAX_FILE or p.stat().st_nlink > 1:
                    raise ValueError("file exceeds 1 MiB or is hard-linked")
                return result({"content": p.read_text(encoding="utf-8")})
            data = a["content"].encode("utf-8")
            if len(data) > MAX_FILE or (p.exists() and p.stat().st_nlink > 1):
                raise ValueError("file exceeds 1 MiB or is hard-linked")
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_bytes(data)
            return result({"path": a["path"], "bytes": len(data)})
        if name in ("build", "sim_run", "sim_input", "install"):
            app = self.app(a["app"])
            self.safe_tree(app)
            if name == "sim_run":
                return self.simulate(app, a.get("frames", 60), a.get("script", ""), a.get("orientation", "portrait"))
            if name == "sim_input":
                if app.name not in self.replays:
                    raise ValueError("call sim_run for this app first")
                frames, script, orientation = self.replays[app.name]
                extra, end = event_script(a["events"], frames)
                return self.simulate(app, end, ";".join(filter(None, [script, extra])), orientation)
            built = lsdk.build(app)
            if name == "build" or not built.ok:
                return result(built.to_json(), error=not built.ok)
            lk, updates = self.link(), []
            def update(sent, total, text):
                row = {"sent": sent, "total": total, "message": text}
                if not updates or updates[-1] != row:
                    updates.append(row)
                    progress(sent, total, text)
            try:
                filename, manifest, reply = lapboard.install(lk, Path(built.lap).read_bytes(), update)
            except Exception as e:
                return result({"ok": False, "via": lk.kind, "error": str(e), "progress": updates}, error=True)
            opened, open_error = [], None
            if a.get("open", True):
                try:
                    command = "desk open " + manifest["id"]
                    opened = lk.rpc([command], None, timeout=3)
                    if "desk: open " + manifest["id"] not in opened or any(s.startswith(("desk: busy", "desk: no app", "desk: unknown")) for s in opened):
                        raise lapboard.BoardError("\n".join(opened) or "desk did not acknowledge open")
                except Exception as e:
                    open_error = str(e)
            if open_error:
                return result({"ok": False, "installed": True, "via": lk.kind, "name": filename,
                               "reply": reply, "open_error": open_error, "progress": updates}, error=True)
            return result({"ok": True, "via": lk.kind, "name": filename, "reply": reply, "open": opened, "progress": updates})
        if name == "board_console":
            raw = a["line"]
            line = raw.strip()
            if not line or len(raw.encode("utf-8")) > 200 or any(ord(c) < 32 or ord(c) == 127 for c in raw) or any(c in raw for c in ";|&"):
                raise ValueError("one console line, max 200 UTF-8 bytes, no separators/control characters")
            words = re.findall(r"[a-z0-9_]+", line.lower())
            guarded = bool(set(words) & {"format", "formatting", "mkfs", "erase", "flash", "flashing", "ota", "update", "bootloader", "tx", "transmit", "send", "ptt", "arm", "say", "reset", "reboot", "factory"})
            guarded |= len(words) > 1 and (words[0] in ("sd", "app") and words[1] in ("rm", "remove", "delete") or words[0] in ("crumb", "crash") and words[1] == "clear")
            guarded |= len(words) > 1 and words[0] == "mesh" and words[1] in ("dm", "advert")
            guarded |= len(words) > 1 and words[0] == "cellreport" and words[1] in ("target", "test")
            if guarded and not a.get("confirm", False):
                return result({"error": "requires confirm=true", "guarded_commands": GUARDED}, error=True)
            lines = self.link().rpc([line], None, timeout=5)
            return result({"lines": lines, "text": "\n".join(lines), "guarded_commands": GUARDED})
        if name == "board_screen":
            return self.screen()
        if name == "board_input":
            records = wire_events(a["events"])  # validate every event before sending any
            lk = self.link()
            # Bridge console lines are capped at 200 chars: 3*48 + 8 = 152.
            for i in range(0, len(records), 3):
                lk.rpc(["term ev " + b"".join(records[i:i + 3]).hex()], None, timeout=0.3)
            time.sleep(0.08)
            return self.screen(lk)
        if name == "board_logs":
            lk = self.link()
            n, app = a.get("n", 100), a.get("app")
            if app and (not lsdk.ID_RE.fullmatch(app) or app in (".", "..")):
                raise ValueError("invalid app id")
            if lk.kind == "bridge":
                lines = lapboard._http(lk.url + "/board/tail?since=0&wait=0").get("lines", [])
            else:
                lines, deadline = [], time.monotonic() + 2
                lk.tail(lines.append, lambda: len(lines) >= 2000 or time.monotonic() >= deadline)
            if app:
                lines = [s for s in lines if "[app %s]" % app in s]
            return result({"lines": lines[-n:], "via": lk.kind, "historical": lk.kind == "bridge"})
        raise ValueError("unknown tool")


class RpcError(Exception):
    def __init__(self, code, message):
        self.code, self.message = code, message


class StdioServer:
    def __init__(self, sdk, output):
        self.sdk, self.output = sdk, output
        self.initialized = self.ready = False

    def send(self, message):
        self.output.write(json.dumps(message, ensure_ascii=False, allow_nan=False) + "\n")
        self.output.flush()

    def dispatch(self, method, p):
        if method == "initialize":
            if self.initialized:
                raise RpcError(-32600, "already initialized")
            if not isinstance(p.get("protocolVersion"), str) or not isinstance(p.get("capabilities"), dict) or not isinstance(p.get("clientInfo"), dict):
                raise RpcError(-32602, "initialize needs protocolVersion, capabilities and clientInfo")
            self.initialized = True
            version = p["protocolVersion"] if p["protocolVersion"] in VERSIONS else VERSIONS[-1]
            return {"protocolVersion": version, "serverInfo": {"name": "lakeshark", "version": "0.1.0"},
                    "capabilities": {"tools": {"listChanged": False}, "resources": {"subscribe": False, "listChanged": False}}}
        if method == "ping":
            return {}
        if not self.ready:
            raise RpcError(-32000, "initialize and send notifications/initialized first")
        if method in ("tools/list", "resources/list", "resources/templates/list"):
            if p.get("cursor"):
                raise RpcError(-32602, "unknown cursor")
            if method == "tools/list":
                return {"tools": TOOLS}
            if method == "resources/templates/list":
                return {"resourceTemplates": []}
            return {"resources": [{"uri": u, "name": f.name, "mimeType": "text/markdown" if f.suffix == ".md" else "text/x-c"} for u, f in RESOURCES.items()]}
        if method == "resources/read":
            uri = p.get("uri")
            if not isinstance(uri, str) or uri not in RESOURCES:
                raise RpcError(-32002, "resource not found")
            f = RESOURCES[uri]
            return {"contents": [{"uri": uri, "mimeType": "text/markdown" if f.suffix == ".md" else "text/x-c", "text": f.read_text(encoding="utf-8")}]}
        if method == "tools/call":
            name, a = p.get("name"), p.get("arguments", {})
            if not isinstance(name, str) or name not in SCHEMAS:
                raise RpcError(-32602, "unknown tool")
            token = p.get("_meta", {}).get("progressToken") if isinstance(p.get("_meta", {}), dict) else None
            def progress(sent, total, text):
                if type(token) in (str, int, float):
                    self.send({"jsonrpc": "2.0", "method": "notifications/progress", "params": {
                        "progressToken": token, "progress": sent, "total": total, "message": text}})
            try:
                # Third-party helpers must never contaminate the protocol stream.
                with contextlib.redirect_stdout(sys.stderr):
                    return self.sdk.call(name, a, progress)
            except Exception as e:
                return result({"error": str(e)}, error=True)
        raise RpcError(-32601, "method not found")

    def handle(self, message):
        ident = None
        try:
            if not isinstance(message, dict) or message.get("jsonrpc") != "2.0":
                raise RpcError(-32600, "invalid JSON-RPC request")
            if "method" not in message and "id" in message and ("result" in message or "error" in message):
                return
            ident = message.get("id")
            if "id" in message and (type(ident) not in (int, str) or ident is None):
                ident = None
                raise RpcError(-32600, "invalid request id")
            method = message.get("method")
            if not isinstance(method, str):
                raise RpcError(-32600, "method must be a string")
            # Notifications and client responses never get a response or execute tools.
            if "id" not in message:
                if method == "notifications/initialized" and self.initialized:
                    self.ready = True
                return
            p = message.get("params", {})
            if not isinstance(p, dict):
                raise RpcError(-32602, "params must be an object")
            value = self.dispatch(method, p)
            self.send({"jsonrpc": "2.0", "id": ident, "result": value})
        except RpcError as e:
            self.send({"jsonrpc": "2.0", "id": ident, "error": {"code": e.code, "message": e.message}})
        except Exception as e:
            print("MCP internal error: " + str(e), file=sys.stderr)
            self.send({"jsonrpc": "2.0", "id": ident, "error": {"code": -32603, "message": "internal error"}})

    def run(self, stream):
        for line in stream:
            try:
                message = json.loads(line, parse_constant=lambda s: (_ for _ in ()).throw(ValueError(s)))
            except (ValueError, UnicodeError):
                self.send({"jsonrpc": "2.0", "id": None, "error": {"code": -32700, "message": "parse error"}})
                continue
            self.handle(message)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--workspace", default=os.environ.get("LSDK_WORKSPACE") or str(SDK / "apps"))
    args = parser.parse_args(argv)
    for stream in (sys.stdin, sys.stdout):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8", errors="strict", newline="\n")
    try:
        StdioServer(LakeShark(args.workspace), sys.stdout).run(sys.stdin)
    except (BrokenPipeError, KeyboardInterrupt):
        pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
