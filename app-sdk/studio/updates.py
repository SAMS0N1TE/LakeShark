"""Desktop updater. No GUI dependencies; source runs can check but never install.

LSDK_UPDATER_TEST_URL is honored only outside a frozen build and only for a
loopback HTTP manifest. Installer URLs retain the production allowlist.
"""
import hashlib
from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from urllib.parse import urlparse
from urllib.request import HTTPRedirectHandler, Request, build_opener

LATEST = "https://terminalbay.com/desktop/latest.json"
RELEASES = "https://github.com/SAMS0N1TE/LakeShark/releases"
PREFIX = RELEASES + "/download/"
DAY = 86400
_VERSION = re.compile(r"(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-([0-9A-Za-z]+(?:[.-][0-9A-Za-z]+)*))?(?:\+[0-9A-Za-z.-]+)?\Z")


def version_key(version):
    m = _VERSION.fullmatch(version) if isinstance(version, str) else None
    if not m:
        raise ValueError("invalid version")
    # Natural numeric ordering also handles the owner's rc1, rc2, rc10 tags.
    pre = tuple((0, int(p)) if p.isdigit() else
                (1, tuple((0, int(t)) if t.isdigit() else (1, t)
                          for t in re.findall(r"\d+|[^\d]+", p)))
                for p in (m[4] or "").split("."))
    return tuple(map(int, m.group(1, 2, 3))) + (m[4] is None, pre)


def app_version():
    if getattr(sys, "frozen", False):
        return json.loads((Path(sys._MEIPASS) / "web/build.json").read_text("utf-8"))["version"]
    iss = Path(__file__).resolve().parents[2] / "host/installer/lakeshark.iss"
    return re.search(r'^#define AppVersion "([^"]+)"', iss.read_text("utf-8"), re.M)[1]


def validate_manifest(data):
    if not isinstance(data, dict):
        raise ValueError("invalid update manifest")
    version_key(data.get("version"))
    url = data.get("url", "")
    # No credentials, ports, queries, fragments, encoded paths or traversal.
    if not isinstance(url, str) or not re.fullmatch(
            re.escape(PREFIX) + r"[A-Za-z0-9_.+-]+/[A-Za-z0-9_.+-]+\.exe", url):
        raise ValueError("installer URL is not an allowed LakeShark release")
    if url[len(PREFIX):].split("/")[0] in (".", ".."):
        raise ValueError("installer URL contains traversal")
    if not re.fullmatch(r"[0-9a-fA-F]{64}", str(data.get("sha256", ""))):
        raise ValueError("invalid installer sha256")
    if type(data.get("size")) is not int or not 0 < data["size"] <= 2 * 1024**3:
        raise ValueError("invalid installer size")
    if data.get("channel") not in ("stable", "pre-release"):
        raise ValueError("invalid update channel")
    notes = data.get("notes", "")
    if not isinstance(notes, str) or len(notes) > 1000:
        raise ValueError("invalid update notes")
    return {k: data[k] for k in ("version", "channel", "url", "sha256", "size")} | {
        "notes": " ".join(notes.split()), "release_url": RELEASES + "/tag/" + url[len(PREFIX):].split("/")[0]}


