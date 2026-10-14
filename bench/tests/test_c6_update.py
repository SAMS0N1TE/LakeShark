"""Compile and exercise the production C validator/state machine, no IDF or ports.

Run: python bench/tests/test_c6_update.py
Only this test's DLL is built. hashlib supplies the platform SHA primitive;
the image parsing, digest comparison, version policy and sequencing are C.
"""
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / "bench/out/c6"
SLOT = 0x1E0000
U8P = C.POINTER(C.c_uint8)
HASH = C.CFUNCTYPE(C.c_int, U8P, C.c_size_t, U8P)
SIMPLE = C.CFUNCTYPE(C.c_int, C.c_void_p)
VERSION = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_void_p)
WRITE = C.CFUNCTYPE(C.c_int, C.c_void_p, U8P, C.c_size_t)
WAIT = C.CFUNCTYPE(C.c_int, C.c_void_p, C.c_uint, C.c_void_p)
MILLIS = C.CFUNCTYPE(C.c_uint64, C.c_void_p)


class Image(C.Structure):
    _fields_ = [("project", C.c_char * 32), ("version", C.c_char * 32)]


class Result(C.Structure):
    _fields_ = [("stage", C.c_int), ("boot", C.c_int),
                ("sent", C.c_size_t), ("size", C.c_size_t), ("error", C.c_int),
                ("old_version", C.c_char * 32), ("new_version", C.c_char * 32),
                ("message", C.c_char * 160)]


REPORT = C.CFUNCTYPE(None, C.c_void_p, C.POINTER(Result))


class Transport(C.Structure):
    _fields_ = [("ctx", C.c_void_p), ("power", SIMPLE), ("version", VERSION),
                ("stop", SIMPLE), ("begin", SIMPLE), ("write", WRITE),
                ("end", SIMPLE), ("activate", SIMPLE), ("wait", WAIT),
                ("millis", MILLIS), ("report", REPORT)]


@HASH
def sha(data, size, digest):
    C.memmove(digest, hashlib.sha256(C.string_at(data, size)).digest(), 32)
    return 0


def buffer(data):
    return (C.c_uint8 * len(data)).from_buffer_copy(data)


def resign(data):
    """Repair checksum/hash so metadata tests aren't merely hash failures."""
    data = bytearray(data)
    p, checksum = 24, 0xEF
    for _ in range(data[1]):
        size = struct.unpack_from("<I", data, p + 4)[0]
        p += 8
        for byte in data[p:p + size]:
            checksum ^= byte
        p += size
    hashed = (p + 16) & ~15
    data[hashed - 1] = checksum
    data[hashed:hashed + 32] = hashlib.sha256(data[:hashed]).digest()
    return bytes(data)


class Fake:
    def __init__(self, fail=None, old=b"2.12.8", new=b"2.12.9", timeout=False):
        self.fail, self.old, self.new, self.timeout = fail, old, new, timeout
        self.calls, self.sent, self.reports = [], bytearray(), []
        self.off = False
        self.now = 0
        self.io = Transport(None, SIMPLE(lambda _: self.call("power")),
                            VERSION(self.version), SIMPLE(self.stop),
                            SIMPLE(lambda _: self.call("begin")), WRITE(self.write),
                            SIMPLE(lambda _: self.call("end")),
                            SIMPLE(lambda _: self.call("activate")), WAIT(self.wait),
                            MILLIS(self.millis), REPORT(self.report))

    def call(self, name):
        self.calls.append(name)
        return -7 if self.fail == name else 0

    def version(self, _, out):
        C.memmove(out, self.old + b"\0", len(self.old) + 1)
        return self.call("version")

    def stop(self, _):
        self.off = True
        return self.call("stop")

    def write(self, _, data, size):
        self.sent.extend(C.string_at(data, size))
        return self.call("write")

    def wait(self, _, timeout, out):
        self.wait_limit = timeout
        C.memmove(out, self.new + b"\0", len(self.new) + 1)
        return self.call("wait")

    def millis(self, _):
        self.now += 600001 if self.timeout else 1
        return self.now

    def report(self, _, ptr):
        r = ptr.contents
        self.reports.append((r.stage, r.sent, bytes(r.message)))


