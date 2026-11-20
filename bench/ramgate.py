#!/usr/bin/env python3
"""Internal-RAM gate: reads a build's lakeshark.map and checks the static RAM
(.dram0.data + .dram0.bss + .noinit) against a per-board budget and a stored baseline.

Why: on the Nano modules are compiled into the firmware, so a module's big static
buffers land in internal RAM and the boot reservation (CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL,
a contiguous block at main_task) can no longer be satisfied. Nothing caught it before a board did.

  python bench/ramgate.py --config p4-nano-v3-dsi            # budget + baseline from bench/
  python bench/ramgate.py --map build_x/lakeshark.map --budget-kb 300 --top 15
  python bench/ramgate.py --config p4-nano-v3-dsi --record   # store the current numbers as baseline
  python bench/ramgate.py --config p4-nano-v3-dsi --diff old.map   # which objects grew

configs.json fields per config: ram_budget_kb (hard limit for dram statics),
ram_growth_kb (allowed growth over the baseline, default 8). Baselines: bench/ram-baseline.json.
Exit 1 on failure unless --warn-only.
"""
import argparse, json, os, re, sys

BENCH = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(BENCH)
BASELINE = os.path.join(BENCH, 'ram-baseline.json')
INTERNAL_LO, INTERNAL_HI = 0x4ff00000, 0x50000000      # P4 HP L2MEM (IRAM + DRAM)
DRAM_OUT = ('.dram0.data', '.dram0.bss', '.noinit')     # what the budget counts
OUT_RE = re.compile(r'^(\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)')
IN_FULL = re.compile(r'^ (\.\S+)\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$')
IN_NAME = re.compile(r'^ (\.\S+)\s*$')
IN_CONT = re.compile(r'^\s+0x([0-9a-f]+)\s+0x([0-9a-f]+)\s+(\S.*)$')


def parse(path):
    """-> (outputs {name: size}, items {(object, input_section): size}) for internal RAM."""
    outputs, items = {}, {}
    cur, pending = None, None
    with open(path, errors='replace') as f:
        for line in f:
            m = OUT_RE.match(line)
            if m:
                cur, pending = m.group(1), None
                addr, size = int(m.group(2), 16), int(m.group(3), 16)
                if INTERNAL_LO <= addr < INTERNAL_HI and size:
                    outputs[cur] = size
                continue
            if cur is None or not (cur.startswith('.dram0') or cur.startswith('.iram0') or cur == '.noinit'):
                continue
            m = IN_FULL.match(line)
            if m:
                name, addr, size, obj = m.group(1), int(m.group(2), 16), int(m.group(3), 16), m.group(4)
                pending = None
            else:
                m = IN_NAME.match(line)
                if m:
                    pending = m.group(1)
                    continue
                m = IN_CONT.match(line)
                if not (m and pending):
                    continue
                name, addr, size, obj = pending, int(m.group(1), 16), int(m.group(2), 16), m.group(3)
                pending = None
            if size and INTERNAL_LO <= addr < INTERNAL_HI and 'fill' not in obj.split()[0]:
                key = (cur, obj.strip(), name)
                items[key] = items.get(key, 0) + size
    return outputs, items


def find_nm():
    import glob, shutil
    n = shutil.which('riscv32-esp-elf-nm')
    if n:
        return n
    c = sorted(glob.glob('C:/Espressif/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-nm.exe') +
               glob.glob('C:/Espressif/tools/riscv32-esp-elf/*/bin/riscv32-esp-elf-nm.exe') +
               glob.glob('C:/esp/**/riscv32-esp-elf-nm.exe', recursive=True))
    return c[-1] if c else None


def elf_symbols(elf):
    """Largest internal-RAM data symbols by name; LTO builds lose the object name in the map."""
    import subprocess
    nm = find_nm()
    if not nm or not os.path.isfile(elf):
        return None
    try:
        out = subprocess.run([nm, '-S', '-C', '--defined-only', elf], capture_output=True, text=True, timeout=120).stdout
    except Exception:
        return None
    syms = []
    for ln in out.splitlines():
        p = ln.split(None, 3)
        if len(p) == 4 and p[2] in 'bBdDrRvV':
            try:
                addr, sz = int(p[0], 16), int(p[1], 16)
            except ValueError:
                continue
            if INTERNAL_LO <= addr < INTERNAL_HI and sz:
                syms.append((sz, p[3]))
    return sorted(syms, reverse=True)


def short(obj):
    obj = obj.replace('\\', '/')
    m = re.search(r'([^/]+\.a)\((.+?)\)', obj)
    if m:
        return '%s(%s)' % (m.group(1), m.group(2))
    return '/'.join(obj.split('/')[-3:])


def dram_total(outputs):
    return sum(outputs.get(n, 0) for n in DRAM_OUT)


