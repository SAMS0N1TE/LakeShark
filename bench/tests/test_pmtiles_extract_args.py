"""Argument validation for the PMTiles extractor."""

import contextlib
import importlib.util
import io
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "pmtiles_extract.py"
SPEC = importlib.util.spec_from_file_location("pmtiles_extract", SCRIPT)
extract = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(extract)


class RemoteReached(Exception):
    pass


class ExtractArgsTests(unittest.TestCase):
    def run_main(self, bbox, zoom=()):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "missing" / "extract.pmtiles"
            argv = ["pmtiles_extract.py", "--url", "https://example.test/map.pmtiles",
                    "--bbox", *bbox, *zoom, "--out", str(output)]
            stdout = io.StringIO()
            stderr = io.StringIO()
            with mock.patch.object(sys, "argv", argv), \
                 mock.patch.object(extract, "Remote", side_effect=RemoteReached) as remote, \
                 contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
                try:
                    extract.main()
                except SystemExit as exc:
                    result = exc
                except RemoteReached as exc:
                    result = exc
                else:
                    self.fail("main returned before reaching Remote")
            self.assertFalse(output.exists())
            self.assertFalse(output.parent.exists())
            return result, remote.call_count, stdout.getvalue(), stderr.getvalue()

    def test_invalid_bbox(self):
        cases = (
            (["20", "0", "10", "10"], "antimeridian"),
            (["10", "0", "10", "10"], "west < east"),
            (["10", "10", "20", "0"], "south < north"),
            (["10", "0", "20", "0"], "south < north"),
            (["-181", "0", "20", "10"], "[-180, 180]"),
            (["10", "0", "181", "10"], "[-180, 180]"),
            (["10", "-90", "20", "10"], "poles"),
            (["10", "0", "20", "90"], "poles"),
            (["nan", "0", "20", "10"], "finite"),
            # argparse reads -inf as a flag on some Pythons, a number on others.
            (["10", "-inf", "20", "10"], ("expected 4 arguments", "finite")),
            (["10", "0", "inf", "10"], "finite"),
            (["10", "0", "20", "nan"], "finite"),
        )
        for bbox, message in cases:
            with self.subTest(bbox=bbox):
                result, calls, stdout, stderr = self.run_main(bbox)
                self.assertIsInstance(result, SystemExit)
                self.assertEqual(result.code, 2)
                self.assertEqual(calls, 0)
                self.assertEqual(stdout, "")
                if isinstance(message, tuple):
                    self.assertTrue(any(m in stderr for m in message), stderr)
                else:
                    self.assertIn(message, stderr)

    def test_invalid_zoom(self):
        bbox = ["10", "0", "20", "10"]
        for zoom in (["--minzoom", "-1"],
                     ["--maxzoom", "27"],
                     ["--minzoom", "5", "--maxzoom", "4"]):
            with self.subTest(zoom=zoom):
                result, calls, stdout, stderr = self.run_main(bbox, zoom)
                self.assertIsInstance(result, SystemExit)
                self.assertEqual(result.code, 2)
                self.assertEqual(calls, 0)
                self.assertEqual(stdout, "")
                self.assertIn("0 <= minzoom <= maxzoom <= 26", stderr)

    def test_valid_bbox_is_echoed_before_remote(self):
        result, calls, stdout, stderr = self.run_main(
            ["-73.5", "40.25", "-72.5", "41.75"],
            ["--minzoom", "0", "--maxzoom", "26"])
        self.assertIsInstance(result, RemoteReached)
        self.assertEqual(calls, 1)
        self.assertEqual(stderr, "")
        self.assertEqual(stdout.strip(),
                         "bbox: west=-73.5 south=40.25 east=-72.5 north=41.75")


if __name__ == "__main__":
    unittest.main()

