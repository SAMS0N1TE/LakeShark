"""Build, sign and publish store apps into a site folder.

    python app-sdk/catalog.py --key STORE.pem --site SITE_DIR [app-sdk/apps/<name> ...]

Each app folder has its C files and an app.json with "category",
"description" and, for work in progress, "status": "wip". Writes SITE_DIR/apps/files/<id>-<version>.lap and rewrites
SITE_DIR/apps/catalog.json from what the files themselves say, so the store
never lists a name, size or checksum the board would disagree with.
"""
import argparse, hashlib, json, os, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import lap  # noqa: E402

CATEGORIES = {'games', 'tools', 'radio', 'maps', 'utilities'}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--key', required=True, help='store signing key (PEM, outside any repository)')
    ap.add_argument('--site', required=True, help='site folder; apps/ is written inside it')
    ap.add_argument('--min-firmware', default='2.18.0')
    ap.add_argument('apps', nargs='*')
    a = ap.parse_args()
    here = os.path.dirname(os.path.abspath(__file__))
    dirs = a.apps or sorted(os.path.join(here, 'apps', d) for d in os.listdir(os.path.join(here, 'apps')))
    out_dir = os.path.join(a.site, 'apps', 'files')
    os.makedirs(out_dir, exist_ok=True)
    entries = []
    with tempfile.TemporaryDirectory() as tmp:
        for d in dirs:
            meta = json.load(open(os.path.join(d, 'app.json'), encoding='utf-8'))
            if meta.get('category') not in CATEGORIES:
                sys.exit('%s: category must be one of %s' % (d, sorted(CATEGORIES)))
            built = os.path.join(tmp, os.path.basename(d) + '.lap')
            lap.build(d, built)
            lap.sign(built, a.key)
            data = open(built, 'rb').read()
            m = lap.Elf(data).manifest
            if not m.get('signature_present'):
                sys.exit('%s: signing did not take' % d)
            name = '%s-%s.lap' % (m['id'], m['version'])
            open(os.path.join(out_dir, name), 'wb').write(data)
            entries.append({
                'id': m['id'], 'name': m['name'], 'sub': m['sub'], 'version': m['version'],
                'author': m['author'], 'category': meta['category'], 'color': m['color'],
                'icon_art': m['icon_art'], 'description': meta.get('description', ''),
                'size': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'signed': True,
                'file': 'apps/files/' + name, 'min_firmware': a.min_firmware,
            })
            if meta.get('status') == 'wip':      # the store badges it EARLY ACCESS
                entries[-1]['status'] = 'wip'
            print('%-8s %-6s %6d B  %s' % (m['id'], m['version'], len(data), name))
    cat = {'schema': 1, 'generated': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), 'apps': entries}
    open(os.path.join(a.site, 'apps', 'catalog.json'), 'w', encoding='utf-8', newline='\n').write(json.dumps(cat, indent=1) + '\n')
    print('catalog: %d apps' % len(entries))


if __name__ == '__main__':
    main()
