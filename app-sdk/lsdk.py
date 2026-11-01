#!/usr/bin/env python3
"""lsdk: make, build, run and install LakeShark board apps.

    lsdk new NAME [--template blank|radio|dots|3d|service] [--dir DIR]
    lsdk build [APP]                 compile for the board; errors as file:line:col: message
    lsdk sim [APP] [--frames N] [-l] [--script S]
                                     run it in the simulator: live in this terminal,
                                     or N frames as pictures in APP/build/frames
    lsdk install [APP|FILE.lap] [--port COMx] [--via auto|bridge|serial] [--open]
                                     put it on the board: through the desktop bridge
                                     when it holds the port, else over USB serial
    lsdk logs [ID] [--all]           the board's console, the app's lines
    lsdk info [APP|FILE.lap|ID] [--board]
    lsdk rm ID [--data]
    lsdk studio [--port 8790] [--no-browser]
                                     LakeShark Studio in the browser, on 127.0.0.1
    lsdk snippets                    rewrite the VS Code snippets from the headers
    lsdk mcp [--workspace DIR]        MCP server over stdio (no network listener)

APP is an app folder (its .c files and app.json); the default is the folder
you are in. Python 3.9+, Espressif's RISC-V toolchain for build; the simulator
needs this repository's bench and a host compiler (see README, "Your first
app in 5 minutes"); install, logs and rm need pyserial.
"""
import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

SDK = Path(__file__).resolve().parent
REPO = SDK.parent
BENCH = REPO / "bench"
TEMPLATES = SDK / "templates"
sys.path.insert(0, str(SDK))
import lap  # noqa: E402

TEMPLATE_INFO = {
    "blank": ("A face, one live value, the board's keys and OPTIONS", "tools"),
    "radio": ("Spectrum and waterfall on any radio, with RADIO, TUNE and OPTIONS", "radio"),
    "dots": ("A picture in dots: a ball rolling with the motion sensor", "games"),
    "3d": ("The 3D engine: a lit scene you walk round with a finger", "games"),
    "service": ("A background service watching the decoded timeline", "radio"),
}
TEMPLATE_SUB = {"blank": "my first app", "radio": "spectrum on any radio", "dots": "a ball that rolls",
                "3d": "a lit 3d scene", "service": "watches the decodes"}      # LAP_APP .sub: 31 at most
ID_RE = re.compile(r"^[a-z0-9_.-]{1,31}$")


class LsdkError(Exception):
    pass


# ------------------------------------------------------------------ host tools

def host_env():
    """The environment for the simulator build: ninja, CMake and a host gcc on
    PATH (this machine keeps them in Espressif's, CMake's and CLion's folders).
    LSDK_HOST_PATH, if set, goes first."""
    env = dict(os.environ)
    add = [p for p in os.environ.get("LSDK_HOST_PATH", "").split(os.pathsep) if p]
    if os.name == "nt":
        want = [("ninja.exe", [r"C:\Espressif\tools\ninja\*"]),
                ("cmake.exe", [r"C:\Program Files\CMake\bin"]),
                ("gcc.exe", [r"C:\Program Files\JetBrains\CLion*\bin\mingw\bin", r"C:\mingw64\bin",
                             r"C:\msys64\ucrt64\bin"])]
        for exe, pats in want:
            if any(Path(d, exe).exists() for d in add):
                continue
            found = None
            for pat in pats:
                for d in sorted(glob.glob(pat), reverse=True):
                    if Path(d, exe).exists():
                        found = d
                        break
                if found:
                    break
            if found:
                add.append(found)
            elif not shutil.which(exe):
                pass                                  # reported when the build fails
    env["PATH"] = os.pathsep.join(add + [env.get("PATH", "")])
    return env


# -------------------------------------------------------------------- problems

DIAG = re.compile(r"^(?P<file>(?:[A-Za-z]:)?[^:\n]+?):(?P<line>\d+):(?:(?P<col>\d+):)? "
                  r"(?P<sev>fatal error|error|warning|note): (?P<msg>.*)$")
UNDEF = re.compile(r"undefined reference to [`'](?P<sym>[^'`]+)'")


