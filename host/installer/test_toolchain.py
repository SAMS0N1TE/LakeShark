"""Offline security and failure tests for the per-user compiler installer."""
import hashlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "app-sdk"))
import toolchain


def archive(extra=None):
    stream = io.BytesIO()
    with zipfile.ZipFile(stream, "w") as z:
        z.writestr("riscv32-esp-elf/bin/riscv32-esp-elf-gcc.exe", b"test compiler")
        z.writestr("riscv32-esp-elf/bin/riscv32-esp-elf-strip.exe", b"test strip")
        if extra:
            z.writestr(extra, b"outside")
    return stream.getvalue()


class InstallerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / "toolchain"
        self.patches = [patch.object(toolchain, "root", return_value=self.root)]
        for p in self.patches:
            p.start()
            self.addCleanup(p.stop)

    def download(self, data, digest=None):
        for p in (patch.object(toolchain, "SIZE", len(data)),
                  patch.object(toolchain, "SHA256", digest or hashlib.sha256(data).hexdigest()),
                  patch.object(toolchain.urllib.request, "urlopen", return_value=io.BytesIO(data))):
            p.start()
            self.addCleanup(p.stop)

    def test_verified_install_and_idempotence(self):
        self.download(archive())
        progress = []
        result = toolchain.install(lambda **kw: progress.append(kw))
        self.assertTrue(result.is_file())
        self.assertEqual(result, toolchain.compiler_path())
        self.assertTrue(any("Extracting" in j.get("message", "") for j in progress))
        with patch.object(toolchain.urllib.request, "urlopen", side_effect=AssertionError("must reuse")):
            self.assertEqual(toolchain.install(), result)
        self.assertFalse(list(self.root.glob(".download-*")))

    def test_bad_checksum_never_exposes_compiler(self):
        self.download(archive(), "0" * 64)
        with self.assertRaisesRegex(ValueError, "SHA-256"):
            toolchain.install()
        self.assertFalse(toolchain.compiler_path().exists())
        self.assertFalse(list(self.root.glob(".download-*")))

    def test_traversal_and_windows_drive_paths_rejected(self):
        for name in ("../../escape", "C:/escape", "..\\escape"):
            with self.subTest(name=name):
                self.download(archive(name))
                with self.assertRaisesRegex(ValueError, "Unsafe path"):
                    toolchain.install()
                self.assertFalse(toolchain.compiler_path().exists())

    def test_network_error_is_pollable_and_retryable(self):
        import time
        with patch.object(toolchain.urllib.request, "urlopen", side_effect=OSError("offline")):
            toolchain.start()
            deadline = time.monotonic() + 5
            while toolchain.status()["state"] == "running" and time.monotonic() < deadline:
                time.sleep(.01)
        self.assertEqual(toolchain.status()["state"], "error")
        self.assertIn("offline", toolchain.status()["message"])
        self.download(archive())
        toolchain.start()
        deadline = time.monotonic() + 5
        while toolchain.status()["state"] == "running" and time.monotonic() < deadline:
            time.sleep(.01)
        self.assertEqual(toolchain.status()["state"], "done")


if __name__ == "__main__":
    unittest.main()
