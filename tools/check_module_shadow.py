"""Reject root modules that shadow the Python standard library."""
from pathlib import Path
import os
import subprocess
import sys


def git(*args):
    out = subprocess.run(['git', *args], capture_output=True, text=True)
    if out.returncode != 0:
        return []
    return [line for line in out.stdout.splitlines() if line.strip()]


def main():
    root = Path(__file__).resolve().parent.parent

    try:
        std = set(sys.stdlib_module_names)
    except AttributeError:
        # Python < 3.10. The check is advisory rather than absent: the names
        # below are the ones a tool is actually likely to import by accident.
        std = {'cmd', 'code', 'copy', 'glob', 'io', 'json', 'logging', 'math',
               'os', 'platform', 'queue', 're', 'select', 'signal', 'socket',
               'string', 'struct', 'subprocess', 'sys', 'time', 'token',
               'types', 'typing', 'uuid'}

    bad = []
    for f in git('ls-files', '*.py'):
        p = Path(f)
        # Root only - see the docstring.
        if len(p.parts) != 1:
            continue
        name = p.stem
        if name in std:
            bad.append((f, name))

    if not bad:
        return 0

    print('a tracked module at the repository root shadows a standard library '
          'module:')
    for f, name in bad:
        print(f'    {f}  shadows  import {name}')
    print('Rename the module or move it under tools/.')
    return 1


if __name__ == '__main__':
    raise SystemExit(main())
