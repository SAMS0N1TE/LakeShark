"""Local-only LCD4.3 application upgrade candidate; never flashes or erases."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import zipfile
from datetime import datetime, timezone

BOARD = "ESP32-P4-WIFI6-Touch-LCD-4.3"
USER_DOCS = {
    "docs/LCD43_RELEASE_NOTES.md",
    "docs/LCD43_QUICKSTART.md",
    "docs/LCD43_TROUBLESHOOTING.md",
    "docs/THIRD_PARTY_REVIEW.md",
}


def user_documents(root, docs_list):
    if docs_list is None:
        return {}
    names = json.loads(Path(docs_list).read_text())
    require(isinstance(names, list) and names, "Documentation allowlist must be a nonempty JSON array")
    require(all(isinstance(name, str) and name in USER_DOCS for name in names),
            "Documentation path is not in the fixed release-doc allowlist")
    require(len(names) == len(set(names)), "Duplicate documentation path")
    files = {}
    for name in names:
        path = contained(root, name)
        require(path == root / name, "Documentation symlink refused")
        require(path.stat().st_size <= 256 * 1024, "Oversized release documentation")
        data = path.read_bytes()
        require(b'\0' not in data, "Binary release documentation refused")
        data.decode('utf-8')
        files[name] = data
    return files


def write_zip(files, destination):
    """Deterministic archive of the package allowlist, never a directory walk."""
    with zipfile.ZipFile(destination, mode='x', compression=zipfile.ZIP_STORED) as archive:
        for name in sorted(files):
            entry = zipfile.ZipInfo(name, date_time=(1980, 1, 1, 0, 0, 0))
            entry.create_system = 3
            entry.external_attr = 0o100644 << 16
            entry.compress_type = zipfile.ZIP_STORED
            archive.writestr(entry, files[name])


def require(ok, message):
    if not ok:
        raise ValueError(message)


def digest(data):
    return hashlib.sha256(data).hexdigest()


def contained(root, name):
    p = (root / name).resolve()
    require(p.is_relative_to(root) and p.is_file(), f"Missing/escaping artifact: {name}")
    return p


def image_info(app, elf):
    require(len(app) >= 288 and app[0] == 0xE9, "Not an ESP application image")
    require(struct.unpack_from("<H", app, 12)[0] == 18, "Not ESP32-P4 image")
    require(app[3] >> 4 == 5, "Image header is not 32MB")
    require(app[23] == 1, "Image lacks appended SHA256 integrity digest")
    require(1 <= app[1] <= 16, "Invalid segment count")
    pos, checksum = 24, 0xEF
    for _ in range(app[1]):
        require(pos + 8 <= len(app), "Truncated segment header")
        length = struct.unpack_from("<I", app, pos + 4)[0]
        pos += 8
        require(pos + length <= len(app), "Truncated segment")
        for byte in app[pos:pos + length]:
            checksum ^= byte
        pos += length
    checksum_at = pos + (15 - pos % 16)
    require(checksum_at + 33 == len(app), "Unexpected image trailer/length")
    require(app[checksum_at] == checksum, "Image checksum mismatch")
    require(hashlib.sha256(app[:checksum_at + 1]).digest() == app[-32:], "Image SHA256 mismatch")
    require(struct.unpack_from("<I", app, 32)[0] == 0xABCD5432, "Missing app descriptor")
    require(len(elf) >= 52 and elf[:7] == b"\x7fELF\x01\x01\x01" and
            struct.unpack_from("<H", elf, 18)[0] == 243, "Not a 32-bit RISC-V ELF")
    require(app[176:208] == hashlib.sha256(elf).digest(), "Application/ELF SHA256 mismatch")
    require(BOARD.encode() in app, "LCD4.3 board identity missing from application")
    string = lambda a, b: app[a:b].split(b"\0", 1)[0].decode("utf-8", errors="strict")
    require(string(80, 112) == "lakeshark", "Wrong application project")
    return {"version": string(48, 80), "idf_version": string(144, 176)}


def source_provenance(root, elf_mtime):
    def git(*args):
        return subprocess.check_output(["git", "-C", str(root), *args])
    require(Path(git("rev-parse", "--show-toplevel").decode().strip()).resolve() == root,
            "Project path is not a Git worktree root")
    names = git("ls-files", "-z", "--cached", "--others", "--exclude-standard").decode().split("\0")
    suffixes = {".c", ".cc", ".cpp", ".h", ".hpp", ".s", ".S", ".inc", ".ld", ".cmake", ".defaults", ".csv", ".yml", ".yaml", ".lock"}
    records = []
    for name in sorted(set(filter(None, names))):
        path = Path(name)
        if path.parts[0] not in {"main", "components", "boards", "cmake"} and len(path.parts) != 1:
            continue
        if path.suffix not in suffixes and path.name not in {"CMakeLists.txt", "Kconfig", "Kconfig.projbuild", "dependencies.lock"}:
            continue
        p = (root / path).resolve()
        require(p.is_relative_to(root), "Source symlink escapes worktree")
        if not p.exists():
            records.append({"path": name, "deleted": True})
            continue
        require(p.stat().st_mtime_ns <= elf_mtime, f"Source newer than ELF: {name}; rebuild first")
        records.append({"path": name, "sha256": digest(p.read_bytes())})
    require(records, "No source inputs fingerprinted")
    encoded = json.dumps(records, sort_keys=True, separators=(",", ":")).encode()
    return {
        "head": git("rev-parse", "HEAD").decode().strip(),
        "dirty": bool(git("status", "--porcelain", "--untracked-files=all")),
        "source_input_fingerprint_sha256": digest(encoded),
        "source_inputs": records,
        "method": "SHA256 inventory of tracked and untracked nonignored source inputs in main/components/boards/cmake and root build files; deleted entries retained. No source contents or raw patches bundled.",
        "limitation": "Packaging-time source fingerprint, not proof of a reproducible build or of ignored/vendor/toolchain inputs. ELF/app correspondence is cryptographically verified. HEAD alone does not identify dirty source. Retain the exact local source worktree separately.",
    }


def package(build_dir, output, license_list, docs_list=None, zip_output=None):
    build, output = Path(build_dir).resolve(), Path(output).resolve()
    require(build.is_dir(), "Build directory does not exist")
    require(not output.exists(), "Output already exists; refusing overwrite")
    desc_path = contained(build, "project_description.json")
    desc = json.loads(desc_path.read_text())
    require(desc.get("target") == "esp32p4" and desc.get("project_name") == "lakeshark", "Wrong target/project")
    require(Path(desc["build_dir"]).resolve() == build, "Build metadata belongs to another directory")
    root = Path(desc["project_path"]).resolve()
    require(not output.is_relative_to(build) and not output.is_relative_to(root), "Output must be outside source/build tree")
    archive_path = Path(zip_output).resolve() if zip_output is not None else None
    if archive_path is not None:
        require(not archive_path.exists(), "ZIP output already exists; refusing overwrite")
        require(archive_path.suffix.lower() == '.zip', "ZIP output must have .zip suffix")
        require(not archive_path.is_relative_to(root) and not archive_path.is_relative_to(build)
                and not archive_path.is_relative_to(output), "ZIP must be outside source/build/package directories")
    sdk_path = contained(build, "sdkconfig")
    require(Path(desc["config_file"]).resolve() == sdk_path, "Configuration path mismatch")
    sdk = sdk_path.read_text()
    for setting in ('CONFIG_LS_BOARD_P4_TOUCH_LCD_43=y', 'CONFIG_ESPTOOLPY_FLASHSIZE_32MB=y', 'CONFIG_IDF_TARGET="esp32p4"'):
        require(setting in sdk.splitlines(), f"Missing required configuration: {setting}")
    require('CONFIG_LAKESHARK_HEADLESS=y' not in sdk.splitlines(), "Headless image refused")
    require(not any(x in sdk.splitlines() for x in ('CONFIG_SECURE_FLASH_ENC_ENABLED=y', 'CONFIG_SECURE_BOOT=y')),
            "Secure boot/encrypted build requires a separate upgrade procedure")
    enabled_boards = [x for x in sdk.splitlines() if x.startswith('CONFIG_LS_BOARD_') and x.endswith('=y')]
    require(enabled_boards == ['CONFIG_LS_BOARD_P4_TOUCH_LCD_43=y'], "Ambiguous board configuration")
    header_path = contained(build, "config/sdkconfig.h")
    header = header_path.read_text()
    require('#define CONFIG_LS_BOARD_P4_TOUCH_LCD_43 1' in header and '#define CONFIG_ESPTOOLPY_FLASHSIZE_32MB 1' in header,
            "Generated configuration header mismatch")
    require('#define CONFIG_LAKESHARK_HEADLESS 1' not in header, "Generated header is headless")
    app_path, elf_path = contained(build, desc["app_bin"]), contained(build, desc["app_elf"])
    app, elf = app_path.read_bytes(), elf_path.read_bytes()
    info = image_info(app, elf)
    require(info["version"] == desc["project_version"], "Version metadata mismatch")
    args_path = contained(build, "flasher_args.json")
    args = json.loads(args_path.read_text())
    require(args["extra_esptool_args"]["chip"] == "esp32p4" and args["flash_settings"]["flash_size"] == "32MB", "Flash metadata mismatch")
    require(contained(build, args["app"]["file"]) == app_path and args["app"].get("encrypted") == "false", "Unexpected/encrypted app")
    offset = int(args["app"]["offset"], 0)
    require(offset == 0x10000 and args["flash_files"].get("0x10000") == args["app"]["file"], "Unexpected application offset")
    partition_path = contained(build, args["partition-table"]["file"])
    partitions = partition_path.read_bytes()
    entries, md5_seen = [], False
    for pos in range(0, len(partitions) - 31, 32):
        magic, kind, subtype, address, size = struct.unpack_from("<HBBII", partitions, pos)
        if magic == 0x50AA:
            entries.append((kind, subtype, address, size))
        elif magic == 0xEBEB:
            require(partitions[pos + 16:pos + 32] == hashlib.md5(partitions[:pos]).digest(), "Partition table MD5 mismatch")
            md5_seen = True
            break
        else:
            break
    require(md5_seen, "Partition table integrity record missing")
    ordered = sorted(entries, key=lambda e: e[2])
    require(all(a[2] + a[3] <= b[2] for a, b in zip(ordered, ordered[1:])), "Overlapping partitions")
    apps = [e for e in entries if e[0] == 0]
    require(len(apps) == 1 and apps[0][1] == 0 and apps[0][2] == offset, "Expected one factory application partition")
    require(len(app) <= apps[0][3] and all(e[2] + e[3] <= 32 * 1024 * 1024 for e in entries), "Image/partitions exceed capacity")
    for meta in (sdk_path, header_path, partition_path):
        require(meta.stat().st_mtime_ns <= elf_path.stat().st_mtime_ns, f"Build metadata newer than ELF: {meta.name}")
    provenance = source_provenance(root, elf_path.stat().st_mtime_ns)
    doc_files = user_documents(root, docs_list)
    licenses = json.loads(Path(license_list).read_text())
    require(isinstance(licenses, list) and licenses, "Required license path list is empty")
    license_files = {}
    for name in licenses:
        require(isinstance(name, str) and (any(word in Path(name).name.upper() for word in ("LICENSE", "LICENCE", "COPYING", "NOTICE")) or Path(name).name.upper() in {"RPSL.TXT", "RCSL.TXT"}), "License allowlist may contain only license/notice filenames")
        path = contained(root, name)
        relative = path.relative_to(root).as_posix()
        require(path.stat().st_size <= 1024 * 1024, "Oversized license notice")
        license_files["licenses/" + relative] = path.read_bytes()
    manifest = {"schema": 1, "kind": "application-only-upgrade-candidate", "board": BOARD,
        "distribution_status": "LOCAL PREVIEW ONLY - NOT CLEARED FOR PUBLIC DISTRIBUTION",
        "known_release_blockers": ["SAM license status unresolved", "Font provenance unresolved", "Complete third-party notice and corresponding-source review required"],
        "flash_bytes": 33554432, "app_offset": "0x10000", "app_partition_bytes": apps[0][3],
        "created_utc": datetime.now(timezone.utc).isoformat(), **info,
        "app_sha256": digest(app), "elf_sha256": digest(elf),
        "sdkconfig_sha256": digest(sdk_path.read_bytes()), "partition_table_sha256": digest(partitions),
        "license_files": sorted(license_files),
        "documentation_files": sorted(doc_files),
        "license_scope": "Explicit allowlist supplied by packager; inclusion is NOT distribution authorization. Tool does not certify license completeness or fulfill source-code obligations.",
        "provenance": provenance,
        "excludes": ["bootloader", "partition table", "NVS", "storage", "coredumps", "SD contents", "credentials", "RF captures"]}
    instructions = f"""LCD4.3 APPLICATION-ONLY UPGRADE CANDIDATE
