"""Package a clean, locally built T-Display P4 application update."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
import zipfile

ROOT = Path(__file__).resolve().parents[2]


def git(*args):
    return subprocess.check_output(['git', '-C', str(ROOT), *args], text=True).strip()


def validate(config, metadata, image, revision, version):
    required = ('CONFIG_LS_BOARD_T_DISPLAY_P4=y',
                'CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y',
                'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="boards/partitions_16m.csv"')
    if any(line not in config.splitlines() for line in required):
        raise ValueError('Expected T-Display P4 with the existing 16 MB layout')
    identity = version + '-g' + revision[:12]
    if metadata.get('project_version') != identity:
        raise ValueError('Build metadata does not match the clean source revision')
    # ESP image header (24), first segment header (8), then esp_app_desc.
    if len(image) < 256 or image[0] != 0xe9 or image[32:36] != bytes.fromhex('3254cdab'):
        raise ValueError('Missing ESP application descriptor')
    actual = image[48:80].split(b'\0', 1)[0].decode('ascii')
    if actual != identity:
        raise ValueError('Firmware version does not match build metadata')
    if len(image) > 0x900000:
        raise ValueError('Application exceeds the existing app partition')
    return identity


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--version', default='2.1.0-rc1')
    parser.add_argument('--build', type=Path, default=ROOT/'build_tdp4')
    parser.add_argument('--output', type=Path, default=ROOT/'release-artifacts')
    args = parser.parse_args()
    if git('status', '--porcelain'):
        raise SystemExit('Commit or resolve all source changes before packaging')
    revision = git('rev-parse', 'HEAD')
    build = args.build.resolve()
    config = (build/'sdkconfig').read_text()
    metadata = json.loads((build/'project_description.json').read_text())
    app = (build/'lakeshark.bin').read_bytes()
    identity = validate(config, metadata, app, revision, args.version)
    prefix = 'lakeshark-tdp4-' + args.version
    out = args.output.resolve()/prefix
    out.mkdir(parents=True, exist_ok=False)
    source = out/(prefix+'-source.zip')
    subprocess.run(['git','-C',str(ROOT),'archive','--format=zip',
                    '--prefix='+prefix+'-source/','-o',str(source),revision],check=True)
    # Pin dependencies needed when rebuilding the source archive.
    with zipfile.ZipFile(source,'a',compression=zipfile.ZIP_DEFLATED) as archive:
        archive.write(ROOT/'dependencies.lock',prefix+'-source/dependencies.lock')
    readme = f'''LakeShark {args.version} — T-Display P4 application update

Source revision: {revision}
Firmware identity: {identity}
Toolchain: ESP-IDF 5.4.3; see build-sdkconfig for the exact configuration.

For an EXISTING T-Display P4 installation using boards/partitions_16m.csv
and matching ESP-Hosted C6 firmware. This is not a Nano, LCD-4.3 or C6 image.
Check the SHA256SUMS file before flashing. Replace YOUR_PORT with the display port:

python -m esptool --chip esp32p4 --port YOUR_PORT --baud 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB 0x10000 lakeshark.bin

This command updates the app only. It does not erase NVS or SD, change partitions,
or install C6 firmware. For first installation, build the included source using
docs/TDP4_FIRST_FLASH.md. Do not use this app-only zip for a different layout.

Read RELEASE_NOTES.md and VALIDATION.md for tested features and experimental limits.
Keep the paired source archive; it includes license notices and dependency pins.
Nothing in this package has been pushed or published by the packaging tool.
'''
    manifest = {'version':args.version,'identity':identity,'source_revision':revision,
                'board':'T-Display-P4','flash_size':16777216,'app_offset':'0x10000',
                'app_size':len(app),'app_sha256':hashlib.sha256(app).hexdigest(),
                'partition_sha256':hashlib.sha256((build/'partition_table/partition-table.bin').read_bytes()).hexdigest(),
                'idf_version':metadata.get('idf_ver'),'package_kind':'app-only'}
    payload = {'lakeshark.bin':app,'README.txt':readme.encode(),
               'manifest.json':(json.dumps(manifest,indent=2)+'\n').encode(),
               'build-sdkconfig':config.encode(),
               'RELEASE_NOTES.md':(ROOT/'docs/TDP4_RELEASE_NOTES.md').read_bytes(),
               'VALIDATION.md':(ROOT/'docs/TDP4_RC_CHECKLIST.md').read_bytes(),
               'THIRD_PARTY_NOTICES.md':(ROOT/'docs/THIRD_PARTY_NOTICES.md').read_bytes(),
               'LICENSE':(ROOT/'LICENSE').read_bytes()}
    package = out/(prefix+'-app.zip')
    with zipfile.ZipFile(package,'w',compression=zipfile.ZIP_DEFLATED) as archive:
        for name,data in payload.items(): archive.writestr(name,data)
    for item in (source,package):
        with zipfile.ZipFile(item) as archive:
            if archive.testzip(): raise ValueError('Archive CRC validation failed')
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2)+'\n')
    (out/'SHA256SUMS').write_text(''.join(hashlib.sha256(p.read_bytes()).hexdigest()+'  '+p.name+'\n'
                                         for p in (source,package,out/'manifest.json')))
    print(out)
    print(identity)


if __name__ == '__main__':
    main()
