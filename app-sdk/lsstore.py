"""The community store from lsdk and Studio: sign in, check, bundle and send.

Stdlib only. The store API is a Cloudflare Worker (apps-api.terminalbay.com,
or LSDK_STORE_API); it opens a pull request on the catalog repository for
each submission, so nobody has to use GitHub itself.

Sign-in is GitHub's device flow through the Worker. The token is kept in the
user's config folder (%APPDATA%\\lakeshark\\store.json on Windows, else
~/.config/lakeshark/store.json; LSDK_CONFIG_DIR moves it), readable only by
the user where the OS allows, and is never printed or sent to a web page.

The checks here mirror the catalog's rules (app.json schema 2, the license
list, the bundle allowlist) so a mistake shows up before anything is sent.
"""
import io
import json
import os
import re
import stat
import sys
import time
import urllib.error
import urllib.request
import zipfile
from pathlib import Path

DEFAULT_API = "https://apps-api.terminalbay.com"
STATUS_LINK = "https://terminalbay.com/?m=lakeshark-apps#submit/mine"
DEVICE_PAGE = "https://github.com/login/device"

CATEGORIES = ["games", "tools", "radio", "maps", "utilities"]
AI_USES = ["none", "assisted", "substantial", "generated"]
AI_TEXT = {"none": "no AI at all", "assisted": "AI helped (suggestions, fixes, review)",
           "substantial": "AI wrote large parts", "generated": "AI wrote most or all of it"}
LICENSES = ["MIT", "BSD-2-Clause", "BSD-3-Clause", "ISC", "Apache-2.0", "Zlib", "Unlicense", "CC0-1.0", "0BSD",
            "MPL-2.0", "GPL-2.0-or-later", "GPL-3.0-only", "GPL-3.0-or-later", "LGPL-2.1-or-later",
            "LGPL-3.0-or-later", "CC-BY-4.0", "CC-BY-SA-4.0"]
ASSET_ONLY = {"CC-BY-4.0", "CC-BY-SA-4.0"}
THIRD_USES = ["code", "data", "asset", "reference"]
DECLARATIONS = [
    ("own_work_or_licensed", "I wrote this, or have the right to publish every part of it."),
    ("no_ai_cleanroom", "No AI (or anyone) was used to re-create someone else's code or design to get round its license."),
    ("licenses_respected", "Every dependency's license allows this use and is credited in the source."),
    ("ethical_sources", "Nothing leaked, stolen, confidential, scraped against terms or proprietary is in it."),
]
# an app under the first license may take code under these (same or later version family)
COPYLEFT_OK = {
    "GPL-2.0-or-later": {"GPL-2.0-or-later", "GPL-3.0-only", "GPL-3.0-or-later"},
    "GPL-3.0-only": {"GPL-3.0-only", "GPL-3.0-or-later"},
    "GPL-3.0-or-later": {"GPL-3.0-only", "GPL-3.0-or-later"},
    "LGPL-2.1-or-later": {"LGPL-2.1-or-later", "LGPL-3.0-or-later", "GPL-2.0-or-later", "GPL-3.0-only", "GPL-3.0-or-later"},
    "LGPL-3.0-or-later": {"LGPL-3.0-or-later", "GPL-3.0-only", "GPL-3.0-or-later"},
}
# words a LICENSE file under each license carries
LICENSE_WORDS = {
    "MIT": ["permission is hereby granted, free of charge"],
    "BSD-2-Clause": ["redistribution and use in source and binary forms"],
    "BSD-3-Clause": ["redistribution and use in source and binary forms", "neither the name"],
    "ISC": ["permission to use, copy, modify, and/or distribute this software for any purpose"],
    "0BSD": ["permission to use, copy, modify, and/or distribute this software for any purpose"],
    "Apache-2.0": ["apache license", "version 2.0"],
    "Zlib": ["provided 'as-is'", "altered source versions must be plainly marked"],
    "Unlicense": ["this is free and unencumbered software released into the public domain"],
    "CC0-1.0": ["cc0 1.0"],
    "MPL-2.0": ["mozilla public license", "2.0"],
    "GPL-2.0-or-later": ["gnu general public license", "version 2"],
    "GPL-3.0-only": ["gnu general public license", "version 3"],
    "GPL-3.0-or-later": ["gnu general public license", "version 3"],
    "LGPL-2.1-or-later": ["lesser general public license", "version 2.1"],
    "LGPL-3.0-or-later": ["lesser general public license", "version 3"],
    "CC-BY-4.0": ["attribution 4.0"],
    "CC-BY-SA-4.0": ["attribution-sharealike 4.0"],
}
STATE_WORDS = {"checking": "checking", "needs-changes": "needs changes", "waiting-for-review": "waiting for review",
               "published": "published", "rejected": "rejected"}