LOCAL PREVIEW ONLY - NOT CLEARED FOR PUBLIC DISTRIBUTION
SAM licensing and font provenance remain unresolved. Including notices does
not grant distribution rights. Do not publish this package.

Only for {BOARD}, 32MB flash, with the EXISTING compatible factory app
partition at 0x10000 (size {apps[0][3]} bytes). This is not initial provisioning.
Confirm physical board identity, port, flash size and existing partition layout
before writing. If any differ, STOP. Back up device data separately and privately.
If device flash encryption or secure boot is enabled, STOP; this procedure does
not support secure/encrypted provisioning or upgrades.
This package contains no bootloader, partition table, NVS, storage or RF data.

Verify SHA256SUMS.txt for every file before use. In an ESP-IDF environment,
after confirming the port, the application-only command is:

python -m esptool --chip esp32p4 --port <CONFIRMED_PORT> write_flash 0x10000 app.bin

Do NOT use erase_flash, erase_region, --erase-all, full flash, or write any other
offset. Normal write_flash erases only sectors occupied by the application;
it does not erase the full chip or data partitions. Keep power stable: this
single factory partition upgrade is not atomic/power-fail safe.

Keep app.elf for this exact app.bin for offline crash decoding. No hardware
validation is implied by packaging. See manifest.json for hashes and honest
dirty-source provenance limitations. This tool does not flash or access ports.
An audited license list does not itself fulfill source-distribution obligations;
complete the license/source compliance review before public distribution.
"""
    files = {"app.bin": app, "app.elf": elf, "manifest.json": (json.dumps(manifest, indent=2) + "\n").encode(),
             "FLASH_INSTRUCTIONS.txt": instructions.encode()}
    files.update(license_files)
    files.update(doc_files)
    files["SHA256SUMS.txt"] = "".join(f"{digest(files[name])}  {name}\n" for name in sorted(files)).encode('utf-8')
    output.mkdir(parents=True, exist_ok=False)
    for name, data in files.items():
        (output / name).parent.mkdir(parents=True, exist_ok=True)
        (output / name).write_bytes(data)
    if archive_path is not None:
        archive_path.parent.mkdir(parents=True, exist_ok=True)
        write_zip(files, archive_path)
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--license-list", required=True, help="JSON array of source-relative license/notice paths; NOT distribution authorization")
    parser.add_argument("--docs-list", help="JSON array selecting the four fixed release-document paths")
    parser.add_argument("--zip", dest="zip_output", help="Optional new ZIP outside source/build/package; allowlist only")
    options = parser.parse_args()
    try:
        print(package(options.build_dir, options.output, options.license_list, options.docs_list, options.zip_output))
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError, struct.error) as error:
        parser.exit(2, f"REFUSED: {error}\n")
