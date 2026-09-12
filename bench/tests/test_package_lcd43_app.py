"""No device/network access: synthetic app/ELF/partition files in temp repos."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import time
import unittest
import zipfile

SCRIPT = Path(__file__).resolve().parents[2] / 'tools/package_lcd43_app.py'
spec = importlib.util.spec_from_file_location('packager', SCRIPT)
packager = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packager)


class PackageTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ls-release-test-')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.root = self.base / 'source'
        self.root.mkdir()
        self.build = self.root / 'build'
        self.build.mkdir()
        self.out = self.base / 'package'
        (self.root / 'main').mkdir()
        (self.root / 'main/main.c').write_text('int main(void) { return 0; }')
        (self.root / 'LICENSE').write_text('Fixture license notice')
        (self.root / '.gitignore').write_text('build/\n')
        for command in (['init', '-q'], ['add', '.'], ['-c', 'user.name=Fixture', '-c', 'user.email=fixture@example.invalid', 'commit', '-qm', 'fixture']):
            subprocess.run(['git', '-C', str(self.root), *command], check=True, capture_output=True)
        self.license_list = self.base / 'license-list.json'
        self.license_list.write_text('["LICENSE"]')
        (self.build / 'sdkconfig').write_text('CONFIG_LS_BOARD_P4_TOUCH_LCD_43=y\nCONFIG_ESPTOOLPY_FLASHSIZE_32MB=y\nCONFIG_IDF_TARGET="esp32p4"\n')
        (self.build / 'config').mkdir()
        (self.build / 'config/sdkconfig.h').write_text('#define CONFIG_LS_BOARD_P4_TOUCH_LCD_43 1\n#define CONFIG_ESPTOOLPY_FLASHSIZE_32MB 1\n')
        elf = bytearray(64)
        elf[:7] = b'\x7fELF\x01\x01\x01'
        struct.pack_into('<H', elf, 18, 243)
        (self.build / 'app.elf').write_bytes(elf)
        body = bytearray(256)
        struct.pack_into('<I', body, 0, 0xABCD5432)
        body[16:21] = b'1.0.1'
        body[48:57] = b'lakeshark'
        body[112:118] = b'v5.5.4'
        body[144:176] = hashlib.sha256(elf).digest()
        body.extend(packager.BOARD.encode())
        image = bytearray(24)
        image[0], image[1], image[3], image[23] = 0xE9, 1, 0x50, 1
        struct.pack_into('<H', image, 12, 18)
        image.extend(struct.pack('<II', 0x40000000, len(body)))
        image.extend(body)
        checksum = 0xEF
        for byte in body:
            checksum ^= byte
        image.extend(b'\0' * (15 - len(image) % 16))
        image.append(checksum)
        image.extend(hashlib.sha256(image).digest())
        (self.build / 'app.bin').write_bytes(image)
        partition = struct.pack('<HBBII16sI', 0x50AA, 0, 0, 0x10000, 0xD00000, b'factory', 0)
        partition += b'\xeb\xeb' + b'\xff' * 14 + hashlib.md5(partition).digest()
        (self.build / 'partitions.bin').write_bytes(partition)
        self.desc = {'project_name': 'lakeshark', 'project_version': '1.0.1', 'target': 'esp32p4', 'project_path': str(self.root),
                     'build_dir': str(self.build), 'config_file': str(self.build / 'sdkconfig'), 'app_bin': 'app.bin', 'app_elf': 'app.elf'}
        self.args = {'extra_esptool_args': {'chip': 'esp32p4'}, 'flash_settings': {'flash_size': '32MB'},
                     'app': {'offset': '0x10000', 'file': 'app.bin', 'encrypted': 'false'},
                     'flash_files': {'0x10000': 'app.bin'}, 'partition-table': {'file': 'partitions.bin'}}
        self.metadata()
        later = time.time_ns() + 10_000_000_000
        os.utime(self.build / 'app.elf', ns=(later, later))

    def metadata(self):
        (self.build / 'project_description.json').write_text(json.dumps(self.desc))
        (self.build / 'flasher_args.json').write_text(json.dumps(self.args))

    def run_package(self):
        return packager.package(self.build, self.out, self.license_list)

    def test_exact_allowlist_dirty_source_and_hashes(self):
        (self.root / 'main/main.c').write_text('int main(void) { return 1; }')
        (self.build / 'nvs.bin').write_bytes(b'PRIVATE PASSWORD')
        (self.build / 'storage.bin').write_bytes(b'PRIVATE RF CAPTURE')
        (self.root / 'tools').mkdir()
        (self.root / 'tools/new-release-tool.py').write_text('# newer non-firmware tooling')
        self.run_package()
        files = {p.relative_to(self.out).as_posix() for p in self.out.rglob('*') if p.is_file()}
        self.assertEqual(files, {'app.bin', 'app.elf', 'manifest.json', 'SHA256SUMS.txt', 'FLASH_INSTRUCTIONS.txt', 'licenses/LICENSE'})
        manifest = json.loads((self.out / 'manifest.json').read_text())
        self.assertIn('NOT CLEARED', manifest['distribution_status'])
        self.assertIn('Do not publish', (self.out / 'FLASH_INSTRUCTIONS.txt').read_text())
        self.assertTrue(manifest['provenance']['dirty'])
        self.assertTrue(manifest['provenance']['source_input_fingerprint_sha256'])
        for line in (self.out / 'SHA256SUMS.txt').read_text().splitlines():
            checksum, name = line.split('  ', 1)
            self.assertEqual(checksum, packager.digest((self.out / name).read_bytes()))
        with self.assertRaisesRegex(ValueError, 'overwrite'):
            self.run_package()

    def test_mismatched_elf_refused(self):
        p = self.build / 'app.elf'
        p.write_bytes(p.read_bytes() + b'wrong')
        with self.assertRaisesRegex(ValueError, 'ELF SHA256 mismatch'):
            self.run_package()
        self.assertFalse(self.out.exists())

    def test_wrong_board_refused(self):
        (self.build / 'sdkconfig').write_text('CONFIG_LS_BOARD_P4_NANO=y')
        with self.assertRaisesRegex(ValueError, 'configuration'):
            self.run_package()

    def test_bad_offset_and_escape_refused(self):
        self.args['app']['offset'] = '0x9000'
        self.metadata()
        with self.assertRaisesRegex(ValueError, 'offset'):
            self.run_package()
        self.args['app']['offset'] = '0x10000'
        self.desc['app_bin'] = '../main/main.c'
        self.metadata()
        with self.assertRaisesRegex(ValueError, 'escaping'):
            self.run_package()

    def test_newer_source_refused(self):
        now = time.time_ns() + 20_000_000_000
        os.utime(self.root / 'main/main.c', ns=(now, now))
        with self.assertRaisesRegex(ValueError, 'newer than ELF'):
            self.run_package()

    def test_cmake_build_inputs_are_fingerprinted_and_newer_ones_refused(self):
        folder = self.root / 'cmake'
        folder.mkdir()
        for name in ('ls_build_identity.cmake', 'ls_refresh_identity.cmake'):
            (folder / name).write_text('# build identity input\n')
        provenance = packager.source_provenance(self.root, (self.build / 'app.elf').stat().st_mtime_ns)
        recorded = {entry['path'] for entry in provenance['source_inputs']}
        self.assertIn('cmake/ls_build_identity.cmake', recorded)
        self.assertIn('cmake/ls_refresh_identity.cmake', recorded)
        now = time.time_ns() + 20_000_000_000
        os.utime(folder / 'ls_refresh_identity.cmake', ns=(now, now))
        with self.assertRaisesRegex(ValueError, 'newer than ELF: cmake/ls_refresh_identity.cmake'):
            self.run_package()

    def test_cc_sources_are_fingerprinted_and_newer_ones_refused(self):
        source = self.root / 'components/imbe_vocoder/decode.cc'
        source.parent.mkdir(parents=True)
        source.write_text('// synthetic compiled vocoder input\n')
        provenance = packager.source_provenance(self.root, (self.build / 'app.elf').stat().st_mtime_ns)
        records = {entry['path']: entry for entry in provenance['source_inputs']}
        self.assertEqual(records['components/imbe_vocoder/decode.cc']['sha256'],
                         packager.digest(source.read_bytes()))
        now = time.time_ns() + 20_000_000_000
        os.utime(source, ns=(now, now))
        with self.assertRaisesRegex(ValueError, 'newer than ELF: components/imbe_vocoder/decode.cc'):
            self.run_package()
        self.assertFalse(self.out.exists())

    def test_partition_integrity_refused(self):
        p = self.build / 'partitions.bin'
        p.write_bytes(p.read_bytes()[:-1] + b'!')
        with self.assertRaisesRegex(ValueError, 'MD5 mismatch'):
            self.run_package()

    def test_corrupted_image_refused(self):
        p = self.build / 'app.bin'
        p.write_bytes(p.read_bytes()[:-1] + b'!')
        with self.assertRaisesRegex(ValueError, 'SHA256 mismatch'):
            self.run_package()

    def test_missing_artifact_refused(self):
        (self.build / 'app.elf').unlink()
        with self.assertRaisesRegex(ValueError, 'Missing'):
            self.run_package()

    def test_arbitrary_data_not_allowed_as_license(self):
        self.license_list.write_text('["main/main.c"]')
        with self.assertRaisesRegex(ValueError, 'license/notice'):
            self.run_package()
        self.assertFalse(self.out.exists())

    def test_no_empty_license_list_or_output_inside_source(self):
        self.license_list.write_text('[]')
        with self.assertRaisesRegex(ValueError, 'empty'):
            self.run_package()
        self.license_list.write_text('["LICENSE"]')
        with self.assertRaisesRegex(ValueError, 'outside'):
            packager.package(self.build, self.root / 'release', self.license_list)

    def test_encrypted_build_refused(self):
        p = self.build / 'sdkconfig'
        p.write_text(p.read_text() + 'CONFIG_SECURE_FLASH_ENC_ENABLED=y\n')
        with self.assertRaisesRegex(ValueError, 'encrypted'):
            self.run_package()

    def make_docs(self):
        (self.root / 'docs').mkdir()
        names = sorted(packager.USER_DOCS)
        for name in names:
            (self.root / name).write_text('# Reviewed local preview document\n')
        listing = self.base / 'docs-list.json'
        listing.write_text(json.dumps(names))
        return listing, names

    def test_fixed_docs_are_checksummed_and_zip_is_exact_and_deterministic(self):
        listing, names = self.make_docs()
        archive = self.base / 'preview.zip'
        packager.package(self.build, self.out, self.license_list, listing, archive)
        manifest = json.loads((self.out / 'manifest.json').read_text())
        self.assertEqual(manifest['documentation_files'], names)
        self.assertIn('NOT CLEARED', manifest['distribution_status'])
        files = {p.relative_to(self.out).as_posix(): p.read_bytes() for p in self.out.rglob('*') if p.is_file()}
        for line in files['SHA256SUMS.txt'].decode().splitlines():
            checksum, name = line.split('  ', 1)
            self.assertEqual(checksum, packager.digest(files[name]))
        with zipfile.ZipFile(archive) as z:
            self.assertEqual(z.namelist(), sorted(files))
            for name, data in files.items():
                self.assertEqual(z.read(name), data)
                self.assertEqual(z.getinfo(name).date_time, (1980, 1, 1, 0, 0, 0))
        second = self.base / 'same-content.zip'
        packager.write_zip(files, second)
        self.assertEqual(archive.read_bytes(), second.read_bytes())
        with self.assertRaises(FileExistsError):
            packager.write_zip(files, second)

    def test_docs_traversal_absolute_and_arbitrary_files_refused(self):
        listing, _ = self.make_docs()
        bad_paths = ['../private.md', 'docs/../main/main.c', 'main/main.c',
                     'docs/PRIVATE_CAPTURE.md', str(self.root / 'docs/LCD43_QUICKSTART.md'),
                     'docs\\LCD43_QUICKSTART.md']
        for name in bad_paths:
            listing.write_text(json.dumps([name]))
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'fixed release-doc allowlist'):
                packager.package(self.build, self.out, self.license_list, listing)
            self.assertFalse(self.out.exists())

    def test_missing_duplicate_and_binary_docs_refused(self):
        listing, names = self.make_docs()
        listing.write_text(json.dumps([names[0], names[0]]))
        with self.assertRaisesRegex(ValueError, 'Duplicate'):
            packager.package(self.build, self.out, self.license_list, listing)
        listing.write_text(json.dumps(names))
        (self.root / names[0]).write_bytes(b'\0capture')
        with self.assertRaisesRegex(ValueError, 'Binary'):
            packager.package(self.build, self.out, self.license_list, listing)
        (self.root / names[0]).unlink()
        with self.assertRaisesRegex(ValueError, 'Missing'):
            packager.package(self.build, self.out, self.license_list, listing)

    def test_zip_overwrite_and_inside_package_refused_before_output(self):
        archive = self.base / 'existing.zip'
        archive.write_bytes(b'preserve')
        with self.assertRaisesRegex(ValueError, 'refusing overwrite'):
            packager.package(self.build, self.out, self.license_list, zip_output=archive)
        self.assertEqual(archive.read_bytes(), b'preserve')
        self.assertFalse(self.out.exists())
        with self.assertRaisesRegex(ValueError, 'outside'):
            packager.package(self.build, self.out, self.license_list, zip_output=self.out / 'nested.zip')
        self.assertFalse(self.out.exists())


if __name__ == '__main__':
    unittest.main()