MAX_BUNDLE = 2 * 1024 * 1024
MAX_FILES = 64
MAX_SCREENS = 4
MAX_SCREEN = 300 * 1024
MAX_CLIP = 1536 * 1024
ALLOWED_EXT = {".c", ".h", ".json", ".md", ".txt", ".png", ".gif"}
QUIET_SKIP = {"build", ".vscode", ".git", ".gitignore", "__pycache__", ".studio-trash", ".DS_Store"}
ID_RE = re.compile(r"^[a-z0-9_.-]{1,31}$")


class StoreError(ValueError):
    pass


# ------------------------------------------------------------------ the token

def config_dir():
    if os.environ.get("LSDK_CONFIG_DIR"):
        return Path(os.environ["LSDK_CONFIG_DIR"])
    if os.name == "nt" and os.environ.get("APPDATA"):
        return Path(os.environ["APPDATA"]) / "lakeshark"
    return Path(os.environ.get("XDG_CONFIG_HOME") or Path.home() / ".config") / "lakeshark"


def token_file():
    return config_dir() / "store.json"


def load_login():
    """{"token", "login", "api"} or None."""
    try:
        j = json.loads(token_file().read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None
    return j if isinstance(j, dict) and j.get("token") else None


def save_login(token, login=None, api=None):
    """Written to a new file that only this user can read (0600 on POSIX; on
    Windows the profile folder's ACL already keeps others out), then moved
    over the old one."""
    d = config_dir()
    d.mkdir(parents=True, exist_ok=True)
    if os.name != "nt":
        try:
            os.chmod(d, 0o700)
        except OSError:
            pass
    tmp = d / ("store.json.%d.tmp" % os.getpid())
    fd = os.open(str(tmp), os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
    with os.fdopen(fd, "w", encoding="utf-8") as f:
        json.dump({"token": token, "login": login or "", "api": api or api_base(), "saved": int(time.time())}, f)
    if os.name != "nt":
        os.chmod(tmp, stat.S_IRUSR | stat.S_IWUSR)
    os.replace(tmp, token_file())
    return token_file()


def clear_login():
    try:
        token_file().unlink()
        return True
    except FileNotFoundError:
        return False


def api_base():
    return (os.environ.get("LSDK_STORE_API") or DEFAULT_API).rstrip("/")


# ------------------------------------------------------------------ the Worker

class Client:
    """The store API. Errors come back as StoreError with the Worker's words."""

    def __init__(self, base=None, token=None, timeout=60):
        self.base = (base or api_base()).rstrip("/")
        self.token = token
        self.timeout = timeout

    @classmethod
    def signed_in(cls):
        j = load_login()
        if not j:
            raise StoreError("not signed in: lsdk login (or Studio's Publish panel)")
        return cls(token=j["token"])

    def call(self, method, path, body=None, ctype="application/json", auth=True):
        data = body if isinstance(body, (bytes, type(None))) else json.dumps(body).encode()
        req = urllib.request.Request(self.base + path, data=data, method=method)
        req.add_header("Accept", "application/json")
        req.add_header("User-Agent", "lsdk-store/1")
        if data is not None:
            req.add_header("Content-Type", ctype)
        if auth:
            if not self.token:
                raise StoreError("not signed in: lsdk login")
            req.add_header("Authorization", "Bearer " + self.token)
        try:
            with urllib.request.urlopen(req, timeout=self.timeout) as r:
                raw = r.read()
        except urllib.error.HTTPError as e:
            raw = e.read() or b""
            try:
                j = json.loads(raw.decode("utf-8", "replace"))
            except ValueError:
                j = {}
            if isinstance(j, dict) and e.code in (400, 428) and path.startswith("/auth/device/poll"):
                return j                                  # pending / slow_down / expired come back as errors too
            msg = (j.get("message") or j.get("error") or "") if isinstance(j, dict) else ""
            if e.code == 401:
                raise StoreError("the store did not accept the sign-in: lsdk login again" + (" (%s)" % msg if msg else ""))
            problems = j.get("problems") or j.get("errors") if isinstance(j, dict) else None
            if problems:
                msg = (msg + ":\n  " if msg else "") + "\n  ".join(str(p) for p in problems)
            raise StoreError("the store said %d: %s" % (e.code, msg or e.reason))
        except (urllib.error.URLError, OSError) as e:
            raise StoreError("cannot reach the store at %s (%s)" % (self.base, getattr(e, "reason", e)))
        try:
            return json.loads(raw.decode("utf-8") or "{}")
        except ValueError:
            raise StoreError("the store answered something that is not JSON")

    def device_start(self):
        return self.call("POST", "/auth/device/start", {}, auth=False)

    def device_poll(self, device_code):
        return self.call("POST", "/auth/device/poll", {"device_code": device_code}, auth=False)

    def me(self):
        return self.call("GET", "/me")

    def submit(self, zip_bytes):
        return self.call("POST", "/submit", zip_bytes, ctype="application/zip")

    def submissions(self):
        j = self.call("GET", "/submissions")
        return j.get("submissions", j) if isinstance(j, dict) else j

    def submission(self, pr):
        return self.call("GET", "/submissions/%d" % int(pr))


def poll_state(j):
    """One device-poll answer -> ('done', token) | ('pending'|'slow_down'|'expired'|'denied', None)."""
    if not isinstance(j, dict):
        return "pending", None
    if j.get("access_token"):
        return "done", j["access_token"]
    words = {str(j.get(k, "")) for k in ("status", "state", "error")} | {k for k in j if j.get(k) is True}
    if words & {"slow_down"}:
        return "slow_down", None
    if words & {"expired", "expired_token"}:
        return "expired", None
    if words & {"access_denied", "denied"}:
        return "denied", None
    return "pending", None


def device_login(client, show, sleep=time.sleep, clock=time.monotonic):
    """GitHub's device flow through the Worker. show(user_code, url) tells the
    person what to type where. Returns the token; never prints it."""
    s = client.device_start()
    code, url = s.get("user_code"), s.get("verification_uri") or DEVICE_PAGE
    if not code or not s.get("device_code"):
        raise StoreError("the store did not give a sign-in code")
    show(code, url)
    interval = max(1, int(s.get("interval") or 5))
    end = clock() + int(s.get("expires_in") or 900)
    while clock() < end:
        sleep(interval)
        state, token = poll_state(client.device_poll(s["device_code"]))
        if state == "done":
            return token
        if state == "slow_down":
            interval += 5
        elif state == "expired":
            raise StoreError("the code ran out before it was entered: try again")
        elif state == "denied":
            raise StoreError("sign-in was turned down on GitHub")
    raise StoreError("the code ran out before it was entered: try again")


# ------------------------------------------------------------------ app.json

def read_meta(app):
    f = Path(app) / "app.json"
    if not f.exists():
        return {}
    try:
        j = json.loads(f.read_text(encoding="utf-8"))
    except ValueError as e:
        raise StoreError("app.json is not valid JSON: %s" % e)
    if not isinstance(j, dict):
        raise StoreError("app.json must be an object")
    return j


def write_meta(app, meta):
    f = Path(app) / "app.json"
    f.write_text(json.dumps(meta, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")


def schema2(meta, app_id=None):
    """meta with every schema-2 key present (nothing already there changes)."""
    m = {"schema": 2}
    m.update((k, v) for k, v in meta.items() if k != "schema")
    if app_id and not m.get("id"):
        m["id"] = app_id
    m.setdefault("category", "")
    m.setdefault("description", "")
    m.setdefault("license", "")
    m.setdefault("source_url", "")
    ai = dict(m.get("ai") or {})
    ai.setdefault("use", "")
    ai.setdefault("tools", [])
    ai.setdefault("extent", "")
    m["ai"] = ai
    m.setdefault("third_party", [])
    m.setdefault("references", [])
    d = dict(m.get("declarations") or {})
    for k, _ in DECLARATIONS:
        d.setdefault(k, False)
    m["declarations"] = d
    return m


def new_meta(app_id, category, description):
    """A new app's app.json: schema 2 with the AI, license and declarations
    left for the author to answer when publishing."""
    return {"schema": 2, "id": app_id, "category": category, "description": description, "license": "",
            "source_url": "", "ai": {"use": "", "tools": [], "extent": ""}, "third_party": [], "references": [],
            "declarations": {k: False for k, _ in DECLARATIONS}, "status": "wip"}


def check_meta(meta, source_id=None):
    """(errors, missing): missing are the fields a person can still fill in
    (lsdk asks for them at a terminal, Studio shows them in the form);
    errors are everything else that is wrong."""
    errors, missing = [], []
    if meta.get("schema") != 2:
        missing.append("schema")
    app_id = meta.get("id")
    if not app_id:
        missing.append("id")
    elif not isinstance(app_id, str) or not ID_RE.match(app_id):
        errors.append("id: 1-31 of a-z 0-9 _ . -")
    elif source_id and app_id != source_id:
        errors.append("id: app.json says '%s' but LAP_APP in the source says '%s'" % (app_id, source_id))
    if source_id is None:
        errors.append("no LAP_APP(.id = \"...\") found in the .c files")
    cat = meta.get("category")
    if not cat:
        missing.append("category")
    elif cat not in CATEGORIES:
        errors.append("category: one of %s" % ", ".join(CATEGORIES))
    desc = meta.get("description")
    if not isinstance(desc, str) or not desc.strip():
        missing.append("description")
    lic = meta.get("license")
    if not lic:
        missing.append("license")
    elif lic not in LICENSES:
        errors.append("license: '%s' is not on the list (%s)" % (lic, ", ".join(LICENSES)))
    elif lic in ASSET_ONLY:
        errors.append("license: %s is for assets and data, not for an app's code" % lic)
    url = meta.get("source_url", "")
    if url and (not isinstance(url, str) or not url.startswith("https://")):
        errors.append("source_url: an https:// address, or empty")
    ai = meta.get("ai")
    if not isinstance(ai, dict):
        missing.append("ai.use")
        ai = {}
    use = ai.get("use")
    if not use:
        missing.append("ai.use")
    elif use not in AI_USES:
        errors.append("ai.use: one of %s" % ", ".join(AI_USES))
    elif use != "none":
        tools = ai.get("tools")
        if not isinstance(tools, list) or not [t for t in tools if isinstance(t, str) and t.strip()]:
            missing.append("ai.tools")
        if not isinstance(ai.get("extent"), str) or not ai.get("extent", "").strip():
            missing.append("ai.extent")
    third = meta.get("third_party", [])
    if not isinstance(third, list):
        errors.append("third_party: a list")
        third = []
    for i, t in enumerate(third):
        where = "third_party[%d]" % i
        if not isinstance(t, dict):
            errors.append(where + ": an object")
            continue
        if not str(t.get("name", "")).strip():
            errors.append(where + ": name")
        tl, tu = t.get("license"), t.get("use")
        if tl not in LICENSES:
            errors.append("%s (%s): license '%s' is not on the list" % (where, t.get("name", ""), tl or ""))
        if tu not in THIRD_USES:
            errors.append("%s (%s): use is one of %s" % (where, t.get("name", ""), ", ".join(THIRD_USES)))
        if tl in ASSET_ONLY and tu == "code":
            errors.append("%s (%s): %s is never for code" % (where, t.get("name", ""), tl))
        if tu == "code" and tl in COPYLEFT_OK and lic in LICENSES and lic not in COPYLEFT_OK[tl]:
            errors.append("%s (%s): code under %s needs the app under %s" %
                          (where, t.get("name", ""), tl, " or ".join(sorted(COPYLEFT_OK[tl]))))
    refs = meta.get("references", [])
    if not isinstance(refs, list) or not all(isinstance(r, str) for r in refs):
        errors.append("references: a list of strings")
    decl = meta.get("declarations")
    decl = decl if isinstance(decl, dict) else {}
    for k, _ in DECLARATIONS:
        if decl.get(k) is not True:
            missing.append("declarations." + k)
    if "status" in meta and meta["status"] not in ("wip", "", None):
        errors.append("status: \"wip\" or left out")
    return errors, missing


def guess_license(text):
    """The SPDX id a LICENSE text reads like, or None."""
    t = " ".join((text or "").lower().split())
    m = re.search(r"spdx-license-identifier:\s*([a-z0-9.+-]+)", t)
    if m:
        for lic in LICENSES:
            if lic.lower() == m.group(1):
                return lic
    order = ["LGPL-3.0-or-later", "LGPL-2.1-or-later", "GPL-3.0-or-later", "GPL-2.0-or-later", "Apache-2.0", "MPL-2.0",
             "CC-BY-SA-4.0", "CC-BY-4.0", "CC0-1.0", "Unlicense", "Zlib", "BSD-3-Clause", "BSD-2-Clause", "MIT", "ISC"]
    for lic in order:
        if all(w in t for w in LICENSE_WORDS[lic]):
            if lic.startswith("GPL") and "lesser general public license" in t:
                continue
            return lic
    return None


def license_matches(lic, text):
    t = " ".join((text or "").lower().split())
    if re.search(r"spdx-license-identifier:\s*" + re.escape(lic.lower()) + r"\b", t):
        return True
    words = LICENSE_WORDS.get(lic)
    if not words or not all(w in t for w in words):
        return False
    if lic.startswith("GPL") and "lesser general public license" in t:
        return False
    return True


def mit_license(author, year=None):
    year = year or time.strftime("%Y")
    return ("MIT License\n\nCopyright (c) %s %s\n\n" % (year, author) +
            "Permission is hereby granted, free of charge, to any person obtaining a copy\n"
            "of this software and associated documentation files (the \"Software\"), to deal\n"
            "in the Software without restriction, including without limitation the rights\n"
            "to use, copy, modify, merge, publish, distribute, sublicense, and/or sell\n"
            "copies of the Software, and to permit persons to whom the Software is\n"
            "furnished to do so, subject to the following conditions:\n\n"
            "The above copyright notice and this permission notice shall be included in all\n"
            "copies or substantial portions of the Software.\n\n"
            "THE SOFTWARE IS PROVIDED \"AS IS\", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR\n"
            "IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,\n"
            "FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE\n"
            "AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER\n"
            "LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,\n"
            "OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE\n"
            "SOFTWARE.\n")


# ------------------------------------------------------------------ the bundle

def _media_kind(data):
    return "png" if data[:8] == b"\x89PNG\r\n\x1a\n" else "gif" if data[:6] in (b"GIF87a", b"GIF89a") else None


def bundle_files(app):
    """(files, left_out, errors): files are (name in the zip, path) in the
    catalog's layout; left_out are (path, why) for what the store does not
    take; errors stop the publish."""
    app = Path(app)
    files, left, errors = [], [], []
    screens = 0
    for root, dirs, names in os.walk(app, followlinks=False):
        rdir = Path(root)
        rel_dir = rdir.relative_to(app)
        for d in sorted(dirs):
            p = rdir / d
            if d in QUIET_SKIP or d.startswith("."):
                if d not in QUIET_SKIP:
                    left.append(((rel_dir / d).as_posix() + "/", "hidden"))
                dirs.remove(d)
            elif p.is_symlink():
                left.append(((rel_dir / d).as_posix() + "/", "a link"))
                dirs.remove(d)
        dirs.sort()
        for n in sorted(names):
            p = rdir / n
            rel = (rel_dir / n).as_posix()
            if n in QUIET_SKIP:
                continue
            if n.startswith("."):
                left.append((rel, "hidden"))
                continue
            if p.is_symlink():
                left.append((rel, "a link"))
                continue
            ext = p.suffix.lower()
            if n != "LICENSE" and ext not in ALLOWED_EXT:
                left.append((rel, "not a file type the store takes (.c .h .json .md .txt .png .gif, LICENSE)"))
                continue
            top = rel_dir == Path(".")
            if top and (ext in (".c", ".h", ".json", ".md", ".txt") or n == "LICENSE"):
                files.append((rel, p))
            elif rel_dir.as_posix() == "src" and ext in (".c", ".h"):
                files.append((rel, p))
            elif rel_dir.as_posix() == "media" and ext in (".png", ".gif"):
                size = p.stat().st_size
                with open(p, "rb") as f:
                    kind = _media_kind(f.read(8))
                if kind != ext[1:]:
                    errors.append("%s: not a real %s" % (rel, ext[1:].upper()))
                elif n == "thumb.png":
                    files.append((rel, p))
                elif re.fullmatch(r"screen-[A-Za-z0-9_-]+\.(png|gif)", n):
                    screens += 1
                    if size > MAX_SCREEN:
                        errors.append("%s: %d KB, a screen may be 300 KB at most" % (rel, size // 1024))
                    files.append((rel, p))
                elif n == "clip.gif":
                    if size > MAX_CLIP:
                        errors.append("%s: %d KB, the clip may be 1.5 MB at most" % (rel, size // 1024))
                    files.append((rel, p))
                else:
                    left.append((rel, "media/ holds thumb.png, screen-*.png|gif and clip.gif"))
            else:
                left.append((rel, "not where the store looks (top level, src/ or media/)"))
    names = {a for a, _ in files}
    if "app.json" not in names:
        errors.append("app.json is missing")
    if "LICENSE" not in names:
        errors.append("LICENSE is missing (a file named LICENSE with the license text)")
    if not any(a.endswith(".c") for a in names):
        errors.append("no .c source")
    if screens > MAX_SCREENS:
        errors.append("%d screens: 4 at most" % screens)
    if len(files) > MAX_FILES:
        errors.append("%d files: %d at most" % (len(files), MAX_FILES))
    return files, left, errors


def make_zip(files):
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
        for name, path in files:
            info = zipfile.ZipInfo(name, (2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, Path(path).read_bytes())
    return buf.getvalue()


def bundle(app, meta=None):
    """(zip bytes, left_out). Raises StoreError listing what is wrong."""
    files, left, errors = bundle_files(app)
    if meta is not None and "LICENSE" in {a for a, _ in files} and meta.get("license") in LICENSES:
        text = (Path(app) / "LICENSE").read_text(encoding="utf-8", errors="replace")
        if not license_matches(meta["license"], text):
            g = guess_license(text)
            errors.append("LICENSE does not read like %s%s" % (meta["license"], " (it reads like %s)" % g if g else ""))
    if errors:
        raise StoreError("the bundle is not ready:\n  " + "\n  ".join(errors))
    data = make_zip(files)
    if len(data) > MAX_BUNDLE:
        raise StoreError("the bundle is %d KB zipped: 2 MB at most" % (len(data) // 1024))
    return data, left


# ------------------------------------------------------------------ filling in

def _ask_choice(ask, say, label, choices, default=None, texts=None):
    say("%s:" % label)
    for i, c in enumerate(choices, 1):
        say("  %d  %s%s" % (i, c, ("  " + texts[c]) if texts and c in texts else ""))
    while True:
        a = ask("%s [%s]: " % (label, default or "number")).strip()
        if not a and default:
            return default
        if a.isdigit() and 1 <= int(a) <= len(choices):
            return choices[int(a) - 1]
        if a in choices:
            return a
        say("  a number from 1 to %d" % len(choices))


def _ask_text(ask, say, label):
    while True:
        a = ask(label + ": ").strip()
        if a:
            return a
        say("  this one is needed")


def _ask_yes(ask, label):
    return ask(label + " [y/N]: ").strip().lower() in ("y", "yes")


def fill(meta, missing, ask, say, license_guess=None):
    """Ask for each missing field at a terminal. Returns the new meta, or
    raises StoreError when a declaration is answered no."""
    m = schema2(meta)
    if "category" in missing:
        m["category"] = _ask_choice(ask, say, "Category", CATEGORIES)
    if "description" in missing:
        m["description"] = _ask_text(ask, say, "Description (one to three plain sentences)")
    if "license" in missing:
        m["license"] = _ask_choice(ask, say, "License (the LICENSE file must match)",
                                   [x for x in LICENSES if x not in ASSET_ONLY], license_guess)
    if "ai.use" in missing:
        m["ai"]["use"] = _ask_choice(ask, say, "AI use", AI_USES, texts=AI_TEXT)
    if m["ai"]["use"] not in ("none", "") and not [t for t in m["ai"].get("tools") or [] if str(t).strip()]:
        m["ai"]["tools"] = [t.strip() for t in _ask_text(ask, say, "AI tools used (comma separated)").split(",") if t.strip()]
    if m["ai"]["use"] not in ("none", "") and not str(m["ai"].get("extent", "")).strip():
        m["ai"]["extent"] = _ask_text(ask, say, "What the AI did, in plain words")
    want = [k for k, _ in DECLARATIONS if "declarations." + k in missing]
    if want:
        say("The store's rules (all four must be true):")
        for k, text in DECLARATIONS:
            if k in want:
                if not _ask_yes(ask, "  " + text):
                    raise StoreError("the store takes an app only when all four declarations are true")
                m["declarations"][k] = True
    return m


# ------------------------------------------------------------------ publish

def prepare(app, source_id, interactive=False, ask=input, say=print):
    """app.json checked (and filled in at a terminal, then saved). Returns meta."""
    meta = read_meta(app)
    if not meta.get("id") and source_id:
        meta["id"] = source_id
    errors, missing = check_meta(meta, source_id)
    if missing and interactive and not errors:
        lic = Path(app) / "LICENSE"
        guess = guess_license(lic.read_text(encoding="utf-8", errors="replace")) if lic.exists() else None
        say("app.json needs a few answers before the store takes it (they are saved there):")
        meta = fill(meta, missing, ask, say, guess)
        write_meta(app, meta)
        errors, missing = check_meta(meta, source_id)
    if errors or missing:
        lines = errors + (["missing in app.json: " + ", ".join(missing)] if missing else [])
        raise StoreError("app.json is not ready for the store:\n  " + "\n  ".join(lines))
    return meta


def link_for(result):
    pr = result.get("pr") if isinstance(result, dict) else None
    return STATUS_LINK if pr is not None else (result.get("status_url") or result.get("url") or "")


def describe(sub):
    """One submission as a line of plain words."""
    state = STATE_WORDS.get(sub.get("state"), sub.get("state") or "?")
    checks = sub.get("checks") or sub.get("check_summary") or ""
    if isinstance(checks, dict):
        checks = ", ".join("%s %s" % (v, k) for k, v in checks.items())
    line = "#%-5s %-16s %s" % (sub.get("number", sub.get("pr", "?")), sub.get("id", "?"), state)
    if checks:
        line += "   checks: %s" % checks
    return line


def is_tty():
    try:
        return sys.stdin.isatty() and sys.stdout.isatty()
    except (AttributeError, ValueError):
        return False