class SecureRedirect(HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        u = urlparse(newurl)
        # GitHub releases redirect to their own asset CDN. Never downgrade TLS.
        if (u.scheme != "https" or u.hostname not in
                ("github.com", "release-assets.githubusercontent.com", "objects.githubusercontent.com")
                or u.username or u.password or u.port not in (None, 443)):
            raise ValueError("unsafe update redirect")
        return super().redirect_request(req, fp, code, msg, headers, newurl)


def open_url(url, timeout):
    return build_opener(SecureRedirect()).open(Request(url, headers={"User-Agent": "LakeShark-Updater"}), timeout=timeout)


def manifest_url():
    override = os.environ.get("LSDK_UPDATER_TEST_URL")
    if override and not getattr(sys, "frozen", False):
        u = urlparse(override)
        if u.scheme != "http" or u.hostname != "127.0.0.1" or u.username or u.password:
            raise ValueError("test manifest must use http://127.0.0.1")
        return override
    return LATEST


def fetch_manifest():
    with open_url(manifest_url(), 5) as response:
        raw = response.read(65537)
    if len(raw) > 65536:
        raise ValueError("update manifest too large")
    return validate_manifest(json.loads(raw))


def verify_file(path, meta):
    if path.stat().st_size != meta["size"]:
        raise ValueError("installer size does not match; update refused")
    with path.open("rb") as f:
        digest = hashlib.file_digest(f, "sha256").hexdigest()
    if digest.lower() != meta["sha256"].lower():
        raise ValueError("installer sha256 does not match; update refused")


def download_verified(meta, progress=lambda sent, total: None, opener=open_url):
    """Returns a private TEMP installer only after exact size and hash verification."""
    meta = validate_manifest(meta)
    folder = Path(tempfile.mkdtemp(prefix="LakeShark-update-"))
    path = folder / "LakeShark-Setup.exe"
    try:
        sent = 0
        with opener(meta["url"], 30) as response, path.open("xb") as output:
            while True:
                chunk = response.read(256 * 1024)
                if not chunk:
                    break
                sent += len(chunk)
                if sent > meta["size"]:
                    raise ValueError("installer exceeds expected size; update refused")
                output.write(chunk)
                progress(sent, meta["size"])
        verify_file(path, meta)
        return path
    except BaseException:
        shutil.rmtree(folder, ignore_errors=True)
        raise


class Updater:
    def __init__(self, settings_path):
        self.path = Path(settings_path).with_name("updater.json")
        self.lock = threading.RLock()
        self.version = app_version()
        self.available = None
        self.job = {"state": "idle", "sent": 0, "total": 0}
        try:
            cached = validate_manifest(self._load().get("latest"))
            if version_key(cached["version"]) > version_key(self.version):
                self.available = cached
        except ValueError:
            pass

    def _load(self):
        try:
            data = json.loads(self.path.read_text("utf-8"))
            return data if isinstance(data, dict) else {}
        except (OSError, ValueError):
            return {}

    def _save(self, data):
        self.path.parent.mkdir(parents=True, exist_ok=True)
        fd, tmp = tempfile.mkstemp(prefix="updater-", dir=self.path.parent)
        try:
            with os.fdopen(fd, "w", encoding="utf-8") as f:
                json.dump(data, f)
            os.replace(tmp, self.path)
        finally:
            if os.path.exists(tmp):
                os.unlink(tmp)

    @contextmanager
    def _config_guard(self):
        """Serialize timestamp/settings writes across separate Desktop processes."""
        self.path.parent.mkdir(parents=True, exist_ok=True)
        with self.path.with_suffix(".lock").open("a+b") as f:
            if os.name == "nt":
                import msvcrt
                f.seek(0, 2)
                if not f.tell():
                    f.write(b"\0")
                    f.flush()
                f.seek(0)
                msvcrt.locking(f.fileno(), msvcrt.LK_LOCK, 1)
            else:
                import fcntl
                fcntl.flock(f, fcntl.LOCK_EX)
            try:
                yield
            finally:
                if os.name == "nt":
                    f.seek(0)
                    msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)
                else:
                    fcntl.flock(f, fcntl.LOCK_UN)

    def status(self):
        with self.lock:
            return {"ok": True, "frozen": bool(getattr(sys, "frozen", False)), "version": self.version,
                    "automatic": self._load().get("automatic", True), "available": self.available,
                    "job": dict(self.job)}

    def settings(self, automatic):
        if type(automatic) is not bool:
            raise ValueError("automatic must be true or false")
        with self.lock, self._config_guard():
            data = self._load()
            data["automatic"] = automatic
            self._save(data)
        return self.status()

    def check(self, manual=False):
        with self.lock:
            with self._config_guard():
                data = self._load()
                last = data.get("last_check", 0)
                if not isinstance(last, (int, float)):
                    last = 0
                if not manual and (not data.get("automatic", True) or time.time() - last < DAY):
                    return self.status()
                # Record attempts too: offline launches must not repeatedly hit the network.
                data["last_check"] = time.time()
                self._save(data)
            # Bound the whole check to five seconds, including DNS, redirects and reads.
            result = []
            def fetch():
                try:
                    result.append(fetch_manifest())
                except Exception:
                    pass  # Offline and malformed feeds never interrupt work.
            worker = threading.Thread(target=fetch, daemon=True)
            worker.start()
            worker.join(5)
            if not worker.is_alive() and result:
                self.available = result[0] if version_key(result[0]["version"]) > version_key(self.version) else None
                with self._config_guard():
                    data = self._load()
                    data["latest"] = result[0]
                    self._save(data)
                return self.status() | {"checked": True}
            return self.status() | {"offline": True}

    def start(self, relaunch):
        if not getattr(sys, "frozen", False):
            raise ValueError("installer updates require the installed LakeShark exe")
        if relaunch not in ("studio", "desktop"):
            raise ValueError("invalid relaunch window")
        with self.lock:
            if self.job["state"] in ("downloading", "launching"):
                return self.status()
            if not self.available:
                raise ValueError("check for a newer version first")
            meta = dict(self.available)
            self.job = {"state": "downloading", "sent": 0, "total": meta["size"]}
            threading.Thread(target=self._install, args=(meta, relaunch), daemon=True).start()
            return self.status()

    def _install(self, meta, relaunch):
        path = None
        try:
            def progress(sent, total):
                with self.lock:
                    self.job.update(sent=sent, total=total)
            path = download_verified(meta, progress)
            # Defense in depth: verify again immediately before starting any process.
            verify_file(path, meta)
            with self.lock:
                self.job["state"] = "launching"
            subprocess.Popen([str(path), "/SILENT", "/SUPPRESSMSGBOXES", "/CLOSEAPPLICATIONS",
                              "/RESTARTAPPLICATIONS", "/CURRENTUSER", "/RELAUNCH=" + relaunch],
                             close_fds=True)
            os._exit(0)
        except Exception as e:
            if path:
                shutil.rmtree(path.parent, ignore_errors=True)
            with self.lock:
                self.job.update(state="error", message=str(e))
