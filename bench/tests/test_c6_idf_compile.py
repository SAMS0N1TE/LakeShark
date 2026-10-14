"""Syntax-check only the C6 integration files with an existing IDF build's flags.

No configuration/build/component download/flash. Generated sdkconfig and IDF
headers come from --database; source/include paths point at this worktree.
Usage: python bench/tests/test_c6_idf_compile.py --database PATH/compile_commands.json
"""
import argparse
import ctypes
import json
import os
from pathlib import Path
import shlex
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def split(command):
    if os.name != "nt":
        return shlex.split(command)
    count = ctypes.c_int()
    fn = ctypes.windll.shell32.CommandLineToArgvW
    fn.argtypes = [ctypes.c_wchar_p, ctypes.POINTER(ctypes.c_int)]
    fn.restype = ctypes.POINTER(ctypes.c_wchar_p)
    ptr = fn(command, ctypes.byref(count))
    try:
        return list(ptr[:count.value])
    finally:
        ctypes.windll.kernel32.LocalFree(ptr)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--database", type=Path, required=True)
    parser.add_argument("--source", action="append", help="Only check these changed integration sources")
    args = parser.parse_args()
    db = json.loads(args.database.read_text())
    changes = {
        "main/ls_c6_core.c": "main/ls_ota.c",
        "main/ls_c6_update.c": "main/ls_ota.c",
        "main/ls_wifi.c": "main/ls_wifi.c",
        "main/ble_link.c": "main/ble_link.c",
        "main/ble_link_stub.c": "main/ls_ota.c",
        "main/ls_ota.c": "main/ls_ota.c",
        "main/compact_ui.cpp": "main/compact_ui.cpp",
        "components/apps/tui/ls_c6_view.c": "components/apps/tui/ls_update_view.c",
        "components/apps/tui/ls_update_view.c": "components/apps/tui/ls_update_view.c",
    }
    if args.source:
        changes = {source: changes[source] for source in args.source}
    for source, template in changes.items():
        entry = next(e for e in db if e["file"].replace("\\", "/").endswith("/" + template))
        old_root = entry["file"].replace("\\", "/")[:-len(template)].rstrip("/")
        command = entry.get("arguments") or split(entry["command"])
        rewritten = []
        skip = False
        for arg in command:
            if skip:
                skip = False
                continue
            if arg in ("-o", "-MF", "-MT", "-MQ", "-c"):
                skip = True
                continue
            if arg in ("-MD", "-MMD", "-MP"):
                continue
            # Generated headers and response files stay in the existing build.
            for subtree in ("main", "components", "managed_components"):
                arg = arg.replace(old_root + "/" + subtree, ROOT.as_posix() + "/" + subtree)
            rewritten.append(arg)
        rewritten.extend(["-fsyntax-only", str(ROOT / source)])
        print("Checking", source, flush=True)
        subprocess.run(rewritten, cwd=entry["directory"], check=True)
    print(f"PASS: {len(changes)} integration translation units (syntax only)")


if __name__ == "__main__":
    main()
