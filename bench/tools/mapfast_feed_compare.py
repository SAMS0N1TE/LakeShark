"""Compare production framing/CRC/cell diff with raw32, RLE1 and indexed1.

Run test_rterm_visual.py first. Each variant changes only the picture codec
and pan deflate probes in its generated, hardware-free encoder fixture.
"""
from pathlib import Path
import json,re,subprocess,statistics
root=Path(__file__).resolve().parents[2]
out=root/'bench/build-mapfast'
fixture=(root/'bench/build-pixels/fixture.c').read_text()
encoder=(root/'components/apps/tui/ls_rterm_visual_encode.h').read_text()
helper=(root/'components/apps/tui/ls_rterm_picture_patch.h').read_text()
results=[]
for variant in ('raw32','raw1','rle1','indexed1','indexed4','indexed8','indexed32'):
    dest=out/variant;dest.mkdir(exist_ok=True)
    patch=helper
    if not variant.startswith('indexed'):
        patch=patch.replace('bool indexed=allow_index &&','bool indexed=false &&')
        patch=patch.replace('if(!input && s_picture.palette_generation','if(false && !input && s_picture.palette_generation')
    if variant.startswith('raw'):
        patch=re.sub(r'    for\(int x=0;x<width;\).*?\n','',patch,count=1)
        patch=patch.replace('bool rle=2+(size_t)runs*4<(indexed?packed/4:packed);','bool rle=false;')
    code=fixture.replace('#include "ls_rterm_visual_encode.h"',encoder.replace('#include "ls_rterm_picture_patch.h"',patch))
    probes=re.search(r'\d+$',variant)[0]
    code=code.replace('picture_deflate_probes(&s->visual)',f'((s->visual.pixels && s_picture.pan)?{probes}:32)')
    source=dest/'fixture.c';source.write_text(code)
    subprocess.run(['gcc','-std=gnu11','-O2','-UNDEBUG','-Wno-attributes',
        '-I'+str(root/'components/apps/tui'),'-I'+str(root/'components/ls_textpack/include'),
        '-I'+str(root/'bench/shims'),str(source),
        str(root/'components/apps/tui/tuilib/tui_core.c'),str(root/'components/apps/tui/ls_vt.c'),
        str(root/'components/apps/tui/ls_map_ink.c'),'-o',str(dest/'fixture.exe')],check=True)
    runs=[]
    for i in range(5):
        p=subprocess.run([str(dest/'fixture.exe'),str(root/'bench/build-pixels/desktop-map/snapshot.bin'),str(dest)],
                         cwd=root,check=True,capture_output=True,text=True)
        runs.append({v['case']:v for line in p.stdout.splitlines() if line.startswith('{') and '"case"' in line
                     for v in [json.loads(line)] if v['case'].startswith('2d-')})
    for case in runs[0]:
        result=dict(variant=variant,**runs[0][case])
        result['encode_us']=round(statistics.median(r[case]['encode_us'] for r in runs),1)
        results.append(result)
        print(json.dumps(result))
(out/'feed-comparison.json').write_text(json.dumps(results,indent=2))