def find_symbol(app, sym):
    """Where the app first calls sym: (file, line, col), for a link error."""
    files = sorted(Path(app).glob("*.c")) + sorted(Path(app).glob("*.h"))
    # gcc turns printf("x") into putchar and printf("x\n") into puts
    for name in [sym] + {"putchar": ["printf"], "puts": ["printf"], "fputs": ["fprintf"]}.get(sym, []):
        rx = re.compile(r"\b%s\s*\(" % re.escape(name))
        for f in files:
            for n, text in enumerate(f.read_text(encoding="utf-8", errors="replace").splitlines(), 1):
                m = rx.search(text)
                if m and not text.lstrip().startswith(("//", "/*", "*")):
                    return str(f), n, m.start() + 1
    return (str(files[0]) if files else str(app)), 1, 1


def parse_problems(text, app=None):
    """gcc/ld output -> [{file, line, col, severity, message}] (files absolute)."""
    out, seen = [], set()
    for raw in text.splitlines():
        line = raw.rstrip()
        m = DIAG.match(line)
        if m:
            sev = "error" if m.group("sev") == "fatal error" else m.group("sev")
            f = str(Path(m.group("file")).resolve()) if not m.group("file").startswith("<") else m.group("file")
            p = {"file": f, "line": int(m.group("line")), "col": int(m.group("col") or 1),
                 "severity": sev, "message": m.group("msg").strip()}
        else:
            u = UNDEF.search(line)
            if not u or not app:
                continue
            f, n, c = find_symbol(app, u.group("sym"))
            p = {"file": f, "line": n, "col": c, "severity": "error",
                 "message": "%s is not in this build (undefined reference)" % u.group("sym")}
        key = (p["file"], p["line"], p["col"], p["message"])
        if key not in seen:
            seen.add(key)
            out.append(p)
    return out


def format_problem(p, base=None):
    f = p["file"]
    try:
        f = os.path.relpath(f, base or os.getcwd())
        if f.startswith(".."):
            f = p["file"]
    except ValueError:
        pass
    return "%s:%d:%d: %s: %s" % (f.replace("\\", "/"), p["line"], p["col"], p["severity"], p["message"])


# --------------------------------------------------------------------- the app

def app_dir(path=None):
    d = Path(path or ".").resolve()
    if d.is_file():
        d = d.parent
    if not list(d.glob("*.c")):
        raise LsdkError("%s has no .c files: give the app's folder" % d)
    return d


def source_id(app):
    """The id the app declares in LAP_APP, read from its source."""
    for f in sorted(Path(app).glob("*.c")):
        m = re.search(r'LAP_APP\s*\([^;]*?\.id\s*=\s*"([^"]+)"', f.read_text(encoding="utf-8", errors="replace"), re.S)
        if m:
            return m.group(1)
    return None


def lap_path(app):
    return Path(app) / "build" / (Path(app).name + ".lap")


def new_app(name, template="blank", dest=None, author=None):
    """A working starter app in DEST/NAME from a template. Returns its folder."""
    if template not in TEMPLATE_INFO:
        raise LsdkError("no template '%s' (%s)" % (template, ", ".join(TEMPLATE_INFO)))
    app_id = name.lower()
    if not ID_RE.match(app_id) or app_id in (".", ".."):
        raise LsdkError("a name is 1-31 of a-z 0-9 _ . - (it becomes the app's id)")
    folder = Path(dest or ".").resolve() / app_id
    if folder.exists() and any(folder.iterdir()):
        raise LsdkError("%s already exists" % folder)
    title = re.sub(r"[_.-]+", " ", app_id).upper()[:15].strip() or app_id.upper()[:15]
    art = title[:10].center(10)
    author = author or os.environ.get("LSDK_AUTHOR") or _git_name() or "me"
    sub = TEMPLATE_SUB[template]
    category = TEMPLATE_INFO[template][1]
    words = {"@ID@": app_id, "@NAME@": title, "@SUB@": sub,
             "@AUTHOR@": author[:23].replace('"', "'"), "@ART@": art}
    folder.mkdir(parents=True, exist_ok=True)
    for src in sorted((TEMPLATES / template).rglob("*")):
        if src.is_dir():
            continue
        text = src.read_text(encoding="utf-8")
        for k, v in words.items():
            text = text.replace(k, v)
        dst = folder / src.relative_to(TEMPLATES / template)
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(text, encoding="utf-8", newline="\n")
    if not (folder / "app.json").exists():
        (folder / "app.json").write_text(json.dumps({"category": category, "status": "wip",
                                                     "description": TEMPLATE_INFO[template][0]}, indent=2) + "\n",
                                         encoding="utf-8", newline="\n")
    (folder / ".gitignore").write_text("build/\n", encoding="utf-8", newline="\n")
    vs = folder / ".vscode"
    vs.mkdir(exist_ok=True)
    (vs / "tasks.json").write_text(json.dumps(vscode_tasks(str(SDK / "lsdk.py").replace("\\", "/"), "${workspaceFolder}"),
                                              indent=2) + "\n", encoding="utf-8", newline="\n")
    shutil.copy(SDK / ".vscode" / "lakeshark.code-snippets", vs / "lakeshark.code-snippets")
    (vs / "c_cpp_properties.json").write_text(json.dumps({"version": 4, "configurations": [
        {"name": "LakeShark app", "includePath": ["${workspaceFolder}", str(SDK / "include").replace("\\", "/")],
         "defines": ["__riscv"], "cStandard": "gnu17"}]}, indent=2) + "\n", encoding="utf-8", newline="\n")
    return folder