def load_cfg(name):
    cfgs = json.load(open(os.path.join(BENCH, 'configs.json')))['configs']
    for c in cfgs:
        if c['name'] == name:
            return c
    sys.exit('ramgate: no config %r in bench/configs.json' % name)


def load_baseline():
    try:
        return json.load(open(BASELINE))
    except (OSError, ValueError):
        return {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--config')
    ap.add_argument('--map')
    ap.add_argument('--budget-kb', type=float)
    ap.add_argument('--max-growth-kb', type=float)
    ap.add_argument('--top', type=int, default=12)
    ap.add_argument('--record', action='store_true', help='store current numbers as the baseline')
    ap.add_argument('--diff', help='older map: list the objects that grew')
    ap.add_argument('--warn-only', action='store_true')
    a = ap.parse_args()

    cfg = load_cfg(a.config) if a.config else {}
    mp = a.map or os.path.join(ROOT, cfg.get('dir', ''), 'lakeshark.map')
    if not os.path.isfile(mp):
        sys.exit('ramgate: no map at %s' % mp)
    name = a.config or os.path.basename(os.path.dirname(os.path.abspath(mp)))
    budget = a.budget_kb if a.budget_kb is not None else cfg.get('ram_budget_kb')
    growth = a.max_growth_kb if a.max_growth_kb is not None else cfg.get('ram_growth_kb', 8)

    outputs, items = parse(mp)
    dram = dram_total(outputs)
    iram = outputs.get('.iram0.text', 0) + outputs.get('.iram0.data', 0)
    print('ramgate %s: internal statics %d bytes (%.1f KB)  [data %d + bss %d + noinit %d]  iram text %d' % (
        name, dram, dram / 1024, outputs.get('.dram0.data', 0), outputs.get('.dram0.bss', 0),
        outputs.get('.noinit', 0), iram))

    by_obj = {}
    for (out, obj, sec), sz in items.items():
        if out in DRAM_OUT:
            by_obj[short(obj)] = by_obj.get(short(obj), 0) + sz
    print('  top objects:')
    for o, sz in sorted(by_obj.items(), key=lambda x: -x[1])[:a.top]:
        print('    %8d  %s' % (sz, o))
    print('  top symbols:')
    esyms = elf_symbols(os.path.join(os.path.dirname(os.path.abspath(mp)), 'lakeshark.elf'))
    if esyms:
        for sz, nm_ in esyms[:a.top]:
            print('    %8d  %s' % (sz, nm_))
    syms = [] if esyms else sorted(((sz, sec, short(obj)) for (out, obj, sec), sz in items.items() if out in DRAM_OUT), reverse=True)
    for sz, sec, obj in syms[:a.top]:
        print('    %8d  %s  %s' % (sz, sec, obj))

    if a.diff:
        o2, i2 = parse(a.diff)
        old = {}
        for (out, obj, sec), sz in i2.items():
            if out in DRAM_OUT:
                old[(obj, sec)] = sz
        d = sorted(((sz - old.get((obj, sec), 0), sec, short(obj)) for (out, obj, sec), sz in items.items() if out in DRAM_OUT), reverse=True)
        print('  grew vs %s (total %+d):' % (a.diff, dram - dram_total(o2)))
        for dl, sec, obj in d[:a.top]:
            if dl > 0:
                print('    %+8d  %s  %s' % (dl, sec, obj))

    base = load_baseline()
    fails = []
    if budget is not None and dram > budget * 1024:
        fails.append('internal statics %d KB exceed the %d KB budget (over by %d bytes)' % (
            dram // 1024, budget, dram - int(budget * 1024)))
    b = base.get(name)
    if b is not None:
        d = dram - b['dram']
        print('  baseline %d bytes (%s): %+d bytes' % (b['dram'], b.get('commit', '?'), d))
        if d > growth * 1024:
            fails.append('internal statics grew %+d bytes over the baseline (limit +%d KB); see top objects, '
                         'move big buffers to PSRAM (EXT_RAM_BSS_ATTR) or re-record with --record if reviewed' % (d, growth))
    elif not a.record:
        print('  no baseline for %s (run with --record)' % name)

    if a.record:
        commit = os.popen('git -C "%s" rev-parse --short HEAD' % ROOT).read().strip()
        base[name] = {'dram': dram, 'iram_text': iram, 'commit': commit, 'map': os.path.relpath(mp, ROOT).replace(os.sep, '/')}
        json.dump(base, open(BASELINE, 'w'), indent=2, sort_keys=True)
        print('  recorded baseline for %s' % name)

    for f in fails:
        print('  %s: %s' % ('WARN' if a.warn_only else 'FAIL', f))
    if budget is not None and not fails:
        print('  PASS  (budget %d KB, %.1f KB free)' % (budget, budget - dram / 1024))
    sys.exit(1 if fails and not a.warn_only else 0)


if __name__ == '__main__':
    main()
