"""Headless updater checks. All HTTP fixtures bind 127.0.0.1; no installer runs."""
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
from urllib.error import HTTPError
from urllib.request import Request, urlopen

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "app-sdk/studio"))
import updates
import server
from make_latest_json import make_latest

PAYLOAD = b"test-only installer bytes; never executed"


def manifest(version="2.20.1", **kw):
    return {"version": version, "channel": "stable", "url": updates.PREFIX + "v" + version + "/LakeShark-Setup.exe",
            "sha256": hashlib.sha256(PAYLOAD).hexdigest(), "size": len(PAYLOAD), "notes": "An update. One more sentence."} | kw


class Fixture(BaseHTTPRequestHandler):
    data = manifest()
    hits = 0

    def log_message(self, *a):
        pass

    def do_GET(self):
        type(self).hits += 1
        raw = PAYLOAD if self.path == "/installer.exe" else json.dumps(type(self).data).encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(raw)))
        self.end_headers()
        self.wfile.write(raw)


class UpdaterTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.http = ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
        cls.url = "http://127.0.0.1:%d" % cls.http.server_port
        threading.Thread(target=cls.http.serve_forever, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.http.shutdown()
        cls.http.server_close()

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        env = patch.dict(os.environ, {"LSDK_UPDATER_TEST_URL": self.url + "/latest.json"})
        env.start()
        self.addCleanup(env.stop)
        self.up = updates.Updater(self.root / "studio.json")
        self.up.version = "2.20.0-rc1"
        Fixture.data = manifest()
        Fixture.hits = 0

    def test_version_order(self):
        versions = ["2.20.0-alpha", "2.20.0-beta", "2.20.0-rc1", "2.20.0-rc2", "2.20.0-rc10", "2.20.0", "2.20.1", "2.21.0", "3.0.0"]
        self.assertEqual(sorted(reversed(versions), key=updates.version_key), versions)
        self.assertEqual(updates.version_key("2.20.0+build1"), updates.version_key("2.20.0+build2"))
        for value in ("", "2.20", "v2.20.0", "2.20.0-", None, "2.20.0<script>"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                updates.version_key(value)

    def test_no_update_same_or_older(self):
        for version in ("2.20.0-rc1", "2.19.9", "2.20.0-beta"):
            Fixture.data = manifest(version)
            result = self.up.check(manual=True)
            self.assertTrue(result["checked"])
            self.assertIsNone(result["available"])

    def test_update_available_and_once_daily_persisted(self):
        result = self.up.check()
        self.assertEqual(result["available"]["version"], "2.20.1")
        self.up.check()
        restarted = updates.Updater(self.root / "studio.json")
        self.assertEqual(restarted.status()["available"]["version"], "2.20.1")
        restarted.check()
        self.assertEqual(Fixture.hits, 1)
        self.up.check(manual=True)
        self.assertEqual(Fixture.hits, 2)
        with patch.object(updates.time, "time", return_value=time.time() + updates.DAY + 1):
            self.up.check()
        self.assertEqual(Fixture.hits, 3)

    def test_automatic_setting_off(self):
        self.up.settings(False)
        self.assertFalse(updates.Updater(self.root / "studio.json").status()["automatic"])
        self.up.check()
        self.assertEqual(Fixture.hits, 0)
        self.assertEqual(self.up.check(manual=True)["available"]["version"], "2.20.1")

    def test_separate_processes_share_daily_limit(self):
        code = "import sys; sys.path.insert(0, 'app-sdk/studio'); import updates; updates.Updater(sys.argv[1]).check()"
        children = [subprocess.Popen([sys.executable, "-c", code, str(self.root / "studio.json")], cwd=REPO)
                    for _ in range(2)]
        for child in children:
            self.assertEqual(child.wait(timeout=10), 0)
        self.assertEqual(Fixture.hits, 1)

    def test_offline_silent_and_rate_limited(self):
        with patch.object(updates, "open_url", side_effect=OSError("offline")) as opener:
            result = self.up.check()
            self.assertTrue(result["ok"])
            self.assertTrue(result["offline"])
            self.assertIsNone(result["available"])
            self.assertNotIn("message", result)
            self.up.check()
            opener.assert_called_once()

    def test_local_fake_offline(self):
        dead = ThreadingHTTPServer(("127.0.0.1", 0), Fixture)
        port = dead.server_port
        dead.server_close()
        with patch.dict(os.environ, {"LSDK_UPDATER_TEST_URL": "http://127.0.0.1:%d/latest.json" % port}):
            self.assertTrue(self.up.check(manual=True)["offline"])

    def test_five_second_total_timeout(self):
        event = threading.Event()
        def slow():
            event.wait(8)
            return manifest()
        with patch.object(updates, "fetch_manifest", side_effect=slow):
            before = time.monotonic()
            result = self.up.check(manual=True)
            elapsed = time.monotonic() - before
            event.set()
        self.assertTrue(result["offline"])
        self.assertLess(elapsed, 5.8)
        self.assertIsNone(self.up.available)

    def test_manifest_allowlist_and_fields(self):
        for url in ("http://github.com/SAMS0N1TE/LakeShark/releases/download/v1/a.exe",
                    updates.PREFIX + "v1/a.exe?x=1", updates.PREFIX + "v1/../a.exe",
                    updates.PREFIX + "../a.exe",
                    updates.PREFIX.replace("LakeShark", "Other") + "v1/a.exe",
                    "https://github.com.evil.test/SAMS0N1TE/LakeShark/releases/download/v1/a.exe",
                    "http://127.0.0.1:1234/installer.exe"):
            with self.subTest(url=url), self.assertRaises(ValueError):
                updates.validate_manifest(manifest(url=url))
        for kw in ({"size": True}, {"size": 0}, {"sha256": "bad"}, {"notes": []}, {"version": "bad"}):
            with self.subTest(kw=kw), self.assertRaises(ValueError):
                updates.validate_manifest(manifest(**kw))

    def test_redirects_allow_github_cdn_only(self):
        handler = updates.SecureRedirect()
        req = Request(manifest()["url"])
        for url in ("http://release-assets.githubusercontent.com/asset", "https://evil.test/asset", "file:///tmp/x"):
            with self.assertRaises(ValueError):
                handler.redirect_request(req, None, 302, "Found", {}, url)
        redirect = handler.redirect_request(req, None, 302, "Found", {}, "https://release-assets.githubusercontent.com/asset")
        self.assertEqual(redirect.host, "release-assets.githubusercontent.com")

    def local_installer(self, url, timeout):
        # Test transport remaps a VALID production URL; runtime has no bypass.
        self.assertTrue(url.startswith(updates.PREFIX))
        return urlopen(self.url + "/installer.exe", timeout=timeout)

    def test_download_verify_only(self):
        self.up.check(manual=True)
        progress = []
        with patch.object(updates.subprocess, "Popen") as run:
            path = updates.download_verified(self.up.available, lambda n, total: progress.append((n, total)), self.local_installer)
            self.addCleanup(shutil.rmtree, path.parent)
            self.assertEqual(path.read_bytes(), PAYLOAD)
            self.assertEqual(progress[-1], (len(PAYLOAD), len(PAYLOAD)))
            run.assert_not_called()

    def test_bad_hash_refused_before_launch(self):
        Fixture.data = manifest(sha256="0" * 64)
        self.up.check(manual=True)
        original = updates.download_verified
        with patch.object(updates, "download_verified", side_effect=lambda m, p: original(m, p, self.local_installer)), \
                patch.object(updates.subprocess, "Popen") as run, patch.object(updates.os, "_exit") as exit_process:
            self.up._install(self.up.available, "studio")
            self.assertEqual(self.up.job["state"], "error")
            self.assertIn("sha256", self.up.job["message"])
            run.assert_not_called()
            exit_process.assert_not_called()

    def test_size_mismatch_refused(self):
        for size in (len(PAYLOAD) - 1, len(PAYLOAD) + 1):
            with self.assertRaisesRegex(ValueError, "size"):
                updates.download_verified(manifest(size=size), opener=self.local_installer)

    def test_launch_boundary_uses_verified_file_and_relaunch_flags(self):
        original = updates.download_verified
        files = []
        def download(meta, progress):
            path = original(meta, progress, self.local_installer)
            files.append(path)
            return path
        with patch.object(updates, "download_verified", side_effect=download), \
                patch.object(updates.subprocess, "Popen") as run, patch.object(updates.os, "_exit") as exit_process:
            self.up._install(manifest(), "studio")
            args = run.call_args.args[0]
            self.assertEqual(args[1:], ["/SILENT", "/SUPPRESSMSGBOXES", "/CLOSEAPPLICATIONS",
                                        "/RESTARTAPPLICATIONS", "/CURRENTUSER", "/RELAUNCH=studio"])
            updates.verify_file(Path(args[0]), manifest())
            exit_process.assert_called_once_with(0)
        for path in files:
            shutil.rmtree(path.parent)

    def test_changed_file_refused_at_launch_boundary(self):
        original = updates.download_verified
        def changed(meta, progress):
            path = original(meta, progress, self.local_installer)
            path.write_bytes(b"x" * len(PAYLOAD))
            return path
        with patch.object(updates, "download_verified", side_effect=changed), \
                patch.object(updates.subprocess, "Popen") as run, patch.object(updates.os, "_exit") as exit_process:
            self.up._install(manifest(), "desktop")
            self.assertEqual(self.up.job["state"], "error")
            run.assert_not_called()
            exit_process.assert_not_called()

    def test_source_install_refused_and_override_ignored_when_frozen(self):
        self.up.check(manual=True)
        with self.assertRaisesRegex(ValueError, "installed"):
            self.up.start("studio")
        with patch.object(sys, "frozen", True, create=True):
            self.assertEqual(updates.manifest_url(), updates.LATEST)

    def test_http_endpoints_and_write_guards(self):
        with patch.object(server, "_updater", self.up):
            http = server.serve(0, self.root / "workspace")
            threading.Thread(target=http.serve_forever, daemon=True).start()
            try:
                base = "http://127.0.0.1:%d/api/updater/" % http.server_port
                self.assertEqual(json.load(urlopen(base + "status"))["version"], "2.20.0-rc1")
                def post(action, data, **headers):
                    return urlopen(Request(base + action, json.dumps(data).encode(), headers=headers))
                with self.assertRaises(HTTPError) as denied:
                    post("check", {"manual": True})
                self.assertEqual(denied.exception.code, 403)
                denied.exception.close()
                result = json.load(post("check", {"manual": True}, **{"X-Studio": "1"}))
                self.assertEqual(result["available"]["version"], "2.20.1")
                self.assertFalse(json.load(post("settings", {"automatic": False}, **{"X-Studio": "1"}))["automatic"])
                for action in ("install", "check", "settings"):
                    with self.assertRaises(HTTPError) as denied:
                        urlopen(base + action)
                    denied.exception.close()
                with self.assertRaises(HTTPError) as denied:
                    post("install", {"relaunch": "studio"}, **{"X-Studio": "1"})
                denied.exception.close()
                with self.assertRaises(HTTPError) as denied:
                    post("check", {}, **{"X-Studio": "1", "Origin": "https://evil.test"})
                denied.exception.close()
            finally:
                http.shutdown()
                http.server_close()

    def test_make_latest_on_built_installer(self):
        version = updates.app_version()
        installer = REPO / ("build_installer/LakeShark-Setup-" + version + ".exe")
        self.assertTrue(installer.is_file(), "build the installer before this integration check")
        data = make_latest(installer, self.root / "latest.json", "A release for testing.")
        self.assertEqual(data["version"], version)
        self.assertEqual(data["size"], installer.stat().st_size)
        updates.validate_manifest(data)
        with installer.open("rb") as f:
            self.assertEqual(data["sha256"], hashlib.file_digest(f, "sha256").hexdigest())


if __name__ == "__main__":
    unittest.main()