def _git_name():
    try:
        return subprocess.run(["git", "config", "user.name"], capture_output=True, text=True, timeout=3).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def vscode_tasks(lsdk, cwd):
    matcher = {"owner": "lakeshark", "fileLocation": ["autoDetect", cwd],
               "pattern": {"regexp": r"^(.*?):(\d+):(\d+): (error|warning|note): (.*)$",
                           "file": 1, "line": 2, "column": 3, "severity": 4, "message": 5}}
    def task(label, args, group=None, matcher_on=True):
        t = {"label": label, "type": "process", "command": "python", "args": [lsdk] + args,
             "options": {"cwd": cwd}, "problemMatcher": [matcher] if matcher_on else []}
        if group:
            t["group"] = group
        return t
    return {"version": "2.0.0", "tasks": [
        task("lsdk: build", ["build"], {"kind": "build", "isDefault": True}),
        task("lsdk: sim", ["sim"]),
        task("lsdk: sim frames", ["sim", "--frames", "60"]),
        task("lsdk: install", ["install"]),
        task("lsdk: install and open", ["install", "--open"]),
        task("lsdk: logs", ["logs"], matcher_on=False),
        task("lsdk: studio", ["studio"], matcher_on=False),
    ]}


class BuildResult:
    def __init__(self, ok, problems, output, lap_file=None, manifest=None, size=0):
        self.ok, self.problems, self.output = ok, problems, output
        self.lap, self.manifest, self.size = lap_file, manifest, size

    def to_json(self):
        return {"ok": self.ok, "problems": self.problems, "output": self.output,
                "lap": str(self.lap) if self.lap else None, "manifest": self.manifest, "size": self.size}


def build(app, out=None):
    """Compile for the board with lap.py. Never raises for the app's own mistakes."""
    app = app_dir(app)
    out = Path(out) if out else lap_path(app)
    log = []
    try:
        path = lap.build(app, out, log=log)
    except lap.CompileError as e:
        return BuildResult(False, parse_problems(e.output, app) or
                           [{"file": str(next(iter(sorted(app.glob("*.c"))))), "line": 1, "col": 1, "severity": "error",
                             "message": "the compiler failed: " + e.output.strip().splitlines()[-1] if e.output.strip() else "the compiler failed"}],
                           e.output)
    except ValueError as e:
        msg = str(e)
        m = re.match(r"needs (\S+)", msg)
        if m:                                          # a call the board does not export
            f, n, c = find_symbol(app, m.group(1))
            p = {"file": f, "line": n, "col": c, "severity": "error",
                 "message": "%s is not part of the app API (lakeshark_app.h, lakeshark_ui.h, lap3d.h)" % m.group(1)}
        else:
            p = {"file": str(sorted(app.glob("*.c"))[0]), "line": 1, "col": 1, "severity": "error", "message": "lap: " + msg}
        return BuildResult(False, [p], "".join(log) + msg + "\n")
    data = Path(path).read_bytes()
    e = lap.Elf(data)
    return BuildResult(True, parse_problems("".join(log), app), "".join(log), Path(path), e.manifest, len(data))


# ------------------------------------------------------------------- simulator

def sim_build_dir():
    return Path(os.environ.get("LSDK_SIM_BUILD") or BENCH / "build-lsdk")


def lssim_exe():
    return sim_build_dir() / ("lssim.exe" if os.name == "nt" else "lssim")


