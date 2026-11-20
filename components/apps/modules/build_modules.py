#!/usr/bin/env python3
"""Build the firmware modules (.lsm) of one board configuration.

A module is a set of the firmware's own source files compiled position
independent and linked as a shared ELF that the board loads from its internal
app storage (see tui/ls_mod.h). This script

  * compiles each module's sources with the flags the firmware uses for the
    component they come from (taken from compile_commands.json),
  * links <id>.lsm and lists the symbols it leaves undefined,
  * writes ls_mod_exports.S (name -> address, sorted) for the firmware,
  * writes ls_mod_table.inc: per module its file, size and SHA-256, and per
    screen a const proxy descriptor read out of the module ELF,
  * stages the storage partition's contents (spiffs/ + apps/<id>.lsm).

Outputs are rewritten only when they change, so an unchanged tree relinks
nothing.  Run by the build (components/apps/CMakeLists.txt); by hand:
  python build_modules.py --project . --build build_tdp4
"""
import argparse, hashlib, json, os, re, shlex, shutil, struct, subprocess, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

REF = {  # a file of each component that is always compiled: its flags are the component's
    'components/apps/': 'components/apps/tui/ls_app.c',
    'components/lakeshark/': 'components/lakeshark/core/settings.c',
}
# What a module asks for by one name and gets under another (see ls_mod.c).
OVERRIDE = {'ls_tui_screen_index_of': 'ls_mod_screen_index_of'}
SCREEN_PTRS = ('name', 'hint', 'enter', 'leave', 'draw', 'key', 'touch', 'radio')
SCREEN_BOOLS = ('hold_auto_rotation', 'diff_switch', 'wheel')

# A module's code and statics live in PSRAM. These patterns are wrong there unless a
# human has looked: put "mod-lint: ok <why>" in a comment on the line or the line above.
LINT = [
    (r'\bStatic(Task|Queue|Semaphore|EventGroup|Timer|StreamBuffer|MessageBuffer)_t\b',
     'static FreeRTOS object: a module static is PSRAM, FreeRTOS wants internal RAM (allocate it from the heap)'),
    (r'\bx\w*Create\w*Static\w*\s*\(', 'xxxCreateStatic with module buffers (TCB/queue storage must be internal RAM)'),
    (r'\b(IRAM_ATTR|DRAM_ATTR|RTC_\w*_ATTR|NOINLINE_ATTR_IRAM)\b', 'IRAM/DRAM placement is meaningless in a module and code that needs it must stay in the firmware'),
    (r'\b(esp_intr_\w+|gpio_isr_handler_add|gpio_install_isr_service|ESP_TIMER_ISR|\w+FromISR|portENTER_CRITICAL_ISR|portEXIT_CRITICAL_ISR)\b',
     'interrupt context: module code is in PSRAM and must not run from an ISR or with the cache off (use a firmware shim)'),
    (r'\.on_\w+\s*=|\b(i2s|rmt|spi|gptimer|sdmmc|lcd|usb|adc)_\w*_(register_event_callbacks|callbacks)\b',
     'driver "on_*" callback: runs in ISR context'),
    (r'\b(i2s_new_|rmt_new_|gptimer_new_|spi_bus_initialize|spi_bus_add_device|esp_lcd_new_|sdmmc_host_init|adc_continuous_new)',
     'peripheral driver set-up from a module: DMA buffers/ISRs belong in the firmware'),
]


def lint_module(project, mid, srcs):
    """Return a list of "file:line: message" for unreviewed risky patterns."""
    bad = []
    for src in srcs:
        sp = Path(src) if Path(src).is_absolute() else project / src
        lines = sp.read_text(encoding='utf-8', errors='replace').splitlines()
        in_block = False
        for i, ln in enumerate(lines):
            code = ln
            if in_block:
                k = code.find('*/')
                if k < 0:
                    continue
                code, in_block = code[k + 2:], False
            code = re.sub(r'/\*.*?\*/', ' ', code)
            if '/*' in code:
                code, in_block = code[:code.index('/*')], True
            code = re.sub(r'//.*', '', code)
            code = re.sub(r'"(\\.|[^"\\])*"', '""', code)
            for pat, why in LINT:
                if re.search(pat, code) and 'mod-lint: ok' not in ln and not (i and 'mod-lint: ok' in lines[i - 1]):
                    bad.append('%s:%d: [%s] %s' % (norm(src), i + 1, mid, why))
    return bad


