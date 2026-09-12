"""Synthetic packages and mocked transports only: never opens a serial port."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

SCRIPT = Path(__file__).resolve().parents[2] / "tools/lakeshark_upgrade.py"
spec = importlib.util.spec_from_file_location("upgrade", SCRIPT)
upgrade = importlib.util.module_from_spec(spec)
spec.loader.exec_module(upgrade)


def table_image():
    data = b""
    for kind, subtype, offset, size, label in [
        (1, 2, 0x9000, 0x6000, b"nvs"), (1, 1, 0xF000, 0x1000, b"phy_init"),
        (0, 0, 0x10000, 0xE00000, b"factory"),
        (1, 0x82, 0xE10000, 0x800000, b"storage"),
        (1, 3, 0x1610000, 0x10000, b"coredump")]:
        data += struct.pack("<HBBII16sI", 0x50AA, kind, subtype, offset, size, label, 0)
    data += b"\xeb\xeb" + b"\xff" * 14 + hashlib.md5(data).digest()
    return data.ljust(upgrade.TABLE_BYTES, b"\xff")


def app_image():
    elf = bytearray(64)
    elf[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<H", elf, 18, 243)
    body = bytearray(256)
    struct.pack_into("<I", body, 0, 0xABCD5432)
    body[16:19] = b"v42"
    body[48:57] = b"lakeshark"
    body[144:176] = hashlib.sha256(elf).digest()
    body.extend(upgrade.BOARD.encode())
    image = bytearray(24)
    image[0], image[1], image[3], image[23] = 0xE9, 1, 0x50, 1
    struct.pack_into("<H", image, 12, 18)
    image.extend(struct.pack("<II", 0x40000000, len(body)))
    image.extend(body)
    checksum = 0xEF
    for byte in body:
        checksum ^= byte
    image.extend(b"\0" * (15 - len(image) % 16))
    image.append(checksum)
    image.extend(hashlib.sha256(image).digest())
    return bytes(image), bytes(elf)


class FakeDevice:
    def __init__(self, table, old_app):
        self.table, self.app = table, old_app
        self.entered = self.closed = self.reset_called = False
        self.writes = []
        self.bad_readback = False
        self.error = None

    def __call__(self, port):
        self.port = port
        return self

    def __enter__(self):
        self.entered = True
        return self

    def __exit__(self, *error):
        self.closed = True

    def inspect(self):
        if self.error:
            raise self.error
        return "ESP32-P4 fixture"

    def read(self, offset, size):
        if offset == upgrade.TABLE_OFFSET:
            return self.table[:size]
        data = self.app[offset - upgrade.APP_OFFSET:offset - upgrade.APP_OFFSET + size]
        return b"\0" * size if self.bad_readback and self.writes else data

    def write_app(self, app):
        self.writes.append((upgrade.APP_OFFSET, app))
        self.app = app

    def reset(self):
        self.reset_called = True


class UpgradeTests(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix="ls-upgrade-test-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.app, self.elf = app_image()
        self.table = table_image()
        self.manifest = dict(schema=1, kind="application-only-upgrade-candidate", board=upgrade.BOARD,
            flash_bytes=upgrade.FLASH_BYTES, app_offset="0x10000", app_partition_bytes=upgrade.APP_BYTES,
            version="v42", app_sha256=upgrade.sha256(self.app), elf_sha256=upgrade.sha256(self.elf),
            partition_table_sha256=upgrade.sha256(self.table), license_files=[], documentation_files=[])
        self.files = {"app.bin": self.app, "app.elf": self.elf, "FLASH_INSTRUCTIONS.txt": b"fixture"}
        self.save()
        self.device = FakeDevice(self.table, self.app)

    def save(self):
        self.files["manifest.json"] = json.dumps(self.manifest).encode()
        for name, data in self.files.items():
            (self.root / name).write_bytes(data)
        (self.root / "SHA256SUMS.txt").write_text("".join(
            f"{upgrade.sha256(data)}  {name}\n" for name, data in self.files.items()))

    def run_tool(self, flash=True, replies=("CONNECT", "FLASH"), extra=()):
        answers = iter(replies)
        args = [str(self.root), "--board", upgrade.BOARD_OPTION]
        if flash:
            args += ["--flash", "--port", "COM77", "--confirm-board", upgrade.BOARD]
        return upgrade.main(args + list(extra), factory=self.device,
            prompt=lambda _: next(answers), output=lambda _: None)

    def test_default_is_offline_without_transport(self):
        self.assertEqual(self.run_tool(False), 0)
        self.assertFalse(self.device.entered)

    def test_success_writes_only_app_and_verifies_then_resets(self):
        self.assertEqual(self.run_tool(), 0)
        self.assertEqual(self.device.writes, [(0x10000, self.app)])
        self.assertTrue(self.device.closed and self.device.reset_called)

    def test_cancel_before_connect_never_opens_port(self):
        self.assertEqual(self.run_tool(replies=("no",)), 1)
        self.assertFalse(self.device.entered)
        self.assertFalse(self.device.writes)

    def test_cancel_after_preflight_closes_without_writes(self):
        self.assertEqual(self.run_tool(replies=("CONNECT", "no")), 1)
        self.assertTrue(self.device.closed)
        self.assertFalse(self.device.writes or self.device.reset_called)

    def test_bad_checksum_rejected_before_connection(self):
        (self.root / "app.bin").write_bytes(self.app + b"tampered")
        self.assertEqual(self.run_tool(), 2)
        self.assertFalse(self.device.entered)

    def test_self_consistent_bad_image_still_rejected(self):
        self.files["app.bin"] = self.app[:-1] + bytes([self.app[-1] ^ 1])
        self.manifest["app_sha256"] = upgrade.sha256(self.files["app.bin"])
        self.save()
        self.assertEqual(self.run_tool(), 2)
        self.assertFalse(self.device.entered)

    def test_wrong_manifest_board_rejected(self):
        self.manifest["board"] = "ESP32-P4-NANO"
        self.save()
        self.assertEqual(self.run_tool(), 2)
        self.assertFalse(self.device.entered)

    def test_physical_confirmation_required(self):
        self.assertEqual(self.run_tool(extra=("--confirm-board", "P4")), 2)
        self.assertFalse(self.device.entered)

    def test_layout_mismatch_never_writes(self):
        self.device.table = b"\xff" * upgrade.TABLE_BYTES
        self.assertEqual(self.run_tool(), 2)
        self.assertTrue(self.device.closed)
        self.assertFalse(self.device.writes)

    def test_manifest_cannot_authorize_different_layout(self):
        self.device.table = self.table[:12] + b"bad!" + self.table[16:]
        self.manifest["partition_table_sha256"] = upgrade.sha256(self.device.table)
        self.save()
        self.assertEqual(self.run_tool(), 2)
        self.assertFalse(self.device.writes)

    def test_first_install_refused(self):
        self.device.app = b"\xff" * 288
        self.assertEqual(self.run_tool(), 2)
        self.assertFalse(self.device.writes)

    def test_failed_readback_does_not_report_success_or_reset(self):
        self.device.bad_readback = True
        self.assertEqual(self.run_tool(), 2)
        self.assertEqual(len(self.device.writes), 1)
        self.assertTrue(self.device.closed)
        self.assertFalse(self.device.reset_called)

    def test_transport_failure_closes_without_writes(self):
        self.device.error = RuntimeError("fixture transport failure")
        self.assertEqual(self.run_tool(), 2)
        self.assertTrue(self.device.closed)
        self.assertFalse(self.device.writes)

    def test_extra_files_and_traversal_rejected(self):
        (self.root / "nvs.bin").write_bytes(b"do not write")
        self.assertEqual(self.run_tool(), 2)
        self.assertFalse(self.device.entered)
        with self.assertRaises(upgrade.Refused):
            upgrade.safe_name("../app.bin")

    def test_port_cannot_be_used_without_explicit_flash(self):
        self.assertEqual(self.run_tool(False, extra=("--port", "MOCK")), 2)
        self.assertFalse(self.device.entered)

    def test_network_serial_url_rejected_before_connection(self):
        self.assertEqual(self.run_tool(extra=("--port", "rfc2217://example.invalid:2217")), 2)
        self.assertFalse(self.device.entered)


class AdapterTests(unittest.TestCase):
    def test_detection_failure_closes_owned_port(self):
        opened = []
        serial_port = types.SimpleNamespace(open=lambda: opened.append("open"),
                                            close=lambda: opened.append("close"))
        serial_module = types.ModuleType("serial")
        serial_module.Serial = lambda **kwargs: serial_port
        cmds = types.ModuleType("esptool.cmds")
        def fail(*args, **kwargs):
            raise RuntimeError("detection failed")
        cmds.detect_chip = fail
        adapter = upgrade.EsptoolDevice.__new__(upgrade.EsptoolDevice)
        adapter.port = "COM77"
        with patch.dict(sys.modules, {"serial": serial_module, "esptool.cmds": cmds}):
            with self.assertRaises(RuntimeError):
                adapter.__enter__()
        self.assertEqual(opened, ["open", "close"])

    def test_chip_security_and_capacity_fail_before_writes(self):
        cases = [dict(chip="ESP32-C6"), dict(secure=True), dict(flags=1), dict(flags=4),
                 dict(crypt=1), dict(crypt=2), dict(secure_boot=True), dict(encrypted=True),
                 dict(flash="16MB"), dict(security={})]
        for change in cases:
            with self.subTest(change=change):
                state = dict(chip="ESP32-P4", secure=False, flags=0, crypt=0,
                    secure_boot=False, encrypted=False, flash="32MB")
                state.update(change)
                rom = types.SimpleNamespace(CHIP_NAME=state["chip"], secure_download_mode=state["secure"],
                    get_security_info=lambda: state.get("security", dict(chip_id=18, flags=state["flags"], flash_crypt_cnt=state["crypt"])),
                    get_secure_boot_enabled=lambda: state["secure_boot"],
                    get_flash_encryption_enabled=lambda: state["encrypted"],
                    flash_spi_attach=lambda _: None, flash_set_parameters=lambda _: None)
                rom.run_stub = lambda: rom
                adapter = upgrade.EsptoolDevice.__new__(upgrade.EsptoolDevice)
                adapter.esp = rom
                cmds = types.ModuleType("esptool.cmds")
                cmds.detect_flash_size = lambda _: state["flash"]
                with patch.dict(sys.modules, {"esptool.cmds": cmds}):
                    with self.assertRaises(upgrade.Refused):
                        adapter.inspect()

    def test_adapter_never_enables_force_erase_or_encryption(self):
        adapter = upgrade.EsptoolDevice.__new__(upgrade.EsptoolDevice)
        adapter.esp = object()
        seen = []
        cmds = types.ModuleType("esptool.cmds")
        cmds.write_flash = lambda device, args: seen.append(args)
        with patch.dict(sys.modules, {"esptool.cmds": cmds}):
            adapter.write_app(b"fixture")
        args = seen[0]
        self.assertFalse(args.force or args.erase_all or args.encrypt)
        self.assertIsNone(args.encrypt_files)
        self.assertEqual(args.flash_size, "keep")
        self.assertEqual([(offset, data.read()) for offset, data in args.addr_filename], [(0x10000, b"fixture")])


if __name__ == "__main__":
    unittest.main()
