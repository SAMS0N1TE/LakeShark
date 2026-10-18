"""EXT_RAM_BSS_ATTR places a variable in PSRAM .bss, which is zeroed at boot: any initializer is
silently dropped on the board while host builds (where the attribute is empty) keep it. 2.16.4's
first build crashed at boot this way (a callback table declared with an initializer was all NULL).

usage: test_ext_ram_bss_no_init.py <repo root>
"""
import re
import sys
from pathlib import Path

root = Path(sys.argv[1])
decl = re.compile(r"EXT_RAM_BSS_ATTR[^;(){}]*?=(?!=)")
bad = []
for d in ("components", "main"):
    for p in (root / d).rglob("*"):
        if p.suffix not in (".c", ".cpp", ".h", ".inc"):
            continue
        text = p.read_text(encoding="utf-8", errors="replace")
        text = re.sub(r"/\*.*?\*/", "", text, flags=re.S)
        for n, line in enumerate(text.splitlines(), 1):
            line = line.split("//", 1)[0]
            if "#define" in line:
                continue
            if decl.search(line):
                bad.append("%s:%d: %s" % (p.relative_to(root), n, line.strip()))
for b in bad:
    print("initializer on EXT_RAM_BSS_ATTR (zeroed at boot on the board):", b)
print("%d offenders" % len(bad))
sys.exit(1 if bad else 0)
