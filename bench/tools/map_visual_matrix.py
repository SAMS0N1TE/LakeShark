"""Production MAP stills: all views/themes/zooms, isolated synthetic live fixtures."""
from pathlib import Path
import concurrent.futures, json, os, subprocess, sys, tempfile
from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[2]
stage = sys.argv[1] if len(sys.argv) > 1 else 'after'
out = root / 'bench/out/map-visual' / stage
out.mkdir(parents=True, exist_ok=True)
themes = ['Terminal Bay', 'Amber', 'Phosphor', 'Ice', 'Synthwave', 'VFD', 'Arcade', 'Night', 'Daylight']
views = ['field', 'blocks', 'lines', 'smooth', 'braille']

def render(job):
    view, zoom, theme, source = job
    name = f'{view}-z{zoom}-{theme.replace(" ", "_")}-{source}'
    env = dict(os.environ, LSSIM_MAP_VIEW=str(views.index(view)),
               LSSIM_MAP_ZOOM=str(zoom), LSSIM_MAP_BUSY='1')
    for key in ('LSSIM_CARTOCORE', 'LSSIM_CELLS', 'LSSIM_CTILE'):
        env.pop(key, None)
    if views.index(view) >= 3:
        env.update(LSSIM_CARTOCORE=view, LSSIM_CARTO_ZOOM=str(zoom),
                   LSSIM_CTILE='D:/assasdad/CartoCore/data/central_nh_compact.ctile')
        if source == 'cells':
            cells=Path('D:/assasdad/CartoCore/data/cells-nh')
            if (cells/'CURRENT').exists(): cells /= (cells/'CURRENT').read_text().strip()
            env['LSSIM_CELLS'] = str(cells)
            env.pop('LSSIM_CTILE',None)
            env['LSSIM_CARTO_NO_EMBEDDED']='1'
    with tempfile.TemporaryDirectory(dir=out, prefix='scene-') as tmp:
        env['TEMP'] = tmp
        args = [os.environ.get('MAP_VISUAL_EXE',str(root/'bench/build/lssim.exe')), 'map', '-f', '8', '-d', '-A', '24',
                '-m', str(root/'bench/fixtures/carto/franklin_z12.pmtiles'), '-o', str(out/(name+'.bmp'))]
        args += ['-D'] if theme == 'Daylight' else ['-t', theme]
        p = subprocess.run(args, cwd=root, env=env, text=True, capture_output=True, timeout=90)
        (out/(name+'.txt')).write_text(p.stdout+p.stderr)
        if p.returncode:
            raise RuntimeError(name+' failed: '+p.stderr[-1000:])
        bmp = out/(name+'.bmp')
        Image.open(bmp).convert('RGB').save(out/(name+'.png'))
        bmp.unlink()
    return name

jobs = [(v,z,t, 'ctile' if i>=3 else 'pmtiles') for i,v in enumerate(views)
        for z in (10,13,15) for t in themes]
jobs += [(v,z,t,'cells') for v in views[3:] for z in (10,13,15) for t in themes]
with concurrent.futures.ThreadPoolExecutor(max_workers=3) as pool:
    results = list(pool.map(render,jobs))
for theme in themes:
    sheet = Image.new('RGB', (5*230,3*515), '#25272b')
    draw = ImageDraw.Draw(sheet)
    for col,v in enumerate(views):
        for row,z in enumerate((10,13,15)):
            source='ctile' if col>=3 else 'pmtiles'
            name=f'{v}-z{z}-{theme.replace(" ", "_")}-{source}'
            im=Image.open(out/(name+'.png'));im.thumbnail((225,485))
            sheet.paste(im,(col*230,row*515+25))
            draw.text((col*230+4,row*515+4),f'{v} z{z} {theme}',fill='white')
    sheet.save(out/(theme.replace(' ','_')+'-sheet.png'))
(out/'manifest.json').write_text(json.dumps(results,indent=2))
print(f'{stage}: {len(results)} production MAP renders; sheets in {out}',flush=True)