def sim_build(app, say=print):
    """Compile the app into lssim (bench/build-lsdk). Returns a BuildResult."""
    app = app_dir(app)
    if not (BENCH / "CMakeLists.txt").exists():
        raise LsdkError("the simulator needs the LakeShark repository's bench/ next to app-sdk/")
    env = host_env()
    bdir = sim_build_dir()
    cache = bdir / "CMakeCache.txt"
    have = []
    if cache.exists():
        m = re.search(r"^LAPSIM_EXTRA_APPS:STRING=(.*)$", cache.read_text(errors="replace"), re.M)
        have = [d for d in (m.group(1).split(";") if m else []) if d]
    mine = app.as_posix()
    want = [d for d in have if Path(d).is_dir() and Path(d).name != app.name] + [mine]
    if not cache.exists() or want != have:
        say("configuring the simulator build (%s)" % bdir)
        r = subprocess.run(["cmake", "-G", "Ninja", "-S", str(BENCH), "-B", str(bdir), "-DLS_SANITIZE=OFF",
                            "-DLAPSIM_EXTRA_APPS=" + ";".join(want)], env=env, capture_output=True, text=True,
                           errors="replace")
        if r.returncode:
            raise LsdkError("cmake configure failed:\n" + (r.stdout + r.stderr)[-3000:])
    if not lssim_exe().exists():
        say("building the simulator once (a minute or two)")
    r = subprocess.run(["cmake", "--build", str(bdir), "--target", "lssim"], env=env, capture_output=True,
                       text=True, errors="replace")
    text = r.stdout + r.stderr
    probs = parse_problems(text, app)
    mine_only = [p for p in probs if p["severity"] == "error" or str(Path(p["file"])).startswith(str(app))]
    if r.returncode:
        if not any(p["severity"] == "error" for p in mine_only):
            mine_only.append({"file": str(sorted(app.glob("*.c"))[0]), "line": 1, "col": 1, "severity": "error",
                              "message": "the simulator build failed: " + (text.strip().splitlines() or ["?"])[-1]})
        return BuildResult(False, mine_only, text)
    return BuildResult(True, mine_only, text, lssim_exe())


def sim_env(app, script=None, no_radio=False):
    env = host_env()
    env["LAPSIM"] = Path(app).name
    if script:
        env["LAPSIM_SCRIPT"] = script
    if no_radio:                                       # a board with no radio, as a Nano without an SDR
        env["LAPSIM_RADIO"] = "none"
    return env


def sim_workdir(app):
    d = Path(app) / "build" / "sim"
    d.mkdir(parents=True, exist_ok=True)
    return d


def sim_copy(app):
    """A private copy of lssim to run, so a rebuild never meets a running one."""
    src = lssim_exe()
    dst = Path(app) / "build" / ("lssim-run.exe" if os.name == "nt" else "lssim-run")
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        shutil.copy2(src, dst)
    except OSError:
        pass                                           # still running: use the one there
    return dst


def sim_frames(app, frames=60, landscape=False, script=None, no_radio=False, text=None):
    """Run N frames and keep each as a picture. Returns the folder. With a
    list `text`, the last frame's cells are appended to it as text."""
    out = Path(app) / "build" / "frames"
    if out.exists():
        shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True)
    env = sim_env(app, script, no_radio)
    env["LSSIM_FRAMES"] = str(out)
    args = [str(sim_copy(app)), "lap", "-A", "40", "-f", str(frames), "-o", str(out / "last.bmp")] + \
        (["-l"] if landscape else []) + (["-d"] if text is not None else [])
    r = subprocess.run(args, env=env, cwd=sim_workdir(app), capture_output=True, text=True, errors="replace")
    if r.returncode:
        raise LsdkError("lssim failed:\n" + (r.stdout + r.stderr)[-2000:])
    if text is not None:
        text.append(r.stdout)
    try:                                                # PNGs when Pillow is here
        from PIL import Image
        for bmp in sorted(out.glob("*.bmp")):
            Image.open(bmp).save(bmp.with_suffix(".png"))
            bmp.unlink()
    except ImportError:
        pass
    return out


def sim_feed_process(app, landscape=False, no_radio=False):
    """lssim --feed for Studio's run panel: stdin/stdout pipes."""
    args = [str(sim_copy(app)), "lap", "--feed"] + (["-l"] if landscape else [])
    flags = subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0
    return subprocess.Popen(args, env=sim_env(app, no_radio=no_radio), cwd=sim_workdir(app), stdin=subprocess.PIPE,
                            stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, creationflags=flags)


