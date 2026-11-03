"""Publishing to the community store, against a fake store API on 127.0.0.1.

    python app-sdk/tests/test_store_publish.py

The fake answers as the Worker does (STORE_SUBMIT_SPEC: /auth/device/start,
/auth/device/poll, /me, /submit, /submissions) and records what it was sent.
Checks, with no network and no GitHub:

  1. the token file: where it lives, who can read it, load, clear;
  2. app.json schema 2: a good one passes, each rule fails on its own, and a
     first-party schema-1 file only asks for the new answers;
  3. the bundle: the allowlist, the layout, every size cap, LICENSE text;
  4. filling in at a terminal (scripted answers) and failing without one;
  5. the device flow's waits (pending, slow down, done, expired);
  6. lsdk login, whoami, publish, submissions and logout as a program;
  7. Studio's /api/store/* doing the same, the token never in a reply.

Steps 6 and 7 build the app, so they need the RISC-V toolchain (skipped,
and said so, without it). Exit 0 when all pass.
"""
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import urllib.error
import urllib.request
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

HERE = Path(__file__).resolve().parent
SDK = HERE.parent
sys.path.insert(0, str(SDK))
sys.path.insert(0, str(SDK / "studio"))
import lap  # noqa: E402
import lsdk  # noqa: E402
import lsstore  # noqa: E402

TOKEN = "fake-store-token-for-tests-0123456789"
FAILS = []
PNG = b"\x89PNG\r\n\x1a\n" + b"\0" * 64
GIF = b"GIF89a" + b"\0" * 64


def check(what, ok, detail=""):
    print(("PASS " if ok else "FAIL ") + what + ("" if ok else "  " + str(detail)[-1500:]), flush=True)
    if not ok:
        FAILS.append(what)


# ------------------------------------------------------------------ the fake store

class Fake:
    polls = 0
    pending_polls = 1
    submits = []
    auth_seen = []


