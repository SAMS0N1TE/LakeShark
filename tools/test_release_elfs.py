"""release_elfs.py pack: images are matched to their ELF by the app descriptor's ELF sha256.

    python tools/test_release_elfs.py
"""
import contextlib
import hashlib
import io
import os
import sys
import tempfile
import unittest
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import release_elfs


def elf(tag):
    return b"\x7fELF" + tag.encode() * 64


def image(version, elf_bytes):
    """Enough of an ESP app image for app_desc(): header, segment header, esp_app_desc_t."""
    img = bytearray(512)
    img[0] = 0xE9
    img[32:36] = bytes.fromhex("3254cdab")
    img[48:48 + len(version)] = version.encode()
    img[176:208] = hashlib.sha256(elf_bytes).digest()
    return bytes(img)


class PackTests(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.mkdtemp()

    def put(self, rel, data):
        p = os.path.join(self.dir, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "wb") as fh:
            fh.write(data)
        return p

    def pack(self, *extra):
        argv = ["release_elfs.py", "pack", "--version", "3.2.0", "--out", os.path.join(self.dir, "out"),
                os.path.join(self.dir, "in"), *extra]
        old, sys.argv = sys.argv, argv
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                release_elfs.main()
        finally:
            sys.argv = old
        return zipfile.ZipFile(os.path.join(self.dir, "out", "lakeshark-3.2.0-elf.zip"))

    def test_identical_images_share_one_elf_from_a_zip(self):
        e = elf("a")
        img = image("3.2.0-g0123456789ab", e)
        for ed in ("tdp4", "tdp4lr", "tdp4net"):
            self.put(f"in/{ed}-3.2.0-g0123456789ab-lakeshark.bin", img)
        with zipfile.ZipFile(self.put("in/gate/lakeshark.elf.zip", b""), "w") as z:
            z.writestr("lakeshark.elf", e)
        with self.pack() as z:
            sha = hashlib.sha256(e).hexdigest()
            self.assertEqual(sorted(z.namelist()), sorted([sha[:12] + ".elf", "ELFS.txt", "ELFS.json"]))
            self.assertEqual(z.read(sha[:12] + ".elf"), e)
            line = [l for l in z.read("ELFS.txt").decode().splitlines() if not l.startswith("#")]
            self.assertEqual(len(line), 1)
            self.assertIn("tdp4lr-3.2.0-g0123456789ab-lakeshark.bin", line[0])

    def test_two_images_two_elves_and_a_decoy(self):
        a, b = elf("a"), elf("b")
        self.put("in/tdp4/lakeshark.bin", image("3.2.0-gaaaaaaaaaaaa", a))
        self.put("in/tdp4tft/lakeshark.bin", image("3.2.0-gbbbbbbbbbbbb", b))
        self.put("in/x/lakeshark.elf", b)
        self.put("in/y/lakeshark.elf", elf("decoy"))
        self.put("in/z/lakeshark.elf", a)
        with self.pack() as z:
            elves = [n for n in z.namelist() if n.endswith(".elf")]
            self.assertEqual(sorted(elves), sorted(hashlib.sha256(x).hexdigest()[:12] + ".elf" for x in (a, b)))

    def test_missing_elf_stops_the_pack(self):
        self.put("in/tdp4/lakeshark.bin", image("3.2.0-gaaaaaaaaaaaa", elf("a")))
        self.put("in/x/lakeshark.elf", elf("other"))
        with self.assertRaises(SystemExit) as cm:
            self.pack()
        self.assertIn("no ELF for", str(cm.exception))

    def test_carried_image_needs_no_elf(self):
        self.put("in/tdp4-3.2.0-gaaaaaaaaaaaa-lakeshark.bin", image("3.2.0-gaaaaaaaaaaaa", elf("a")))
        self.put("in/a.elf", elf("a"))
        self.put("in/tdp4tft-2.23.2-g858445f63779-lakeshark.bin", image("2.23.2-g858445f63779", elf("old")))
        with self.assertRaises(SystemExit):
            self.pack()
        with self.pack("--carried", "tdp4tft-2.23.2-g858445f63779-lakeshark.bin") as z:
            self.assertEqual(len([n for n in z.namelist() if n.endswith(".elf")]), 1)

    def test_same_input_same_zip(self):
        self.put("in/tdp4/lakeshark.bin", image("3.2.0-gaaaaaaaaaaaa", elf("a")))
        self.put("in/a.elf", elf("a"))
        out = os.path.join(self.dir, "out", "lakeshark-3.2.0-elf.zip")
        self.pack().close()
        with open(out, "rb") as fh:
            first = fh.read()
        self.pack().close()
        with open(out, "rb") as fh:
            self.assertEqual(first, fh.read())

    def test_non_firmware_paths(self):
        ok = ["bench/tests/test_x.c", "docs/TDP4_RELEASE_NOTES.md", "tools/x.py", "README.md", "assets/x.png"]
        firmware = ["components/apps/tui/ls_tui.c", "main/main.c", "app-sdk/apps/detector/main.c",
                    "sdkconfig.defaults", "CMakeLists.txt", "boards/partitions_16m.csv"]
        self.assertTrue(all(release_elfs.NOT_FIRMWARE.search(p) for p in ok))
        self.assertFalse(any(release_elfs.NOT_FIRMWARE.search(p) for p in firmware))


if __name__ == "__main__":
    unittest.main()