def sim_terminal(app, landscape=True, no_radio=False):
    """Live in this terminal, through lsterm --sim (Ctrl-] quits)."""
    env = sim_env(app, no_radio=no_radio)
    env["LSSIM_EXE"] = str(sim_copy(app))
    lsterm = REPO / "host" / "lsterm" / "lsterm.py"
    return subprocess.call([sys.executable, str(lsterm), "--sim", "lap:" + Path(app).name], env=env,
                           cwd=sim_workdir(app))


# ----------------------------------------------------------------------- board

def board_link(args):
    import lapboard
    return lapboard.link(getattr(args, "port", None), getattr(args, "via", "auto"))


def progress_bar(sent, total, text):
    w = 30
    k = int(w * sent / total) if total else w
    sys.stdout.write("\r[%s%s] %6d / %d  %-24s" % ("=" * k, " " * (w - k), sent, total, text[:24]))
    sys.stdout.flush()


# -------------------------------------------------------------------- commands

def report(res, base=None):
    for p in res.problems:
        print(format_problem(p, base), file=sys.stderr if p["severity"] == "error" else sys.stdout)
    if not res.ok and not res.problems:
        sys.stderr.write(res.output)


def cmd_new(a):
    folder = new_app(a.name, a.template, a.dir, a.author)
    print("made %s from the %s template" % (folder, a.template))
    print("next:  cd %s" % os.path.relpath(folder))
    print("       lsdk build      lsdk sim      lsdk install")


def cmd_build(a):
    app = app_dir(a.app)
    res = build(app, a.output)
    report(res)
    if not res.ok:
        return 1
    m = res.manifest
    print("%s: %d bytes; %s %s (API %d.%d)" % (os.path.relpath(res.lap), res.size, m["id"], m["version"],
                                              m["api_major"], m["api_minor"]))
    return 0


def cmd_sim(a):
    app = app_dir(a.app)
    res = sim_build(app, say=lambda s: print("lsdk: " + s, flush=True))
    report(res)
    if not res.ok:
        return 1
    if a.frames:
        out = sim_frames(app, a.frames, a.landscape, a.script, a.no_radio)
        print("%d frames in %s" % (len(list(out.glob("*.*"))), out))
        return 0
    if not sys.stdout.isatty():
        out = sim_frames(app, 30, a.landscape, a.script, a.no_radio)
        print("not a terminal: 30 frames in %s (lsdk sim --frames N for more)" % out)
        return 0
    print("lsdk: the app live in this terminal; Ctrl-] quits", flush=True)
    time.sleep(0.6)
    return sim_terminal(app, no_radio=a.no_radio)


def resolve_lap(target):
    p = Path(target or ".")
    if p.is_file() and p.suffix == ".lap":
        return p
    app = app_dir(p)
    res = build(app)
    report(res)
    if not res.ok:
        raise LsdkError("build failed")
    return res.lap


def cmd_install(a):
    import lapboard
    lap_file = resolve_lap(a.app)
    data = Path(lap_file).read_bytes()
    lk = board_link(a)
    print("installing %s (%d bytes) through %s" % (lap_file.name, len(data), lk.describe()))
    name, manifest, reply = lapboard.install(lk, data, progress_bar)
    print("\n" + reply)
    if a.open:
        try:
            for line in lapboard.open_on_board(lk, manifest["id"]):
                print(line)
        except lapboard.BoardError as e:
            print("open: %s" % e)
    return 0


def cmd_logs(a):
    lk = board_link(a)
    want = a.id or (source_id(app_dir(".")) if list(Path(".").glob("*.c")) else None)
    tag = "[app %s]" % want if want else None
    print("the board's console through %s%s; Ctrl-C stops" % (lk.describe(), (", lines of " + want) if want and not a.all else ""),
          flush=True)

    def on_line(line):
        if a.all or not tag or tag in line or line.startswith("APP ") or (want and want in line):
            print(line, flush=True)
    try:
        lk.tail(on_line, lambda: False)
    except KeyboardInterrupt:
        pass
    return 0


