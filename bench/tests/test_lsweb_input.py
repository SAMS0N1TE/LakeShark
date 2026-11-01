"""Bridge byte-arrival/drain/framing tests; no serial device is opened."""
import importlib.util
from pathlib import Path
import threading
import unittest

ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location("lsweb", ROOT / "host/lsterm/lsweb.py")
lsweb = importlib.util.module_from_spec(spec)
spec.loader.exec_module(lsweb)


class FakeSerial:
    def __init__(self, data):
        self.data = bytearray(data)
        self.requests = []
        self.timeout = .01
        self.cancelled = False

    @property
    def in_waiting(self):
        return len(self.data)

    def read(self, n):
        self.requests.append((n, self.timeout))
        out = bytes(self.data[:n])
        del self.data[:n]
        return out

    def cancel_read(self):
        self.cancelled = True


class BridgeInput(unittest.TestCase):
    def test_blocking_first_byte_then_drain(self):
        board = lsweb.Board.__new__(lsweb.Board)  # never starts keep()/opens a port
        board.up = threading.Event()
        board.up.set()
        board.s = FakeSerial(b"abcdef")
        self.assertEqual(board.read(65536), b"abcdef")
        self.assertEqual(board.s.requests, [(1, None), (5, None)])
        board.interrupt_read()
        self.assertTrue(board.s.cancelled)
        board.up.clear()
        self.assertEqual(board.read(65536), b"")

    def test_fragmented_frames_forwarded_individually(self):
        split = lsweb.Split()
        out = []
        one = b"\x1e0003abc\x1f1234"
        two = b"\x1c0002de\x1f5678"
        end = b"\x1e0000\x1fffff"
        split.feed(b"term: caps ev1\n" + one[:8], on_frame=out.append)
        self.assertEqual(out, [])
        frames, lines = split.feed(one[8:] + two + end[:6], on_frame=out.append)
        self.assertEqual(frames, b"")
        self.assertEqual(out, [one, two])
        split.feed(end[6:], on_frame=out.append)
        self.assertEqual(out, [one, two, end])

    def test_console_caps_and_legacy_parser(self):
        frames, lines = lsweb.Split().feed(b"term caps\r\nterm: caps ev1\r\n\x1e0000\x1fffff")
        self.assertEqual(frames, b"\x1e0000\x1fffff")
        self.assertEqual(lines, ["term: caps ev1"])


if __name__ == "__main__":
    unittest.main()
