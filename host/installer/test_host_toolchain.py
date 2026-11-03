"""The host compiler must be verified before executing its self extractor."""
import hashlib
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "app-sdk"))
import host_toolchain
import toolchain


class HostInstallerTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name) / "toolchain"
        self.data = b"fake verified SFX"
        for p in (patch.object(host_toolchain, "root", return_value=self.root),
                  patch.object(host_toolchain, "SIZE", len(self.data)),
                  patch.object(host_toolchain, "SHA256", hashlib.sha256(self.data).hexdigest()),
                  patch.object(toolchain.urllib.request, "urlopen", return_value=io.BytesIO(self.data))):
            p.start()
            self.addCleanup(p.stop)

    def test_checksum_failure_never_executes_extractor(self):
        with patch.object(host_toolchain, "SHA256", "0" * 64), patch.object(host_toolchain, "extract") as extract:
            with self.assertRaisesRegex(ValueError, "SHA-256"):
                host_toolchain.install()
            extract.assert_not_called()
        self.assertFalse(host_toolchain.compiler_path().exists())

    def test_extraction_failure_is_not_installed(self):
        with patch.object(host_toolchain, "extract", side_effect=ValueError("extract failed")):
            with self.assertRaisesRegex(ValueError, "extract failed"):
                host_toolchain.install()
        self.assertFalse(host_toolchain.compiler_path().exists())
        self.assertFalse(list(self.root.glob(".download-*")))

    def test_verified_extract_and_reuse(self):
        def unpack(archive, dest):
            self.assertEqual(archive.read_bytes(), self.data)
            cc = host_toolchain.compiler_path(dest)
            cc.parent.mkdir(parents=True)
            cc.write_bytes(b"fake gcc")
            cc.with_name("strip.exe").write_bytes(b"fake strip")
        with patch.object(host_toolchain, "extract", side_effect=unpack) as extract:
            cc = host_toolchain.install()
            self.assertTrue(cc.is_file())
            extract.assert_called_once()
        with patch.object(toolchain.urllib.request, "urlopen", side_effect=AssertionError("must reuse")):
            self.assertEqual(host_toolchain.install(), cc)


if __name__ == "__main__":
    unittest.main()
