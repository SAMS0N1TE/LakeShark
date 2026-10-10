"""Check board label attributes/palette and render real MAP labels on terrain."""
from pathlib import Path
import os, shutil, subprocess, tempfile
from PIL import Image
root=Path(__file__).resolve().parents[2]
core=Path(os.environ.get('CARTOCORE_DIR', str(root.parent/'CartoCore-wip')))
assert os.environ.get('LSSIM_CTILE'), 'Select a terrain map with LSSIM_CTILE'
out=root/'bench/out/map-visual/cartomap-tests';out.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(prefix='carto-labels-') as tmp:
    src=Path(tmp)/'labels.c'; exe=Path(tmp)/'labels.exe'
    src.write_text(r'''#include "ls_cartocore_cells.h"
#include "ls_theme.h"
#include <assert.h>
#include <stdio.h>
static bool daylight;
bool ls_tui_daylight(void) { return daylight; }
int main(void) {
    unsigned old[]={0,15,7,6,11,7}, bright[]={15,15,15,14,11,15};
    for(unsigned day=0;day<2;day++) { daylight=day;
    for(unsigned i=0;i<6;i++) {
        cc_cell c={'A', (0x80u|old[i])<<24, 0x80000000u};
        unsigned a=carto_attr(c);
        c.codepoint=0xe9; assert(carto_attr(c)==a);
        assert(TUI_ATTR_FG(a)==(day?bright[i]:11) && TUI_ATTR_BG(a)==TUI_BLACK);
        for(int theme=0;theme<ls_tui_theme_count();theme++) {
            const ls_tui_theme_t *t=day?&ls_theme_daylight:ls_tui_theme_at(theme);
            printf("%u %u\n",t->palette[TUI_ATTR_FG(a)],t->palette[0]);
        }
    }
    }
    /* Text spacing is already a plate; map shapes/terrain keep their colours. */
    cc_cell q={0x2598,0x86000000u,0x82000000u};
    assert(carto_attr(q)==TUI_ATTR(TUI_CYAN,TUI_GREEN));
    q.codepoint=0x28ff; assert(glyph(q.codepoint)==0x28ff);
    assert(carto_attr(q)==TUI_ATTR(TUI_CYAN,TUI_GREEN));
    assert(ls_theme_daylight.palette[15]==0 && ls_theme_daylight.palette[0]==0xffff);
}
''')
    subprocess.run([shutil.which('gcc'),'-std=c11','-I'+str(core/'include'),
        '-I'+str(root/'components/apps/tui'),str(src),
        str(root/'components/apps/tui/ls_theme.c'),'-o',str(exe)],check=True)
    def luminance(n):
        rgb=((n>>11)/31,((n>>5)&63)/63,(n&31)/31)
        v=[x/12.92 if x<=0.04045 else ((x+0.055)/1.055)**2.4 for x in rgb]
        return sum(a*b for a,b in zip(v,(0.2126,0.7152,0.0722)))
    for line in subprocess.check_output([str(exe)],text=True).splitlines():
        fg,bg=map(int,line.split());assert (max(luminance(fg),luminance(bg))+.05)/(min(luminance(fg),luminance(bg))+.05)>7
for mode in ('smooth','braille'):
    for layout in ('portrait','landscape'):
        for zoom,extra in ((12,[]),(14,[])):
            name=f'labels-{mode}-{layout}-z{zoom}'
            bmp=out/(name+'.bmp')
            args=[str(root/'bench/build/lssim.exe'),'map','-o',str(bmp),'-f','8','-d']
            if layout=='landscape':args+=['-l']
            p=subprocess.run(args+extra,cwd=root,env=dict(os.environ,LSSIM_CARTOCORE=mode,LSSIM_CARTO_CENTER='43.45,-71.65',LSSIM_CARTO_ZOOM=str(zoom)),
                             capture_output=True,text=True,check=True)
            assert 'CartoCore '+mode in p.stdout and 'unavailable' not in p.stdout
            (out/(name+'.txt')).write_text(p.stdout)
            im=Image.open(bmp).convert('RGB')
            if layout=='landscape':im=im.transpose(Image.Transpose.ROTATE_270)
            im.save(out/(name+'.png'));bmp.unlink()
print('PASS: all label classes yellow; every theme contrast >7:1; Daylight and shapes unchanged; eight MAP PNGs')