def cmd_info(a):
    import lapboard
    target = a.target or "."
    if a.board:
        app_id = target if not Path(target).exists() else source_id(app_dir(target))
        for row in lapboard.info(board_link(a), app_id):
            print(json.dumps(row, indent=2))
        return 0
    p = Path(target)
    if p.is_dir():
        p = lap_path(app_dir(p))
        if not p.exists():
            raise LsdkError("not built yet: lsdk build")
    print(json.dumps(lap.Elf(p.read_bytes()).manifest, indent=2))
    return 0


def cmd_rm(a):
    import lapboard
    print(lapboard.remove(board_link(a), a.id, a.data))
    return 0


def cmd_list(a):
    import lapboard
    for row in lapboard.list_apps(board_link(a)):
        print("%-16s %-16s %-8s %7s B  signed=%s" % (row.get("id"), row.get("name"), row.get("version"),
                                                  row.get("size"), row.get("signed")))
    return 0


def cmd_studio(a):
    sys.path.insert(0, str(SDK / "studio"))
    import server
    return server.main(port=a.port, browser=not a.no_browser, workspace=a.workspace)


def cmd_snippets(a):
    import apidoc
    out = SDK / ".vscode" / "lakeshark.code-snippets"
    out.write_text(json.dumps(apidoc.vscode_snippets(), indent=1) + "\n", encoding="utf-8", newline="\n")
    print("wrote", out)
    return 0


def cmd_mcp(a):
    # Load by filename: the local mcp/ folder must not shadow the official SDK.
    import importlib.util
    spec = importlib.util.spec_from_file_location("lakeshark_mcp", SDK / "mcp" / "server.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module.main(["--workspace", a.workspace] if a.workspace else [])


def main(argv=None):
    p = argparse.ArgumentParser(prog="lsdk", description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    n = sub.add_parser("new", help="a starter app from a template")
    n.add_argument("name")
    n.add_argument("--template", "-t", default="blank", choices=list(TEMPLATE_INFO))
    n.add_argument("--dir", default=None, help="where to make it (default: here)")
    n.add_argument("--author", default=None)
    b = sub.add_parser("build", help="compile for the board")
    b.add_argument("app", nargs="?")
    b.add_argument("-o", "--output")
    s = sub.add_parser("sim", help="run in the simulator")
    s.add_argument("app", nargs="?")
    s.add_argument("--frames", type=int, default=0)
    s.add_argument("-l", "--landscape", action="store_true")
    s.add_argument("--script", default=None, help="LAPSIM_SCRIPT, e.g. '20:tilt=0.4,0;40:swipe=L;50:page=TEST'")
    s.add_argument("--no-radio", action="store_true", help="a board with no radio, as a Nano without an SDR")
    for name, helptext in (("install", "put it on the board"), ("logs", "the board's console"),
                           ("info", "what a .lap or the board says"), ("rm", "remove from the board"),
                           ("list", "the apps on the board")):
        c = sub.add_parser(name, help=helptext)
        c.add_argument("--port", default=None)
        c.add_argument("--via", default="auto", choices=["auto", "bridge", "serial"])
        if name == "install":
            c.add_argument("app", nargs="?")
            c.add_argument("--open", action="store_true", help="open it in a desk window after")
        elif name == "logs":
            c.add_argument("id", nargs="?")
            c.add_argument("--all", action="store_true")
        elif name == "info":
            c.add_argument("target", nargs="?")
            c.add_argument("--board", action="store_true")
        elif name == "rm":
            c.add_argument("id")
            c.add_argument("--data", action="store_true", help="its data folder too")
    st = sub.add_parser("studio", help="LakeShark Studio in the browser")
    st.add_argument("--port", type=int, default=8790)
    st.add_argument("--no-browser", action="store_true")
    st.add_argument("--workspace", default=None)
    sub.add_parser("snippets", help="rewrite the VS Code snippets")
    mc = sub.add_parser("mcp", help="LakeShark MCP server over stdio")
    mc.add_argument("--workspace", default=None, help="app folders (default: app-sdk/apps or LSDK_WORKSPACE)")
    a = p.parse_args(argv)
    fn = globals()["cmd_" + a.cmd]
    try:
        return fn(a) or 0
    except LsdkError as e:
        print("lsdk: %s" % e, file=sys.stderr)
        return 1
    except Exception as e:                       # board and serial errors: one line, not a trace
        if type(e).__name__ in ("BoardError", "SerialException", "URLError"):
            print("lsdk: %s" % e, file=sys.stderr)
            return 1
        raise


if __name__ == "__main__":
    sys.exit(main())
