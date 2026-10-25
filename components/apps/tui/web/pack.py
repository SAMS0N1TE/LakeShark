"""The web screen's page as the board serves it: screen.html with every
<script src="..."> beside it put inline, gzipped. Run by the build.

    python pack.py screen.html out/screen.html.gz
"""
import gzip
import os
import re
import sys


def main(src, out):
    here = os.path.dirname(os.path.abspath(src))
    with open(src, encoding='utf-8') as f:
        html = f.read()

    def inline(m):
        with open(os.path.join(here, m.group(1)), encoding='utf-8') as f:
            body = f.read()
        if '</script' in body:
            raise SystemExit(f'{m.group(1)} says </script, which would end its own tag')
        return '<script>\n' + body + '\n</script>'

    html = re.sub(r'<script src="([\w.-]+)"></script>', inline, html)
    data = html.encode('utf-8')
    # mtime 0: the same page gives the same bytes, and the same ETag.
    with open(out, 'wb') as f:
        f.write(gzip.compress(data, compresslevel=9, mtime=0))


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