class C6Tests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        OUT.mkdir(parents=True, exist_ok=True)
        compiler = os.environ.get("CC") or shutil.which("gcc") or shutil.which("clang")
        if not compiler:
            raise RuntimeError("gcc or clang required")
        target = OUT / ("c6_core.dll" if os.name == "nt" else "c6_core.so")
        subprocess.run([compiler, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O2",
                        "-shared", *([] if os.name == "nt" else ["-fPIC"]),
                        str(ROOT / "main/ls_c6_core.c"), "-o", str(target)], check=True)
        cls.lib = C.CDLL(str(target))
        cls.lib.ls_c6_validate.argtypes = [U8P, C.c_size_t, C.c_size_t, HASH, C.POINTER(Image)]
        cls.lib.ls_c6_validate.restype = C.c_char_p
        cls.lib.ls_c6_version_allowed.argtypes = [C.c_char_p, C.c_char_p, C.c_bool]
        cls.lib.ls_c6_version_allowed.restype = C.c_bool
        cls.lib.ls_c6_run.argtypes = [U8P, C.c_size_t, C.c_bool, HASH,
                                     C.POINTER(Transport), C.POINTER(Result)]
        cls.lib.ls_c6_run.restype = C.c_bool
        cls.good = (ROOT / "c6_firmware/network_adapter.bin").read_bytes()

    def validate(self, data, slot=SLOT):
        desc = Image()
        error = self.lib.ls_c6_validate(buffer(data), len(data), slot, sha, C.byref(desc))
        return error, desc

    def run_image(self, fake, data=None, force=False):
        data = self.good if data is None else data
        result = Result()
        ok = self.lib.ls_c6_run(buffer(data), len(data), force, sha, C.byref(fake.io), C.byref(result))
        return ok, result

    def test_shipped_image(self):
        error, desc = self.validate(self.good)
        self.assertIsNone(error)
        self.assertEqual(desc.project, b"network_adapter")
        self.assertEqual(desc.version, b"2.12.9")

    def test_partition_contract(self):
        data = (ROOT / "c6_firmware/partition-table.bin").read_bytes()
        slots = []
        for pos in range(0, len(data), 32):
            magic, kind, subtype, offset, size, _, _ = struct.unpack_from("<HBBII16sI", data, pos)
            if magic != 0x50AA:
                break
            if kind == 0:
                slots.append((subtype, offset, size))
        self.assertEqual(slots, [(16, 0x10000, SLOT), (17, 0x1F0000, SLOT)])

    def test_provenance_and_reconstructed_layout(self):
        provenance = json.loads((ROOT / "c6_firmware/provenance.json").read_text())
        for name, expected in provenance["shipped_files"].items():
            data = (ROOT / "c6_firmware" / name).read_bytes()
            self.assertEqual(len(data), expected["bytes"])
            self.assertEqual(hashlib.sha256(data).hexdigest(), expected["sha256"])
        self.assertEqual(self.good[144:176].split(b"\0")[0].decode(), provenance["shipped_idf"])
        rows = [line.split(",") for line in (ROOT / "c6_firmware/partitions.csv").read_text().splitlines()
                if line and not line.startswith("#")]
        slots = [(row[0].strip(), int(row[3], 0), int(row[4], 0)) for row in rows if row[1].strip() == "app"]
        self.assertEqual(slots, [("ota_0", 0x10000, SLOT), ("ota_1", 0x1F0000, SLOT)])

    def test_truncated(self):
        for length in [0, 1, 23, 24, 31, 287, len(self.good) // 2, len(self.good) - 1]:
            with self.subTest(length=length):
                self.assertIsNotNone(self.validate(self.good[:length])[0])

    def test_wrong_magic(self):
        d = bytearray(self.good); d[0] = 0
        self.assertIn(b"magic", self.validate(d)[0])

    def test_wrong_chip(self):
        d = bytearray(self.good); d[12] = 18
        self.assertIn(b"ESP32-C6", self.validate(resign(d))[0])

    def test_bad_hash(self):
        d = bytearray(self.good); d[-1] ^= 1
        self.assertIn(b"SHA-256", self.validate(d)[0])

    def test_bad_checksum(self):
        d = bytearray(self.good); d[-33] ^= 1
        self.assertIn(b"checksum", self.validate(d)[0])

    def test_missing_hash(self):
        d = bytearray(self.good); d[23] = 0
        self.assertIn(b"SHA-256", self.validate(d)[0])

    def test_oversized(self):
        self.assertIn(b"slot", self.validate(self.good, len(self.good) - 1)[0])
        self.assertIn(b"slot", self.validate(self.good + bytes(SLOT))[0])

    def test_segment_overflow(self):
        d = bytearray(self.good); struct.pack_into("<I", d, 28, 0xFFFFFFFF)
        self.assertIn(b"bounds", self.validate(d)[0])

    def test_segments_count(self):
        for count in [0, 17, 255]:
            d = bytearray(self.good); d[1] = count
            self.assertIn(b"segment count", self.validate(d)[0])

    def test_app_desc(self):
        d = bytearray(self.good); d[32] ^= 1
        self.assertIn(b"app_desc", self.validate(resign(d))[0])
        d = bytearray(self.good); d[48:80] = b"x" * 32
        self.assertIn(b"version", self.validate(resign(d))[0])
        d = bytearray(self.good); d[80] = ord("X")
        self.assertIn(b"project", self.validate(resign(d))[0])

    def test_trailing_payload(self):
        self.assertIn(b"trailing", self.validate(self.good + b"extra")[0])

    def test_version_order(self):
        for candidate, old, allowed in [("2.12.9", "2.12.8", True),
                ("2.12.9", "2.12.9", False), ("2.12.9", "2.13.0", False),
                ("2.10.0", "2.9.9", True), ("2.12.9-rc1", "2.12.8", False),
                ("4294967296.0.0", "2.12.9", False), ("x", "2.12.9", False)]:
            with self.subTest(candidate=candidate, old=old):
                self.assertEqual(self.lib.ls_c6_version_allowed(candidate.encode(), old.encode(), False), allowed)
                self.assertTrue(self.lib.ls_c6_version_allowed(candidate.encode(), old.encode(), True))

    def test_success_and_exact_bytes(self):
        fake = Fake()
        ok, r = self.run_image(fake)
        self.assertTrue(ok)
        self.assertEqual(fake.calls[:4], ["power", "version", "stop", "begin"])
        self.assertEqual(fake.calls[-3:], ["end", "activate", "wait"])
        self.assertEqual(bytes(fake.sent), self.good)
        self.assertEqual(r.sent, len(self.good))
        self.assertEqual(r.stage, 7)
        self.assertTrue(fake.off)
        self.assertEqual(fake.wait_limit, 45000)
        self.assertIn(b"2.12.8 -> 2.12.9", r.message)
        progress = [sent for stage, sent, _ in fake.reports if stage == 3]
        self.assertEqual(progress, list(range(1400, len(self.good), 1400)) + [len(self.good)])

    def test_identical_force(self):
        fake = Fake(old=b"2.12.9")
        ok, r = self.run_image(fake, force=True)
        self.assertTrue(ok)
        self.assertIn(b"2.12.9 -> 2.12.9", r.message)

    def test_same_and_downgrade_refused_before_stop(self):
        for old in [b"2.12.9", b"2.13.0"]:
            fake = Fake(old=old)
            ok, r = self.run_image(fake)
            self.assertFalse(ok)
            self.assertEqual(fake.calls, ["power", "version"])
            self.assertIn(b"force required", r.message)

    def test_invalid_image_never_touches_transport_even_forced(self):
        fake = Fake()
        ok, r = self.run_image(fake, self.good[:-1], force=True)
        self.assertFalse(ok)
        self.assertEqual(fake.calls, [])
        self.assertEqual(r.boot, 0)

    def test_pre_activation_failures_preserve_old_image(self):
        for failure in ["power", "version", "stop", "begin", "write", "end"]:
            with self.subTest(failure=failure):
                fake = Fake(fail=failure)
                ok, r = self.run_image(fake)
                self.assertFalse(ok)
                self.assertEqual(fake.calls[-1], failure)
                self.assertNotIn("activate", fake.calls)
                self.assertEqual(r.boot, 0)
                self.assertIn(b"old image still active", r.message)
                self.assertEqual(r.error, -7)
                if failure not in ["power", "version"]:
                    self.assertTrue(fake.off)

    def test_activation_failure_is_ambiguous(self):
        fake = Fake(fail="activate")
        ok, r = self.run_image(fake)
        self.assertFalse(ok)
        self.assertEqual(r.boot, 1)
        self.assertIn(b"boot selection unknown", r.message)
        self.assertNotIn("wait", fake.calls)

    def test_reboot_timeout(self):
        fake = Fake(fail="wait")
        ok, r = self.run_image(fake)
        self.assertFalse(ok)
        self.assertEqual(r.boot, 2)
        self.assertIn(b"activation accepted", r.message)
        self.assertTrue(fake.off)

    def test_wrong_returned_version(self):
        fake = Fake(new=b"2.12.8")
        ok, r = self.run_image(fake)
        self.assertFalse(ok)
        self.assertIn(b"Unexpected version", r.message)

    def test_transfer_deadline(self):
        fake = Fake(timeout=True)
        ok, r = self.run_image(fake)
        self.assertFalse(ok)
        self.assertIn(b"timed out", r.message)
        self.assertNotIn("write", fake.calls)
        self.assertNotIn("end", fake.calls)
        self.assertEqual(r.boot, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
