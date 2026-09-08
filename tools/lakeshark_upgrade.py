"""Offline package checks by default; explicit, guarded LCD4.3 app upgrade."""
import argparse
import hashlib
import io
import json
from pathlib import Path, PurePosixPath
import re
import struct

BOARD = "ESP32-P4-WIFI6-Touch-LCD-4.3"
BOARD_OPTION = "p4-touch-lcd-43"
FLASH_BYTES = 32 * 1024 * 1024
APP_OFFSET = 0x10000
APP_BYTES = 14 * 1024 * 1024
TABLE_OFFSET = 0x8000
TABLE_BYTES = 0xC00
MAX_PACKAGE_BYTES = 128 * 1024 * 1024
SUPPORTED_ESPTOOL = {"4.11.0", "4.12.dev1"}


class Refused(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise Refused(message)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def safe_name(name):
    require(isinstance(name, str) and name and "\\" not in name and ":" not in name,
            "Invalid package path")
    p = PurePosixPath(name)
    require(not p.is_absolute() and all(x not in ("", ".", "..") for x in name.split("/")),
            "Escaping or noncanonical package path")
    return name


def read_member(root, name, limit):
    path = root
    for component in safe_name(name).split("/"):
        path /= component
        require(not path.is_symlink(), "Package symlinks are not supported")
    require(path.is_file() and path.resolve().is_relative_to(root), "Missing/escaping package file")
    require(path.stat().st_size <= limit, "Package file exceeds size limit")
    with path.open("rb") as source:
        data = source.read(limit + 1)
    require(len(data) <= limit, "Package file grew beyond size limit")
    return data


def descriptor(image):
    require(len(image) >= 288 and image[0] == 0xE9, "Missing ESP application image; first install is unsupported")
    require(struct.unpack_from("<H", image, 12)[0] == 18, "Application is not ESP32-P4")
    require(struct.unpack_from("<I", image, 32)[0] == 0xABCD5432, "Missing application descriptor")
    require(struct.unpack_from("<I", image, 36)[0] == 0, "Anti-rollback application requires a separate upgrade procedure")
    def string(start):
        return image[start:start + 32].split(b"\0", 1)[0].decode("utf-8", errors="strict")
    require(string(80) == "lakeshark", "Existing/packaged application is not LakeShark")
    return string(48)


def validate_image(app, elf, manifest):
    require(descriptor(app) == manifest["version"], "Application version mismatch")
    require(app[3] >> 4 == 5 and app[23] == 1, "Expected 32MB image with SHA256 trailer")
    require(1 <= app[1] <= 16, "Invalid image segment count")
    position, checksum = 24, 0xEF
    for _ in range(app[1]):
        require(position + 8 <= len(app), "Truncated image segment header")
        size = struct.unpack_from("<I", app, position + 4)[0]
        position += 8
        require(position + size <= len(app), "Truncated image segment")
        for byte in app[position:position + size]:
            checksum ^= byte
        position += size
    checksum_at = position + (15 - position % 16)
    require(checksum_at + 33 == len(app), "Unexpected image trailer")
    require(app[checksum_at] == checksum, "Image XOR checksum mismatch")
    require(hashlib.sha256(app[:checksum_at + 1]).digest() == app[-32:], "Image SHA256 trailer mismatch")
    require(len(elf) >= 52 and elf[:7] == b"\x7fELF\x01\x01\x01" and
            struct.unpack_from("<H", elf, 18)[0] == 243, "Expected 32-bit RISC-V ELF")
    require(app[176:208] == hashlib.sha256(elf).digest(), "Application/ELF mismatch")
    require(BOARD.encode() in app, "LCD4.3 identity missing from packaged image")
    require(len(app) <= APP_BYTES, "Application exceeds factory partition")


def check_package(directory, board):
    require(board == BOARD_OPTION, "Unsupported board; only LCD4.3 upgrades are supported")
    root = Path(directory).resolve()
    require(root.is_dir(), "Use an extracted package directory, not a ZIP or build directory")
    checksum_text = read_member(root, "SHA256SUMS.txt", 64 * 1024).decode("utf-8")
    sums = {}
    for line in checksum_text.splitlines():
        match = re.fullmatch(r"([0-9a-f]{64})  (.+)", line)
        require(match is not None, "Malformed SHA256SUMS entry")
        name = safe_name(match[2])
        require(name not in sums and name != "SHA256SUMS.txt", "Duplicate/self-referential checksum")
        sums[name] = match[1]
    require(4 <= len(sums) <= 256, "Unexpected package file count")
    manifest_data = read_member(root, "manifest.json", 4 * 1024 * 1024)
    require(sums.get("manifest.json") == sha256(manifest_data), "Manifest checksum mismatch")
    manifest = json.loads(manifest_data)
    require(manifest.get("schema") == 1 and manifest.get("kind") == "application-only-upgrade-candidate",
            "Unsupported package schema/kind; this is not a first-install tool")
    require(manifest.get("board") == BOARD, "Package board does not match selected physical board")
    require(manifest.get("flash_bytes") == FLASH_BYTES and manifest.get("app_offset") == "0x10000" and
            manifest.get("app_partition_bytes") == APP_BYTES, "Unsupported flash/partition contract")
    require(re.fullmatch(r"[0-9a-f]{64}", manifest.get("partition_table_sha256", "")) is not None,
            "Missing partition table SHA256")
    expected = {"app.bin", "app.elf", "manifest.json", "FLASH_INSTRUCTIONS.txt"}
    for key, prefix in (("license_files", "licenses/"), ("documentation_files", "docs/")):
        names = manifest.get(key, [])
        require(isinstance(names, list) and len(names) == len(set(names)), "Invalid manifest file list")
        for name in names:
            require(safe_name(name).startswith(prefix), "Manifest file outside approved category")
            expected.add(name)
    require(set(sums) == expected, "Checksums do not cover the exact package manifest")
    actual = set()
    for path in root.rglob("*"):
        require(not path.is_symlink(), "Package symlinks are not supported")
        if path.is_file():
            actual.add(path.relative_to(root).as_posix())
    require(actual == expected | {"SHA256SUMS.txt"}, "Unexpected/missing files in package")
    retained, total = {}, 0
    for name, checksum in sums.items():
        limit = APP_BYTES if name == "app.bin" else 64 * 1024 * 1024 if name == "app.elf" else 4 * 1024 * 1024
        data = read_member(root, name, min(limit, MAX_PACKAGE_BYTES - total))
        total += len(data)
        require(sha256(data) == checksum, "Checksum mismatch: " + name)
        if name in ("app.bin", "app.elf"):
            retained[name] = data
    app, elf = retained["app.bin"], retained["app.elf"]
    require(sha256(app) == manifest.get("app_sha256") and sha256(elf) == manifest.get("elf_sha256"),
            "Manifest binary hashes disagree with checksums")
    validate_image(app, elf, manifest)
    return manifest, app


def check_partitions(table, manifest):
    require(len(table) == TABLE_BYTES, "Short partition table read")
    require(sha256(table) == manifest["partition_table_sha256"], "Device partition table differs from package; no migration supported")
    entries = []
    for position in range(0, TABLE_BYTES, 32):
        magic = struct.unpack_from("<H", table, position)[0]
        if magic == 0xEBEB:
            require(table[position + 2:position + 16] == b"\xff" * 14 and
                    table[position + 16:position + 32] == hashlib.md5(table[:position]).digest(),
                    "Partition MD5 record mismatch")
            require(table[position + 32:] == b"\xff" * (TABLE_BYTES - position - 32),
                    "Unexpected partition table trailer")
            break
        require(magic == 0x50AA, "Invalid partition table; first install is unsupported")
        _, kind, subtype, offset, size, label, flags = struct.unpack_from("<HBBII16sI", table, position)
        require(flags == 0, "Encrypted/flagged partitions are unsupported")
        entries.append((kind, subtype, offset, size, label.split(b"\0", 1)[0]))
    else:
        raise Refused("Partition table has no integrity record")
    require(entries == [
        (1, 2, 0x9000, 0x6000, b"nvs"),
        (1, 1, 0xF000, 0x1000, b"phy_init"),
        (0, 0, APP_OFFSET, APP_BYTES, b"factory"),
        (1, 0x82, 0xE10000, 0x800000, b"storage"),
        (1, 3, 0x1610000, 0x10000, b"coredump"),
    ], "Unsupported device layout; bootloader, NVS and storage will not be changed")


class EsptoolDevice:
    """One serial session from preflight to verification; imports never scan ports."""
    def __init__(self, port):
        import esptool
        require(esptool.__version__ in SUPPORTED_ESPTOOL,
                "Unsupported esptool version; use esptool 4.11.0 or reviewed IDF 4.12.dev1")
        self.esptool = esptool
        self.rom = self.esp = None
        self.serial = None
        self.port = port

    def __enter__(self):
        from esptool.cmds import detect_chip
        import serial
        # Own the handle even if chip detection raises before returning a
        # loader. A failed preflight must not strand an open serial session.
        self.serial = serial.Serial(port=None, baudrate=115200, timeout=3, exclusive=True)
        try:
            self.serial.port = self.port
            self.serial.rts = self.serial.dtr = False
            self.serial.open()
            self.rom = self.esp = detect_chip(self.serial, connect_attempts=3)
        except BaseException:
            self.serial.close()
            raise
        return self

    def __exit__(self, *error):
        if self.serial is not None:
            self.serial.close()

    def inspect(self):
        from esptool.cmds import detect_flash_size
        require(self.esp.CHIP_NAME == "ESP32-P4", "Wrong chip; expected ESP32-P4")
        security = self.esp.get_security_info()
        require(security.get("chip_id") == 18 and isinstance(security.get("flags"), int) and
                isinstance(security.get("flash_crypt_cnt"), int), "Security state unavailable")
        require(not self.esp.secure_download_mode and not security["flags"] & 5 and
                security["flash_crypt_cnt"] == 0 and not self.esp.get_secure_boot_enabled() and
                not self.esp.get_flash_encryption_enabled(), "Secure/encrypted device unsupported; nothing will be erased")
        self.esp = self.esp.run_stub()
        self.esp.flash_spi_attach(0)
        require(detect_flash_size(self.esp) == "32MB", "Physical flash is not 32MB")
        self.esp.flash_set_parameters(FLASH_BYTES)
        return self.esp.get_chip_description()

    def read(self, offset, size):
        return self.esp.read_flash(offset, size)

    def write_app(self, app):
        from esptool.cmds import write_flash
        binary = io.BytesIO(app)
        binary.name = "validated-app.bin"
        args = argparse.Namespace(compress=True, no_compress=False, no_stub=False,
            force=False, addr_filename=[(APP_OFFSET, binary)], encrypt=False,
            encrypt_files=None, ignore_flash_encryption_efuse_setting=False,
            flash_size="keep", flash_mode="keep", flash_freq="keep", chip="esp32p4",
            erase_all=False, verify=True)
        write_flash(self.esp, args)

    def reset(self):
        self.esp.hard_reset()


def flash_upgrade(manifest, app, port, confirm_board, factory=EsptoolDevice, prompt=input, output=print):
    require(confirm_board == BOARD, "Physically inspect board label and supply --confirm-board " + BOARD)
    require(isinstance(port, str) and port.strip(), "An explicit serial port is required")
    require(re.fullmatch(r"COM[1-9][0-9]*", port, re.IGNORECASE) is not None or
            (port.startswith("/dev/") and ".." not in port and ":" not in port),
            "Only local COM or /dev/ ports are supported; network serial URLs are refused")
    output("This resets the selected device. P4 chip detection does NOT identify its board model.")
    if prompt(f"Connect to {BOARD} on {port}? Type CONNECT: ").strip() != "CONNECT":
        output("Cancelled before opening the port.")
        return False
    with factory(port) as device:
        description = device.inspect()
        check_partitions(device.read(TABLE_OFFSET, TABLE_BYTES), manifest)
        old_version = descriptor(device.read(APP_OFFSET, 288))
        output(f"Checked {description}; existing LakeShark {old_version} -> {manifest['version']}.")
        output(f"Write only {len(app)} bytes at 0x10000. NVS/storage/bootloader untouched.")
        output("Factory-slot upgrade is NOT power-fail atomic. Keep power stable; back up settings separately.")
        if prompt("Type FLASH to replace the application, anything else cancels: ").strip() != "FLASH":
            output("Cancelled without flash writes. Reset the board to leave download mode.")
            return False
        device.write_app(app)
        check = hashlib.sha256()
        for offset in range(0, len(app), 65536):
            size = min(65536, len(app) - offset)
            data = device.read(APP_OFFSET + offset, size)
            require(len(data) == size, "Short verification read; keep device connected for recovery")
            check.update(data)
        require(check.hexdigest() == sha256(app), "Readback SHA256 failed; application may be incomplete")
        device.reset()
    output("Application readback verified and reset requested. Confirm screen, touch and audio on the board.")
    return True


def main(argv=None, factory=EsptoolDevice, prompt=input, output=print):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("package", help="Extracted validated application-only package directory")
    parser.add_argument("--board", required=True, choices=[BOARD_OPTION])
    parser.add_argument("--flash", action="store_true", help="Explicitly connect, preflight and offer application write")
    parser.add_argument("--port", help="Explicit serial port; never auto-discovered")
    parser.add_argument("--confirm-board", help="Exact physical board label; required with --flash")
    args = parser.parse_args(argv)
    try:
        require(args.flash or not (args.port or args.confirm_board), "Port/board confirmation requires explicit --flash")
        manifest, app = check_package(args.package, args.board)
        output(f"Package checks passed: {BOARD}, {manifest['version']}, {len(app)} application bytes.")
        output(str(manifest.get("distribution_status", "Distribution clearance not established.")))
        output("SHA256 proves integrity, not publisher authenticity. Use only a trusted package.")
        if not args.flash:
            output("CHECK ONLY: no serial port opened and no hardware accessed.")
            return 0
        return 0 if flash_upgrade(manifest, app, args.port, args.confirm_board, factory, prompt, output) else 1
    except KeyboardInterrupt:
        output("Cancelled. If writing had started, the application may be incomplete; do not assume it boots.")
        return 130
    except (Refused, OSError, ValueError, KeyError, TypeError, struct.error, ImportError, RuntimeError, EOFError) as error:
        output("REFUSED: " + str(error))
        if args.flash:
            output("If writing started, the application may be incomplete. Reset/recovery may be needed; no data partitions were targeted.")
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
