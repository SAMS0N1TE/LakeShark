"""No tracked module may take the name of a Python standard library one.

LS-1157  A file called cmd.py sat at the repository root. It shadowed the
standard library's `cmd`, and it opened a serial port at import time.

Nothing in this project imports `cmd`. It did not need to. Running
esp-coredump from the repository root failed with

    serial.serialutil.SerialException: could not open port 'info_corefile'

because esp-coredump imports construct, construct imports pdb, and pdb
imports cmd - which found this instead, read sys.argv and dialled a COM port.
The tool the `crash` command tells an operator to use was blocked by a file in
the working directory, and the error named neither the file nor the reason.

That is the whole failure mode worth catching: the cost is not in the module
that shadows, it is in some unrelated tool three imports away, and the message
points nowhere near the cause.

Only the repository root is checked. That is where Python puts the script's
own directory on sys.path, so it is where a name collision actually bites; a
module deeper in the tree is only reachable by something that deliberately
put it there.

Tracked files only. An operator's local scratch tool is theirs, and this is a
question about what the repository carries to another machine.
"""
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
    print('Rename it, or move it under tools/. LS-1157 is what this costs: an '
          'unrelated tool fails three imports away with an error that names '
          'neither the file nor the reason.')
    return 1


if __name__ == '__main__':
    raise SystemExit(main())