class FakeWorker(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def reply(self, code, obj):
        body = json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def authed(self):
        a = self.headers.get("Authorization", "")
        Fake.auth_seen.append(a)
        if a != "Bearer " + TOKEN:
            self.reply(401, {"error": "bad token"})
            return False
        return True

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def do_GET(self):
        if self.path == "/me":
            if self.authed():
                self.reply(200, {"login": "tester", "github_id": 42, "avatar": "", "account_age_days": 400,
                                 "owned_ids": ["demo"], "can_submit": True})
        elif self.path == "/submissions":
            if self.authed():
                self.reply(200, {"submissions": [{"number": 7, "id": "demo", "state": "waiting-for-review",
                                                  "checks": "4 passed", "last_review": ""}]})
        elif self.path == "/submissions/7":
            if self.authed():
                self.reply(200, {"number": 7, "id": "demo", "state": "needs-changes", "check_comment": "validate: ok"})
        else:
            self.reply(404, {"error": "no route"})

    def do_POST(self):
        data = self.body()
        if self.path == "/auth/device/start":
            Fake.polls = 0
            self.reply(200, {"device_code": "dev-code-1", "user_code": "WDJB-MJHT", "interval": 1, "expires_in": 60,
                             "verification_uri": "https://github.com/login/device"})
        elif self.path == "/auth/device/poll":
            j = json.loads(data or b"{}")
            if j.get("device_code") != "dev-code-1":
                return self.reply(400, {"error": "bad device code"})
            Fake.polls += 1
            if Fake.polls <= Fake.pending_polls:
                return self.reply(400, {"error": "authorization_pending"})
            self.reply(200, {"access_token": TOKEN})
        elif self.path == "/submit":
            if not self.authed():
                return
            if self.headers.get("Content-Type") != "application/zip":
                return self.reply(415, {"error": "zip only"})
            names = zipfile.ZipFile(io.BytesIO(data)).namelist()
            meta = json.loads(zipfile.ZipFile(io.BytesIO(data)).read("app.json"))
            Fake.submits.append({"names": names, "meta": meta, "size": len(data)})
            self.reply(200, {"pr": 7, "url": "https://github.com/SAMS0N1TE/lakeshark-apps/pull/7",
                             "status_url": "https://terminalbay.com/?m=lakeshark-apps#submit/mine"})
        else:
            self.reply(404, {"error": "no route"})


def start_fake():
    srv = ThreadingHTTPServer(("127.0.0.1", 0), FakeWorker)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, "http://127.0.0.1:%d" % srv.server_address[1]


# ------------------------------------------------------------------ helpers

def good_meta(app_id="demo"):
    return {"schema": 2, "id": app_id, "category": "games", "description": "A test app.", "license": "MIT",
            "source_url": "", "ai": {"use": "assisted", "tools": ["Claude"], "extent": "Suggested the layout."},
            "third_party": [], "references": ["RFC 1149"],
            "declarations": {k: True for k, _ in lsstore.DECLARATIONS}, "status": "wip"}


def make_app(root, name="demo", meta=None):
    app = lsdk.new_app(name, "blank", root, "Tester")
    if meta is not None:
        lsstore.write_meta(app, meta)
    return app


def toolchain():
    try:
        lap.compiler()
        return True
    except ValueError:
        return False


# ------------------------------------------------------------------ 1. token file

def test_token(tmp):
    old = dict(os.environ)
    try:
        os.environ.pop("LSDK_CONFIG_DIR", None)
        if os.name == "nt":
            os.environ["APPDATA"] = str(tmp / "appdata")
            check("token file under %APPDATA%\\lakeshark", lsstore.token_file() == tmp / "appdata" / "lakeshark" / "store.json",
                  lsstore.token_file())
        else:
            os.environ.pop("XDG_CONFIG_HOME", None)
            check("token file under ~/.config/lakeshark",
                  lsstore.token_file() == Path.home() / ".config" / "lakeshark" / "store.json", lsstore.token_file())
        os.environ["LSDK_CONFIG_DIR"] = str(tmp / "cfg")
        check("no token: not signed in", lsstore.load_login() is None)
        p = lsstore.save_login(TOKEN, "tester", "http://x")
        j = json.loads(p.read_text())
        check("token saved with the login", j["token"] == TOKEN and j["login"] == "tester")
        check("no temporary file left beside it", [f.name for f in p.parent.iterdir()] == ["store.json"],
              list(p.parent.iterdir()))
        if os.name != "nt":
            check("token file is 0600", (p.stat().st_mode & 0o777) == 0o600, oct(p.stat().st_mode))
        check("load gives it back", (lsstore.load_login() or {}).get("token") == TOKEN)
        p.write_text("{not json")
        check("a broken file reads as signed out", lsstore.load_login() is None)
        lsstore.save_login(TOKEN, "tester")
        check("clear removes it", lsstore.clear_login() and not p.exists() and lsstore.load_login() is None)
        check("clear twice is harmless", lsstore.clear_login() is False)
        os.environ.pop("LSDK_STORE_API", None)
        check("the API defaults to apps-api.terminalbay.com", lsstore.api_base() == "https://apps-api.terminalbay.com")
        os.environ["LSDK_STORE_API"] = "http://127.0.0.1:9/"
        check("LSDK_STORE_API moves it", lsstore.api_base() == "http://127.0.0.1:9")
    finally:
        os.environ.clear()
        os.environ.update(old)


# ------------------------------------------------------------------ 2. app.json

def test_schema():
    e, m = lsstore.check_meta(good_meta(), "demo")
    check("a good schema-2 app.json passes", not e and not m, (e, m))
    none_ai = good_meta()
    none_ai["ai"] = {"use": "none", "tools": [], "extent": ""}
    e, m = lsstore.check_meta(none_ai, "demo")
    check("ai.use none needs no tools or extent", not e and not m, (e, m))

    def bad(what, change, want_err=None, want_missing=None, sid="demo"):
        meta = good_meta()
        change(meta)
        e, m = lsstore.check_meta(meta, sid)
        ok = (want_err is None or any(want_err in x for x in e)) and (want_missing is None or want_missing in m)
        ok = ok and (e or m)
        check("schema 2 refuses " + what, ok, (e, m))

    bad("ai tools missing when AI helped", lambda m: m["ai"].update(tools=[]), want_missing="ai.tools")
    bad("ai extent missing when AI helped", lambda m: m["ai"].update(extent=" "), want_missing="ai.extent")
    bad("an unknown ai use", lambda m: m["ai"].update(use="some"), want_err="ai.use")
    bad("a license not on the list", lambda m: m.update(license="WTFPL"), want_err="not on the list")
    bad("a proprietary license", lambda m: m.update(license="proprietary"), want_err="not on the list")
    bad("a CC license for the app's code", lambda m: m.update(license="CC-BY-4.0"), want_err="assets and data")
    bad("no license", lambda m: m.update(license=""), want_missing="license")
    bad("an id that is not the source's", lambda m: None, want_err="LAP_APP", sid="other")
    bad("a bad category", lambda m: m.update(category="toys"), want_err="category")
    bad("an empty description", lambda m: m.update(description=""), want_missing="description")
    bad("a declaration left false", lambda m: m["declarations"].update(no_ai_cleanroom=False),
        want_missing="declarations.no_ai_cleanroom")
    bad("a declaration missing", lambda m: m["declarations"].pop("ethical_sources"),
        want_missing="declarations.ethical_sources")
    bad("GPL code in an MIT app",
        lambda m: m.update(third_party=[{"name": "lib", "license": "GPL-3.0-only", "url": "", "use": "code"}]),
        want_err="needs the app under")
    bad("LGPL-3 code in a GPL-2.0-or-later app",
        lambda m: m.update(license="GPL-2.0-or-later", third_party=[{"name": "lib", "license": "LGPL-3.0-or-later", "url": "", "use": "code"}]),
        want_err="needs the app under")
    bad("CC for third-party code",
        lambda m: m.update(third_party=[{"name": "art", "license": "CC-BY-4.0", "url": "", "use": "code"}]),
        want_err="never for code")
    bad("a third-party license not on the list",
        lambda m: m.update(third_party=[{"name": "x", "license": "unknown", "url": "", "use": "data"}]),
        want_err="not on the list")
    bad("a third-party use not on the list",
        lambda m: m.update(third_party=[{"name": "x", "license": "MIT", "url": "", "use": "inspiration"}]),
        want_err="use is one of")
    bad("an http source_url", lambda m: m.update(source_url="http://example.com"), want_err="source_url")
    bad("schema 1", lambda m: m.pop("schema"), want_missing="schema")
    ok = good_meta()
    ok.update(license="GPL-3.0-or-later", third_party=[{"name": "lib", "license": "GPL-2.0-or-later", "url": "", "use": "code"},
                                                      {"name": "art", "license": "CC-BY-SA-4.0", "url": "", "use": "asset"}])
    e, m = lsstore.check_meta(ok, "demo")
    check("GPL-2.0+ code in a GPL-3.0+ app with CC art passes", not e and not m, (e, m))
    first = json.loads((SDK / "apps" / "dice" / "app.json").read_text(encoding="utf-8"))
    e, m = lsstore.check_meta(dict(first, id="dice"), "dice")
    check("a first-party schema-1 app.json only lacks the new answers", not e and {"schema", "license", "ai.use"} <= set(m), (e, m))
    mit = lsstore.mit_license("Tester", "2026")
    check("the MIT text reads as MIT", lsstore.license_matches("MIT", mit) and lsstore.guess_license(mit) == "MIT")
    check("the MIT text does not read as Apache-2.0", not lsstore.license_matches("Apache-2.0", mit))
    check("an SPDX line names the license", lsstore.guess_license("SPDX-License-Identifier: BSD-3-Clause") == "BSD-3-Clause")


# ------------------------------------------------------------------ 3. the bundle

def test_bundle(tmp):
    app = make_app(tmp / "b", "demo", good_meta())
    (app / "README.md").write_text("# demo\n")
    (app / "build").mkdir(exist_ok=True)
    (app / "build" / "demo.lap").write_bytes(b"\x7fELF")
    (app / "src").mkdir()
    (app / "src" / "util.c").write_text("int util;\n")
    (app / "src" / "util.o").write_bytes(b"\0")
    (app / "deep").mkdir()
    (app / "deep" / "x.c").write_text("int x;\n")
    (app / "notes.bin").write_bytes(b"\0\1")
    (app / ".secret").write_text("x")
    (app / "media").mkdir()
    (app / "media" / "thumb.png").write_bytes(PNG)
    (app / "media" / "screen-1.png").write_bytes(PNG)
    (app / "media" / "screen-2.gif").write_bytes(GIF)
    (app / "media" / "clip.gif").write_bytes(GIF)
    (app / "media" / "other.png").write_bytes(PNG)
    data, left = lsstore.bundle(app, good_meta())
    names = sorted(zipfile.ZipFile(io.BytesIO(data)).namelist())
    want = sorted(["app.json", "LICENSE", "README.md", "main.c", "src/util.c", "media/thumb.png", "media/screen-1.png",
                   "media/screen-2.gif", "media/clip.gif"])
    check("the bundle holds exactly the store's layout", names == want, names)
    lefts = {r for r, _ in left}
    check("left out: binaries, other folders, unknown media, hidden files",
          {"notes.bin", "deep/x.c", "src/util.o", "media/other.png", ".secret"} <= lefts, left)
    check("build/, .vscode/ and .gitignore left out without a word",
          not any(r.startswith(("build", ".vscode", ".gitignore")) for r in lefts), left)
    check("no path in the zip climbs out or hides", not any(".." in n or n.startswith((".", "/")) for n in names))

    def refused(what, setup, needle):
        a = make_app(tmp / ("r-" + what.replace(" ", "_")), "demo", good_meta())
        setup(a)
        try:
            lsstore.bundle(a, good_meta())
            check("bundle refuses " + what, False, "accepted")
        except lsstore.StoreError as e:
            check("bundle refuses " + what, needle in str(e), e)

    def media(a):
        (a / "media").mkdir(exist_ok=True)
        return a / "media"
    refused("a screen over 300 KB", lambda a: (media(a) / "screen-1.png").write_bytes(PNG + b"\0" * 300 * 1024), "300 KB")
    refused("five screens", lambda a: [(media(a) / ("screen-%d.png" % i)).write_bytes(PNG) for i in range(5)], "4 at most")
    refused("a clip over 1.5 MB", lambda a: (media(a) / "clip.gif").write_bytes(GIF + b"\0" * 1600 * 1024), "1.5 MB")
    refused("a .png that is not a PNG", lambda a: (media(a) / "thumb.png").write_bytes(b"hello"), "not a real PNG")
    refused("more than 64 files", lambda a: [(a / ("f%02d.h" % i)).write_text("") for i in range(64)], "64 at most")
    refused("over 2 MB zipped", lambda a: (a / "data.txt").write_bytes(os.urandom(2 * 1024 * 1024 + 100)), "2 MB at most")
    refused("no LICENSE", lambda a: (a / "LICENSE").unlink(), "LICENSE is missing")
    refused("no .c file", lambda a: (a / "main.c").unlink(), "no .c source")
    refused("a LICENSE that is not the declared one", lambda a: (a / "LICENSE").write_text("Apache License\nVersion 2.0, January 2004\n"),
            "does not read like MIT")


# ------------------------------------------------------------------ 4. filling in

def test_fill(tmp):
    app = make_app(tmp / "f", "demo")
    m = json.loads((app / "app.json").read_text())
    check("lsdk new writes a schema-2 app.json with the answers empty",
          m.get("schema") == 2 and m.get("id") == "demo" and m["license"] == "" and m["ai"]["use"] == ""
          and not any(m["declarations"].values()), m)
    lic = (app / "LICENSE").read_text()
    check("lsdk new writes an MIT LICENSE with the author and the year",
          lic.startswith("MIT License") and "Copyright (c) 2026 Tester" in lic, lic[:80])
    try:
        lsstore.prepare(app, "demo", interactive=False)
        check("without a terminal, publish stops and lists the gaps", False, "accepted")
    except lsstore.StoreError as e:
        check("without a terminal, publish stops and lists the gaps",
              "license" in str(e) and "ai.use" in str(e) and "declarations.own_work_or_licensed" in str(e), e)
    answers = iter(["", "2", "Claude, Copilot", "Wrote the first draft of main.c.", "y", "y", "y", "y"])
    said = []
    meta = lsstore.prepare(app, "demo", interactive=True, ask=lambda q: next(answers), say=said.append)
    saved = json.loads((app / "app.json").read_text())
    check("at a terminal the answers fill app.json and are saved",
          saved == meta and saved["license"] == "MIT" and saved["ai"] == {"use": "assisted", "tools": ["Claude", "Copilot"],
                                                                        "extent": "Wrote the first draft of main.c."}
          and all(saved["declarations"].values()), saved)
    app2 = make_app(tmp / "f2", "demo")
    answers = iter(["1", "1", "n"])
    try:
        lsstore.prepare(app2, "demo", interactive=True, ask=lambda q: next(answers), say=lambda s: None)
        check("a declaration answered no stops the publish", False, "accepted")
    except lsstore.StoreError as e:
        check("a declaration answered no stops the publish", "all four" in str(e), e)


# ------------------------------------------------------------------ 5. device flow

class ScriptedClient:
    def __init__(self, answers):
        self.answers = list(answers)

    def device_start(self):
        return {"device_code": "d", "user_code": "AAAA-BBBB", "verification_uri": lsstore.DEVICE_PAGE, "interval": 1, "expires_in": 600}

    def device_poll(self, code):
        return self.answers.pop(0)


def test_device():
    waits, shown = [], []
    c = ScriptedClient([{"error": "authorization_pending"}, {"slow_down": True}, {"pending": True}, {"access_token": "t"}])
    tok = lsstore.device_login(c, lambda code, url: shown.append((code, url)), sleep=waits.append, clock=lambda: 0)
    check("device flow: code shown, then the token", tok == "t" and shown == [("AAAA-BBBB", lsstore.DEVICE_PAGE)], (tok, shown))
    check("device flow: slow_down waits 5 s longer", waits == [1, 1, 6, 6], waits)
    try:
        lsstore.device_login(ScriptedClient([{"status": "expired"}]), lambda *a: None, sleep=lambda s: None, clock=lambda: 0)
        check("device flow: an expired code stops it", False, "no error")
    except lsstore.StoreError as e:
        check("device flow: an expired code stops it", "ran out" in str(e), e)
    try:
        lsstore.device_login(ScriptedClient([{"error": "access_denied"}]), lambda *a: None, sleep=lambda s: None, clock=lambda: 0)
        check("device flow: a refusal stops it", False, "no error")
    except lsstore.StoreError as e:
        check("device flow: a refusal stops it", "turned down" in str(e), e)


# ------------------------------------------------------------------ 6. lsdk as a program

def lsdk_run(env, *args, cwd=None):
    r = subprocess.run([sys.executable, str(SDK / "lsdk.py")] + list(args), env=env, cwd=cwd, capture_output=True,
                       text=True, stdin=subprocess.DEVNULL, timeout=300)
    return r.returncode, r.stdout + r.stderr


def test_lsdk(tmp, base, built):
    env = dict(os.environ, LSDK_STORE_API=base, LSDK_CONFIG_DIR=str(tmp / "cfg-lsdk"))
    Fake.pending_polls = 1
    rc, out = lsdk_run(env, "login")
    check("lsdk login: shows the code and signs in", rc == 0 and "WDJB-MJHT" in out and "signed in as tester" in out, out)
    check("lsdk login: the token is in the config folder, not on the screen",
          TOKEN not in out and json.loads((tmp / "cfg-lsdk" / "store.json").read_text())["token"] == TOKEN, out)
    rc, out = lsdk_run(env, "whoami")
    check("lsdk whoami", rc == 0 and "tester" in out and "demo" in out and TOKEN not in out, out)
    app = make_app(tmp / "p", "demo")
    rc, out = lsdk_run(env, "publish", str(app))
    check("lsdk publish with no terminal and gaps: stops, lists them, sends nothing",
          rc == 1 and "ai.use" in out and "license" in out and not Fake.submits, out)
    lsstore.write_meta(app, good_meta())
    (app / "notes.bin").write_bytes(b"\0")
    if not built:
        print("SKIP lsdk publish round trip: no RISC-V toolchain (lap.py could not find riscv32-esp-elf-gcc)")
    else:
        rc, out = lsdk_run(env, "publish", str(app))
        sub = Fake.submits[-1] if Fake.submits else {}
        check("lsdk publish: builds, sends the zip, prints the status link",
              rc == 0 and "https://terminalbay.com/?m=lakeshark-apps#submit/mine" in out and "left out notes.bin" in out, out)
        check("lsdk publish: the store got the bundle and the answers",
              sorted(sub.get("names", [])) == ["LICENSE", "app.json", "main.c"] and sub["meta"]["ai"]["tools"] == ["Claude"], sub)
        check("lsdk publish: the token sent as Bearer, never printed", Fake.auth_seen[-1] == "Bearer " + TOKEN and TOKEN not in out)
        rc, out = lsdk_run(env, "publish", str(app), "--dry-run")
        check("lsdk publish --dry-run: a zip in build/, nothing sent",
              rc == 0 and (app / "build" / "demo-store.zip").exists() and len(Fake.submits) == 1, out)
    rc, out = lsdk_run(env, "submissions")
    check("lsdk submissions: plain status words", rc == 0 and "#7" in out and "waiting for review" in out, out)
    rc, out = lsdk_run(env, "submissions", "7")
    check("lsdk submissions 7: one, with the check comment", rc == 0 and "needs changes" in out and "validate: ok" in out, out)
    bad = dict(env, LSDK_STORE_API="http://127.0.0.1:9")
    rc, out = lsdk_run(bad, "submissions")
    check("no store there: one plain line, no trace", rc == 1 and "cannot reach the store" in out and "Traceback" not in out, out)
    rc, out = lsdk_run(env, "logout")
    check("lsdk logout: the token file is gone", rc == 0 and not (tmp / "cfg-lsdk" / "store.json").exists(), out)
    rc, out = lsdk_run(env, "submissions")
    check("signed out: publish and submissions say lsdk login", rc == 1 and "lsdk login" in out, out)


# ------------------------------------------------------------------ 7. Studio

def studio_call(port, name, body=None, studio_header=True):
    req = urllib.request.Request("http://127.0.0.1:%d/api/%s" % (port, name),
                                 data=None if body is None else json.dumps(body).encode(),
                                 method="GET" if body is None else "POST")
    if studio_header:
        req.add_header("X-Studio", "1")
    try:
        with urllib.request.urlopen(req, timeout=300) as r:
            raw = r.read().decode()
            return r.status, json.loads(raw), raw
    except urllib.error.HTTPError as e:
        raw = e.read().decode()
        return e.code, json.loads(raw or "{}"), raw


def test_studio(tmp, base, built):
    old = dict(os.environ)
    os.environ.update(LSDK_STORE_API=base, LSDK_CONFIG_DIR=str(tmp / "cfg-studio"),
                      LSDK_STUDIO_SETTINGS=str(tmp / "studio-settings.json"))
    try:
        import server
        ws = tmp / "ws"
        srv = server.serve(0, ws)
        port = srv.server_address[1]
        threading.Thread(target=srv.serve_forever, daemon=True).start()
        app = make_app(ws, "demo")
        q = "?p=" + urllib.request.quote(str(app))
        code, j, raw = studio_call(port, "store/me")
        check("Studio: signed out to start", code == 200 and j["signed_in"] is False and j["api"] == base, j)
        code, j, raw = studio_call(port, "store/login", {}, studio_header=False)
        check("Studio: a store call without X-Studio is refused", code == 403, (code, j))
        Fake.pending_polls = 1
        code, j, raw = studio_call(port, "store/login", {})
        check("Studio: sign-in gives the code to type, not the device code",
              code == 200 and j["user_code"] == "WDJB-MJHT" and "dev-code-1" not in raw and j["url"].endswith("/login/device"), j)
        code, j, raw = studio_call(port, "store/poll", {})
        check("Studio: waiting while GitHub has not been told", j.get("state") == "pending", j)
        code, j, raw = studio_call(port, "store/poll", {})
        check("Studio: signed in, the token kept by the server only",
              j.get("state") == "done" and j.get("login") == "tester" and TOKEN not in raw
              and json.loads((tmp / "cfg-studio" / "store.json").read_text())["token"] == TOKEN, raw)
        code, j, raw = studio_call(port, "store/me")
        check("Studio: who is signed in", j.get("signed_in") and j.get("login") == "tester" and TOKEN not in raw, raw)
        code, j, raw = studio_call(port, "store/form" + q)
        check("Studio: the form lists what is still to answer",
              code == 200 and "license" in j["missing"] and "ai.use" in j["missing"] and j["has_license"]
              and j["license_guess"] == "MIT" and len(j["lists"]["declarations"]) == 4, j)
        code, j, raw = studio_call(port, "store/publish" + q, {"license": "MIT", "ai": {"use": "assisted", "tools": [], "extent": ""}})
        check("Studio: publish with gaps answers what is missing and sends nothing",
              code == 200 and j["ok"] is False and "ai.tools" in j["missing"] and len(Fake.submits) <= 1, j)
        check("Studio: the answers given are kept in app.json", json.loads((app / "app.json").read_text())["license"] == "MIT")
        answers = {k: v for k, v in good_meta().items() if k not in ("schema", "id", "status")}
        answers["third_party"] = [{"name": "font", "license": "CC-BY-4.0", "url": "https://example.com", "use": "asset"},
                                  {"name": "", "license": "", "url": "", "use": ""}]
        if not built:
            print("SKIP Studio publish round trip: no RISC-V toolchain")
        else:
            n = len(Fake.submits)
            code, j, raw = studio_call(port, "store/publish" + q, answers)
            check("Studio: publish builds and sends, answers the status link",
                  code == 200 and j.get("ok") and j.get("pr") == 7 and j["link"].endswith("#submit/mine") and len(Fake.submits) == n + 1, j)
            saved = json.loads((app / "app.json").read_text())
            check("Studio: empty third-party rows dropped, the rest saved",
                  saved["third_party"] == [answers["third_party"][0]] and Fake.submits[-1]["meta"]["third_party"] == saved["third_party"], saved)
        code, j, raw = studio_call(port, "store/submissions")
        check("Studio: submissions in plain words with their link",
              code == 200 and j["submissions"][0]["words"] == "waiting for review" and j["submissions"][0]["link"].endswith("#submit/mine"), j)
        code, j, raw = studio_call(port, "store/logout", {})
        check("Studio: sign out removes the token", j.get("was") is True and not (tmp / "cfg-studio" / "store.json").exists(), j)
        code, j, raw = studio_call(port, "store/submissions")
        check("Studio: signed out, the store calls say so", code == 400 and "not signed in" in j.get("message", ""), j)
        srv.shutdown()
    finally:
        os.environ.clear()
        os.environ.update(old)


def test_first_party_still_builds(tmp, built):
    if not built:
        print("SKIP first-party schema-1 build: no RISC-V toolchain")
        return
    d = tmp / "dice"
    shutil.copytree(SDK / "apps" / "dice", d, ignore=shutil.ignore_patterns("build"))
    res = lsdk.build(d)
    check("a first-party app with a schema-1 app.json still builds", res.ok, res.problems or res.output[-800:])


def main():
    tmp = Path(tempfile.mkdtemp(prefix="lsdk-store-"))
    built = toolchain()
    srv, base = start_fake()
    try:
        test_token(tmp)
        test_schema()
        test_bundle(tmp)
        test_fill(tmp)
        test_device()
        test_lsdk(tmp, base, built)
        test_studio(tmp, base, built)
        test_first_party_still_builds(tmp, built)
    finally:
        srv.shutdown()
        shutil.rmtree(tmp, ignore_errors=True)
    print("%d failed" % len(FAILS) if FAILS else "all passed")
    return 1 if FAILS else 0


if __name__ == "__main__":
    sys.exit(main())
