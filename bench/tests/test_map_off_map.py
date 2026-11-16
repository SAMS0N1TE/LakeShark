"""Off the card's maps, CartoCore styles say so and point at the nearest map, anywhere."""
from pathlib import Path
import os, subprocess, sys, tempfile
root=Path(__file__).resolve().parents[2]
exe=Path(sys.argv[1]).resolve();cells=Path(sys.argv[2]).resolve()
def run(style,centre,script='',cellsdir=cells):
    env={k:v for k,v in os.environ.items() if not k.startswith('LSSIM_')}
    env.update(LSSIM_CARTOCORE=style,LSSIM_CARTO_ZOOM='15')
    if centre: env['LSSIM_CARTO_CENTER']=centre
    if cellsdir: env['LSSIM_CELLS']=str(cellsdir)
    else: env['LSSIM_CARTO_NO_EMBEDDED']='1'
    with tempfile.TemporaryDirectory(prefix='map-off-') as tmp:
        args=[str(exe),'map','-d','-f','14','-o',str(Path(tmp)/'out.bmp')]
        if script: args+=['-x',script]
        p=subprocess.run(args,cwd=root/'bench',env=env,text=True,capture_output=True,check=True)
    return p.stdout+p.stderr
for style in ('picture','smooth','braille'):
    # one cell south of the fixture's New England cells
    text=run(style,'42.5,-71.0')
    assert 'NO MAP HERE: GO TO MAP 75 mi N >' in text,(style,text)
    # the other side of the world: still the fixture's cells, not a US state list
    text=run(style,'-34.6,-58.38')
    assert 'NO MAP HERE: GO TO MAP 54' in text and ' mi N >' in text,(style,text)
    # on a downloaded cell: no hint, even where the cell is bare ground
    text=run(style,'43.5,-71.0')
    assert 'NO MAP' not in text and 'GO TO MAP' not in text,(style,text)
# a tap on the chip goes to that cell
text=run('picture','42.5,-71.0','5:47,~14')
assert 'MARK HERE 43.58039,-71.01562' in text and 'NO MAP' not in text,text
# nothing on the card at all
text=run('picture',None,cellsdir=None)
assert 'NO MAP ON CARD: GET ONE IN TILES' in text,text
print('PASS: off-map hint names the nearest downloaded map for every CartoCore style')
