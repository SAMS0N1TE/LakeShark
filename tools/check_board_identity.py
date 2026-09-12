"""Reject board identity tokens outside board/, ignoring C/C++ comments/literals."""
from pathlib import Path
import re
import sys

NON_CODE = re.compile(
    r'R"([^\s()\\]{0,16})\([\s\S]*?\)\1"'
    r'|"(?:\\[\s\S]|[^"\\])*"'
    r"|'(?:\\[\s\S]|[^'\\])*'"
    r'|//[^\n]*|/\*[\s\S]*?(?:\*/|\Z)'
)
IDENTITY = re.compile(r'\bCONFIG_LS_BOARD_\w*\b')
BOARD = Path('components/lakeshark/board')


def references(source):
    # C line splicing happens before comments are recognized. Keep a map back
    # to physical lines so diagnostics still point at the original source.
    chars, lines = [], []
    line, pos = 1, 0
    while pos < len(source):
        splice = re.match(r'\\\r?\n', source[pos:pos + 3])
        if splice:
            pos += len(splice[0])
            line += 1
            continue
        chars.append(source[pos])
        lines.append(line)
        line += source[pos] == '\n'
        pos += 1
    code = NON_CODE.sub(lambda m: ' ' * len(m[0]), ''.join(chars))
    return [(lines[m.start()], m[0]) for m in IDENTITY.finditer(code)]


def check(root):
    hits = []
    for name in ('components', 'main'):
        folder = root / name
        if not folder.is_dir():
            raise FileNotFoundError(folder)
        for path in sorted(folder.rglob('*')):
            relative = path.relative_to(root)
            if relative.is_relative_to(BOARD) or path.suffix not in {'.c', '.h', '.cpp', '.cc', '.cxx', '.hpp', '.hh', '.hxx'}:
                continue
            if path.is_file():
                # Vendor C sources can contain non-UTF-8 comments. Preserve
                # undecodable bytes without hiding any ASCII macro tokens.
                for line, token in references(path.read_text(encoding='utf-8', errors='surrogateescape')):
                    hits.append(f'{relative.as_posix()}:{line}: {token}')
    return hits


if __name__ == '__main__':
    hits = check(Path(__file__).resolve().parents[1])
    print('\n'.join(hits) if hits else 'OK: no board-identity tokens outside board/')
    if hits:
        print('Use a capability from board/ls_caps.h instead of board identity.')
    sys.exit(bool(hits))
