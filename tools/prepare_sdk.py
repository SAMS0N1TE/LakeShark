"""Select the recorded SDK revision without discarding local changes."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--idf', required=True, type=Path)
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1] / 'sdk' / 'esp-idf-5.4.3'
    pin = json.loads((root / 'provenance.json').read_text())
    bundle = root / 'p4-reset.bundle'
    if hashlib.sha256(bundle.read_bytes()).hexdigest() != pin['bundle_sha256']:
        raise SystemExit('SDK bundle checksum mismatch')
    def git(*cmd):
        return subprocess.check_output(['git', '-C', str(args.idf), *cmd], text=True).strip()
    if git('status', '--porcelain', '--untracked-files=no'):
        raise SystemExit('SDK has tracked changes; preserve or commit them before selecting the release SDK')
    git('bundle', 'verify', str(bundle))
    git('fetch', str(bundle), 'HEAD')
    # The pinned base, the recorded revision, or an earlier LakeShark SDK
    # revision on the way to it.
    head = git('rev-parse', 'HEAD')
    on_path = subprocess.run(['git', '-C', str(args.idf), 'merge-base', '--is-ancestor',
                              pin['base_commit'], head]).returncode == 0 and \
              subprocess.run(['git', '-C', str(args.idf), 'merge-base', '--is-ancestor',
                              head, pin['commit']]).returncode == 0
    if not on_path:
        raise SystemExit('Expected the pinned ESP-IDF 5.4.3 base or LakeShark SDK revision')
    git('checkout', '--detach', pin['commit'])
    files = pin.get('files') or [{'file': pin['file'], 'file_sha256': pin['file_sha256']}]
    for entry in files:
        source = (args.idf / entry['file']).read_bytes().replace(b'\r\n', b'\n')
        if hashlib.sha256(source).hexdigest() != entry['file_sha256']:
            raise SystemExit('SDK source checksum mismatch: ' + entry['file'])
    print('SDK ready:', pin['commit'])

if __name__ == '__main__':
    main()
