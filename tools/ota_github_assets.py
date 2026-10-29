"""Copies of the OTA site's manifests and images as GitHub release assets.

A board that reaches neither update host (ota.terminalbay.com,
lakeshark-ota.pages.dev) looks for the same files on the newest GitHub
release, named after their path with '/' turned into '-'
(ls_ota_github_url in components/lakeshark/core/ls_ota_core.c):

    tdp4/stable.txt            -> tdp4-stable.txt
    tdp4/2.18.0/lakeshark.bin  -> tdp4-2.18.0-lakeshark.bin

Run it on the folder that is published to the update hosts, after the
manifests are signed, and attach what it writes to the release that GitHub
calls "latest" (the newest one that is not a pre-release):

    python tools/ota_github_assets.py <site-dir> <out-dir> [--channel stable ...] [--upload v2.18.0]

The files are copied byte for byte: the board checks the manifest's
signature and the image's SHA-256 exactly as it does on the hosts. Each
image is checked against its manifest here too, so a stale copy is caught
before it is uploaded.
"""
import argparse, hashlib, posixpath, shutil, subprocess, sys
from pathlib import Path

HOSTS = ('ota.terminalbay.com', 'lakeshark-ota.pages.dev')
MAGIC = 'LAKESHARK-OTA 1'
REPO = 'SAMS0N1TE/LakeShark'


def flat(path):
    return '-'.join(p for p in path.split('/') if p)


def fields(text):
    out = {}
    for line in text.splitlines():
        if '=' in line:
            k, v = line.split('=', 1)
            out.setdefault(k, v)
    return out


def image_path(manifest_rel, url):
    """The image's path on the site, as the board resolves url= against the manifest."""
    if url.startswith('https://'):
        host, _, path = url[len('https://'):].partition('/')
        if host not in HOSTS:
            raise ValueError('image on %s, not an update host: GitHub cannot stand in for it' % host)
        return path.split('?')[0].split('#')[0]
    if url.startswith('/'):
        return url[1:]
    return posixpath.normpath(posixpath.join(posixpath.dirname(manifest_rel), url))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('site', type=Path, help='the folder published to the update hosts')
    ap.add_argument('out', type=Path, help='where the flat copies go')
    ap.add_argument('--channel', action='append', help='manifest names to copy (default: stable)')
    ap.add_argument('--upload', metavar='TAG', help='attach them to this release with gh (replacing older copies)')
    a = ap.parse_args()
    channels = a.channel or ['stable']
    a.out.mkdir(parents=True, exist_ok=True)
    written = []
    for board in sorted(p for p in a.site.iterdir() if p.is_dir()):
        for ch in channels:
            m = board / (ch + '.txt')
            if not m.is_file():
                continue
            rel = '%s/%s.txt' % (board.name, ch)
            text = m.read_text(encoding='utf-8')
            if not text.startswith(MAGIC):
                sys.exit('%s: not an update manifest' % rel)
            f = fields(text)
            img_rel = image_path(rel, f['url'])
            img = a.site / img_rel
            data = img.read_bytes()
            if len(data) != int(f['size']) or hashlib.sha256(data).hexdigest() != f['sha256'].lower():
                sys.exit('%s: does not match %s (size or sha256)' % (img_rel, rel))
            for src, name in ((m, flat(rel)), (img, flat(img_rel))):
                shutil.copyfile(src, a.out / name)
                written.append(a.out / name)
                print('%s -> %s' % (src.relative_to(a.site).as_posix(), name))
    if not written:
        sys.exit('no %s manifests under %s' % ('/'.join(channels), a.site))
    if a.upload:
        subprocess.run(['gh', 'release', 'upload', a.upload, '--clobber', '--repo', REPO, *map(str, written)], check=True)


if __name__ == '__main__':
    main()