def norm(p):
    return str(p).replace('\\', '/')


def write_if_changed(path, data):
    path = Path(path)
    if isinstance(data, str):
        data = data.encode('utf-8')
    if path.is_file() and path.read_bytes() == data:
        return False
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(data)
    return True


def sdkconfig(build):
    on = set()
    for ln in (Path(build) / 'sdkconfig').read_text(errors='replace').splitlines():
        m = re.match(r'CONFIG_(\w+)=(y|.+)', ln)
        if m and m.group(2) != 'n':
            on.add(m.group(1))
    return on


def enabled(mod, cfg):
    need = mod.get('if')
    veto = mod.get('unless')
    return (not need or need in cfg) and (not veto or veto not in cfg)


def ref_flags(build):
    db = json.loads((Path(build) / 'compile_commands.json').read_text(errors='replace'))
    by_file = {norm(e['file']).lower(): e for e in db}
    out = {}
    for prefix, ref in REF.items():
        ent = next((e for f, e in by_file.items() if f.endswith(ref.lower())), None)
        if not ent:
            raise SystemExit('modules: %s is not in compile_commands.json' % ref)
        args = shlex.split(ent['command'].replace('\\', '/'), posix=True)
        keep, skip = [], False
        for a in args[1:]:
            if skip:
                skip = False
                continue
            if a in ('-o', '-MF', '-MT', '-MQ'):
                skip = True
                continue
            if a in ('-c', '-MD', '-MMD', '-MP') or a.startswith('-flto') or a.startswith('-fdiagnostics-color'):
                continue
            if a.endswith(ref) or a.endswith('.c.obj') or a in ('-msave-restore', '-gdwarf-4', '-ggdb'):
                continue
            if a.startswith('-D'):      # quotes in a definition survive the round trip
                a = a.replace('"', '\\"') if '\\"' not in a else a
            keep.append(a)
        out[prefix] = (args[0], keep, ent['directory'])
    return out


def run(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, errors='replace')
    if r.returncode:
        sys.stderr.write(r.stdout + r.stderr)
        raise SystemExit('modules: failed: ' + ' '.join(map(str, cmd[:1])) + ' ... ' + str(cmd[-1]))
    return r.stdout


def deps_fresh(obj, dep, stamp, sig):
    if not obj.is_file() or not dep.is_file() or not stamp.is_file() or stamp.read_text() != sig:
        return False
    t = obj.stat().st_mtime
    text = dep.read_text(errors='replace').replace('\\\n', ' ')
    text = text.split(':', 1)[1] if ':' in text else ''
    # "C\:/x" drive letters are escaped in depfiles
    for f in shlex.split(text.replace('\\:', ':').replace('\\', '/')):
        try:
            if os.stat(f).st_mtime > t:
                return False
        except OSError:
            return False
    return True


def cstr(b):
    out = []
    for c in b:
        ch = chr(c)
        if ch.isalnum() or ch in " .,:;+-*/_()[]<>=!#%&'|~^@{}$":
            out.append(ch)
        else:
            out.append('\\%03o' % c)
    return '"' + ''.join(out) + '"'


