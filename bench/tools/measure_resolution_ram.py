"""Compare resolution changes' RV32 data/BSS against HEAD (no linking or hardware).

Run after the Nano build: python -B bench/tools/measure_resolution_ram.py
Uses that build's compile_commands.json/toolchain and packs HEAD's Cyrillic
bitmaps the same way as firmware. Writes evidence under bench/build-desk/ram.
This measures static sections, not the PSRAM heap budgets. Requires the build's
SDK and Python 3.
"""
from pathlib import Path
import json, re, subprocess
root = Path(__file__).resolve().parents[2]
out = root / 'bench/build-desk/ram'
out.mkdir(exist_ok=True)
raw = out / 'raw'
raw.mkdir(exist_ok=True)
paths = [
    'components/apps/tui/ls_rterm.c', 'components/apps/tui/ls_rterm.h',
    'components/lakeshark/board/ls_panel_ws.c', 'components/lakeshark/board/ls_panel_ws.h',
    'components/apps/tui/ls_font_list.c', 'components/apps/tui/ls_font.h',
    'components/apps/tui/ls_font_cyr.c', 'main/compact_ui.cpp',
]
for p in paths:
    (raw / Path(p).name).write_bytes(subprocess.check_output(['git','show','HEAD:'+p],cwd=root))
(raw/'tui').mkdir(exist_ok=True)
(raw/'tui/ls_rterm.h').write_bytes((raw/'ls_rterm.h').read_bytes())
subprocess.run(['python', str(root/'components/ls_textpack/gen/textpack_gen.py'),
                'fonts', str(out/'packed'), str(raw/'ls_font_cyr.c')],check=True)
entries = json.loads((root/'build_nano_v3_dsi/compile_commands.json').read_text())
result = []
new_fonts = ['ls_font_mono_11.c', 'ls_font_mono_13.c']
for name in ['ls_rterm.c','ls_panel_ws.c','ls_font_list.c','ls_font_cyr.c','compact_ui.cpp'] + new_fonts:
    entry = next(e for e in entries if Path(e['file']).name == name)
    compiler = entry['command'].split(' ',1)[0]
    objdump = str(Path(compiler).with_name('riscv32-esp-elf-objdump.exe'))
    values = {}
    for version in ['HEAD','work']:
        if version == 'HEAD' and name in new_fonts:
            values[version] = {'internal':0,'external':0}
            continue
        target = out / (name + '.' + version + '.o')
        cmd = re.sub(r' -o \S+', ' -o '+target.as_posix(),entry['command'])
        if version == 'HEAD':
            source = (out/'packed'/name) if name == 'ls_font_cyr.c' else (raw/name)
            cmd = cmd.replace(entry['file'],source.as_posix())
            exe, rest = cmd.split(' ',1)
            cmd = exe + ' -I'+raw.as_posix()+' -I'+Path(entry['file']).parent.as_posix()+' '+rest
        proc = subprocess.run(cmd,cwd=entry['directory'],capture_output=True,text=True)
        if proc.returncode:
            print(proc.stderr)
            raise SystemExit(proc.returncode)
        sections = subprocess.check_output([objdump,'-h',str(target)],text=True)
        internal = external = 0
        for line in sections.splitlines():
            m = re.match(r'\s*\d+\s+(\S+)\s+([0-9a-f]+)\s',line)
            if not m: continue
            section, size = m[1], int(m[2],16)
            if section.startswith(('.data','.bss','.sdata','.sbss','.noinit')): internal += size
            if 'ext_ram' in section: external += size
        values[version] = {'internal':internal,'external':external}
    result.append({'file':name,**values,'internal_delta':values['work']['internal']-values['HEAD']['internal'],
                   'external_delta':values['work']['external']-values['HEAD']['external']})
print(json.dumps(result,indent=2))
(out/'objects.json').write_text(json.dumps(result,indent=2))
