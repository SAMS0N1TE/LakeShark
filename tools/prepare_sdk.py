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
    head = git('rev-parse', 'HEAD')
    if head not in (pin['base_commit'], pin['commit']):
        raise SystemExit('Expected the pinned ESP-IDF 5.4.3 base or LakeShark SDK revision')
    git('bundle', 'verify', str(bundle))
    git('fetch', str(bundle), 'HEAD')
    git('checkout', '--detach', pin['commit'])
    source = (args.idf / pin['file']).read_bytes().replace(b'\r\n', b'\n')
    if hashlib.sha256(source).hexdigest() != pin['file_sha256']:
        raise SystemExit('SDK source checksum mismatch')
    print('SDK ready:', pin['commit'])

if __name__ == '__main__':
    main()