class Elf:
    """Just enough of an ELF32 shared object to read a struct that holds pointers."""

    def __init__(self, data):
        self.d = data
        h = struct.unpack_from('<HHIIIIIHHHHHH', data, 16)
        self.ph, self.sh, self.np, self.ns, self.si = h[4], h[5], h[9], h[11], h[12]
        self.sections = [struct.unpack_from('<10I', data, self.sh + i * 40) for i in range(self.ns)]
        self.segments = [struct.unpack_from('<8I', data, self.ph + i * 32) for i in range(self.np)]
        self.rel = {}
        for s in self.sections:
            if s[1] == 4:   # RELA
                for o in range(s[4], s[4] + s[5], 12):
                    addr, info, addend = struct.unpack_from('<IIi', data, o)
                    self.rel[addr] = (info & 255, info >> 8, addend, s[6])
        self.syms = {}
        for s in self.sections:
            if s[1] == 2:   # SYMTAB
                st = self.sections[s[6]]
                for o in range(s[4], s[4] + s[5], 16):
                    ni, val, size, info, other, ndx = struct.unpack_from('<IIIBBH', data, o)
                    if ndx:
                        end = data.index(b'\0', st[4] + ni)
                        self.syms[data[st[4] + ni:end].decode('latin1')] = (val, size)

    def at(self, vaddr, n):
        for t, off, va, _, fsz, msz, fl, al in self.segments:
            if t == 1 and va <= vaddr and vaddr + n <= va + fsz:
                return self.d[off + vaddr - va: off + vaddr - va + n]
        return b'\0' * n

    def pointer(self, vaddr):
        """The address a pointer field will hold after loading, or 0."""
        r = self.rel.get(vaddr)
        if r and r[0] == 3:           # R_RISCV_RELATIVE
            return r[2] & 0xffffffff
        if r:                         # bound to a symbol: non-null
            return 1
        return struct.unpack('<I', self.at(vaddr, 4))[0]

    def writable(self, vaddr):
        return any(t == 1 and fl & 2 and va <= vaddr < va + msz for t, off, va, _, fsz, msz, fl, al in self.segments)

    def string(self, vaddr):
        if self.writable(vaddr):
            raise ValueError('a screen name or radio is a writable buffer; the proxy needs constant text')
        out = b''
        while True:
            c = self.at(vaddr + len(out), 1)
            if c == b'\0':
                return out
            out += c


def screen_fields(elf, name):
    if name not in elf.syms:
        raise SystemExit('modules: screen %s is not defined by its module' % name)
    base = elf.syms[name][0]
    f = {}
    for i, k in enumerate(SCREEN_PTRS):
        p = elf.pointer(base + 4 * i)
        if k == 'hint' and p and elf.writable(p):   # a hint the screen rewrites: the proxy mirrors it
            f['hint'], f['hint_live'] = b'', True
            continue
        f[k] = (elf.string(p) if p else None) if k in ('name', 'hint', 'radio') else bool(p)
    for i, k in enumerate(SCREEN_BOOLS):
        f[k] = elf.at(base + 4 * len(SCREEN_PTRS) + i, 1) != b'\0'
    return f


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--project', required=True)
    ap.add_argument('--build', required=True)
    ap.add_argument('--json')
    ap.add_argument('--out')
    ap.add_argument('--storage-src')
    ap.add_argument('--storage-root')
    ap.add_argument('--only', help='comma list of module ids (experiments with one module)')
    a = ap.parse_args()
    project, build = Path(a.project).resolve(), Path(a.build).resolve()
    here = Path(__file__).resolve().parent
    spec = json.loads(Path(a.json or here / 'modules.json').read_text(encoding='utf-8'))
    out = Path(a.out) if a.out else build / 'ls_modules'
    storage_src = Path(a.storage_src) if a.storage_src else project / 'spiffs'
    storage_root = Path(a.storage_root) if a.storage_root else build / 'storage_root'
    cfg = sdkconfig(build)
    mods = [m for m in spec['modules'] if enabled(m, cfg)]
    if a.only:
        mods = [m for m in mods if m['id'] in a.only.split(',')]
    flags = ref_flags(build)
    cc = flags['components/apps/'][0]
    tool = lambda n: cc.replace('-gcc', '-' + n)
    extra = ['-fPIC', '-fno-lto', '-mno-save-restore', '-fvisibility=hidden', '-ULS_TEXT_PACKED',
             '-DLS_MODULE_BUILD=1', '-include', norm(here / 'ls_mod_prelude.h'), '-w']
    findings = [f for m in mods for f in lint_module(project, m['id'], m['srcs'])]
    import ram_lint
    for m in mods:      # big statics need EXT_RAM_BSS_ATTR (the Nano compiles modules into the firmware)
        for src in m['srcs']:
            sp = Path(src) if Path(src).is_absolute() else project / src
            findings += ram_lint.lint_ram(sp, norm(src), m['id'], sp.read_text(encoding='utf-8', errors='replace').splitlines(), project)
    if findings:
        raise SystemExit('modules: lint failed (add "mod-lint: ok <why>" once reviewed):\n  ' + '\n  '.join(findings))
    out.mkdir(parents=True, exist_ok=True)
    table, undefined, built = [], set(), []
    for m in mods:
        mid = m['id']
        mdir = out / mid
        mdir.mkdir(exist_ok=True)
        objs = []
        glue = ['/* generated by build_modules.py */', '#include "%s"' % norm(project / 'components/apps/tui/ls_mod.h')]
        for s in m.get('screens', []):
            glue.append('extern const char %s[];' % s)
        for s in m.get('syms', []) + ([m['init']] if m.get('init') else []):
            glue.append('extern void %s(void);' % s)
        glue.append('static const void *const SCREENS[] = {%s};' % ', '.join(m.get('screens', [])) if m.get('screens')
                    else 'static const void *const SCREENS[1];')
        glue.append('static const ls_mod_sym_t SYMS[] = {%s {0, 0}};' %
                    ''.join('{"%s", (void *)%s}, ' % (s, s) for s in m.get('syms', [])))
        glue.append('static const ls_mod_desc_t DESC = {LS_MOD_MAGIC, SCREENS, %d, %s, SYMS, %d};' %
                    (len(m.get('screens', [])), '(void (*)(void))' + m['init'] if m.get('init') else '0', len(m.get('syms', []))))
        glue.append('const ls_mod_desc_t *ls_mod_main(void) { return &DESC; }')
        glue_c = mdir / ('%s_glue.c' % mid)
        write_if_changed(glue_c, '\n'.join(glue) + '\n')
        for src in m['srcs'] + [norm(glue_c)]:
            sp = Path(src) if Path(src).is_absolute() else project / src
            rel = norm(src)
            prefix = next((p for p in flags if rel.startswith(p)), 'components/apps/')
            _, fl, cwd = flags[prefix]
            obj = mdir / (sp.name + '.o')
            dep = mdir / (sp.name + '.d')
            stamp = mdir / (sp.name + '.cmd')
            cmd = [cc, *fl, m.get('opt', '-Os'), *extra, *m.get('cflags', []), '-MD', '-MF', norm(dep), '-c', norm(sp), '-o', norm(obj)]
            sig = ' '.join(cmd)
            if not deps_fresh(obj, dep, stamp, sig):
                print('modules: %s <- %s' % (mid, sp.name), flush=True)
                run(cmd, cwd=cwd)
                stamp.write_text(sig)
            objs.append(norm(obj))
        elf_path = mdir / (mid + '.elf')
        lsm = mdir / (mid + '.lsm')
        if not lsm.is_file() or any(os.stat(o).st_mtime > lsm.stat().st_mtime for o in objs):
            run([cc, '-march=rv32imafc_zicsr_zifencei', '-mabi=ilp32f', '-shared', '-nostdlib', '-nostartfiles',
                 '-e', 'ls_mod_main', '-Wl,--gc-sections,-Bsymbolic,-z,noexecstack,-z,max-page-size=4096',
                 *objs, '-o', norm(elf_path)])
            run([tool('strip'), '--strip-unneeded', '--remove-section=.comment', '--remove-section=.riscv.attributes',
                 norm(elf_path), '-o', norm(lsm)])
        elf = Elf(elf_path.read_bytes())
        und = [ln.split()[-1] for ln in run([tool('nm'), '-D', '--undefined-only', norm(lsm)]).splitlines() if ln.strip()]
        undefined.update(und)
        blob = lsm.read_bytes()
        try:
            screens = [(s, screen_fields(elf, s)) for s in m.get('screens', [])]
        except ValueError as e:
            raise SystemExit('modules: %s: %s' % (mid, e))
        table.append((mid, blob, screens))
        built.append('%s %d bytes, %d imports' % (mid, len(blob), len(und)))

    # ---- export table: every name a module imports, sorted, in assembly so no C type is claimed
    names = sorted(undefined)
    s = ['/* generated by build_modules.py: what the modules import from the firmware */',
         '    .section .rodata.ls_mod_exports,"a",@progbits', '    .balign 4',
         '    .global ls_mod_exports', '    .global ls_mod_exports_count', 'ls_mod_exports:']
    for i, n in enumerate(names):
        s.append('    .word .Ln%d, %s' % (i, OVERRIDE.get(n, n)))
    s += ['    .balign 4', 'ls_mod_exports_count:', '    .word %d' % len(names), '    .section .rodata.ls_mod_names,"a",@progbits']
    for i, n in enumerate(names):
        s.append('.Ln%d: .asciz "%s"' % (i, n))
    write_if_changed(out / 'ls_mod_exports.S', '\n'.join(s) + '\n')
    write_if_changed(out / 'ls_mod_exports.txt', '\n'.join(names) + '\n')

    # ---- module and screen tables
    t = ['/* generated by build_modules.py */']
    scr_index = 0
    rows, srows = [], []
    for mi, (mid, blob, screens) in enumerate(table):
        sha = hashlib.sha256(blob).digest()
        rows.append('    {"%s", %du, {%s}},' % (mid, len(blob), ','.join('0x%02x' % b for b in sha)))
        for k, (sname, f) in enumerate(screens):
            t.append('LS_MOD_SCREEN(%d)' % scr_index)
            live = f.get('hint_live')
            if live:
                t.append('EXT_RAM_BSS_ATTR static char ls_mod_hint_%d[LS_MOD_HINT_MAX];' % scr_index)
            fn = lambda key: ('ls_mod_s%d_%s' % (scr_index, key)) if f[key] else 'NULL'
            t.append('const ls_tui_screen_t %s = {.name = %s, .hint = %s, .enter = ls_mod_s%d_enter, .leave = ls_mod_s%d_leave, '
                     '.draw = ls_mod_s%d_draw, .key = %s, .touch = %s, .radio = %s, .hold_auto_rotation = %s, '
                     '.diff_switch = %s, .wheel = %s};' % (
                         sname, cstr(f['name'] or b''),
                         ('ls_mod_hint_%d' % scr_index) if live else cstr(f['hint']) if f['hint'] is not None else 'NULL',
                         scr_index, scr_index, scr_index, fn('key'), fn('touch'),
                         cstr(f['radio']) if f['radio'] is not None else 'NULL',
                         *('true' if f[b] else 'false' for b in SCREEN_BOOLS)))
            srows.append('    {%d, %d, &%s, %s},' % (mi, k, sname, ('ls_mod_hint_%d' % scr_index) if live else 'NULL'))
            scr_index += 1
    t.append('static const ls_mod_info_t LS_MODS[] = {')
    t += rows or ['    {"", 0, {0}},']
    t.append('};')
    t.append('static const ls_mod_screen_info_t LS_MOD_SCREENS[] = {')
    t += srows or ['    {0, 0, NULL, NULL},']
    t.append('};')
    stamp = hashlib.sha256(b''.join(hashlib.sha256(b).digest() for _, b, _ in table)).hexdigest()[:16]
    t.append('#define LS_MOD_STAMP "%s"' % stamp)
    t.append('#define LS_MOD_COUNT %d' % len(rows))
    t.append('#define LS_MOD_SCREEN_COUNT %d' % len(srows))
    write_if_changed(out / 'ls_mod_table.inc', '\n'.join(t) + '\n')
    write_if_changed(out / 'ls_mod_config.h', '/* generated by build_modules.py */\n#pragma once\n' +
                     ''.join('#define LS_MOD_HAS_%s 1\n' % mid.upper() for mid, _, _ in table))

    # ---- storage partition contents
    apps = storage_root / 'apps'
    apps.mkdir(parents=True, exist_ok=True)
    want = {}
    if storage_src.is_dir():
        for p in storage_src.rglob('*'):
            if p.is_file():
                want[norm(p.relative_to(storage_src))] = p.read_bytes()
    for mid, blob, _ in table:
        want['apps/%s.lsm' % mid] = blob
    # "these are this firmware's modules": a fresh install need not hash them at boot
    want['apps/modules.ok'] = stamp.encode()
    for rel, data in want.items():
        write_if_changed(storage_root / rel, data)
    for p in list(storage_root.rglob('*')):
        if p.is_file() and norm(p.relative_to(storage_root)) not in want:
            p.unlink()
    # what to upload after a release: <first 32 hex of sha256>.lsm (see ls_mod.c)
    write_if_changed(out / 'publish.txt', ''.join(
        '%s.lsm %s %d\n' % (hashlib.sha256(b).hexdigest()[:32], mid, len(b)) for mid, b, _ in table))
    total = sum(len(b) for _, b, _ in table)
    print('modules: %s; %d exports; storage %d bytes in %d files' %
          ('; '.join(built) or 'none', len(names), sum(len(v) for v in want.values()), len(want)))
    return 0


if __name__ == '__main__':
    sys.exit(main())
