"""Hardware startup regressions through shared selection/rendering and real MAP."""
from pathlib import Path
import os, shutil, subprocess, tempfile
from PIL import Image
root=Path(__file__).resolve().parents[2]
core=Path(os.environ.get('CARTOCORE_DIR',root.parent/'CartoCore-wip'))
out=root/'bench/out/cartomap';out.mkdir(parents=True,exist_ok=True)
with tempfile.TemporaryDirectory(prefix='carto-startup-') as temp:
    temp=Path(temp);maps=temp/'maps';maps.mkdir()
    mini=root/'data/franklin_mini.ctile';compact=core/'data/franklin_compact.ctile'
    shutil.copyfile(mini,maps/'mini.ctile');shutil.copyfile(compact,maps/'franklin_compact.ctile')
    # Include-order regression: sdkconfig is the ONLY source of the board define.
    (temp/'sdkconfig.h').write_text('#define CONFIG_LS_BOARD_T_DISPLAY_P4 1\n')
    (temp/'header.c').write_text('''#include "ls_cartocore_map.h"
bool ls_carto_map_draw(tui_surface *s,tui_rect a,double x,double y,unsigned z,bool b,bool l) {
    (void)s;(void)a;(void)x;(void)y;(void)z;(void)b;(void)l;return true;
}
''')
    subprocess.run(['gcc','-std=c11','-DESP_PLATFORM','-I'+str(temp),
        '-I'+str(root/'components/apps/tui'),'-c',str(temp/'header.c'),'-o',str(temp/'header.o')],check=True)
    # A smaller valid header covers 0,0 while the largest real map does not.
    moved=bytearray(mini.read_bytes())
    for offset,value in ((12,-10000000),(16,-10000000),(20,10000000),(24,10000000)):
        moved[offset:offset+4]=value.to_bytes(4,'little',signed=True)
    (maps/'cover.ctile').write_bytes(moved)
    (temp/'choose.c').write_text('''#include <assert.h>
#include <stdbool.h>
#include "ls_cartocore_select.h"
int main(int argc,char **argv) {
    assert(argc==2);char path[512];
    assert(carto_choose_file(argv[1],0,0,path,sizeof(path)) && strstr(path,"cover.ctile"));
    assert(carto_choose_file(argv[1],70,70,path,sizeof(path)) && strstr(path,"franklin_compact.ctile"));
}
''')
    subprocess.run(['gcc','-std=c11','-I'+str(core/'include'),'-I'+str(root/'components/apps/tui'),
        str(temp/'choose.c'),str(core/'build/libcartocore.a'),'-lm','-o',str(temp/'choose.exe')],check=True)
    subprocess.run([str(temp/'choose.exe'),str(maps)],check=True)
    (maps/'cover.ctile').unlink()
    # One frame must return immediately with an opening placeholder, before SD I/O.
    for layout in ('portrait','landscape'):
        env=dict(os.environ,LSSIM_CARTOCORE='smooth',LSSIM_CARTO_DIR=str(maps))
        bmp=out/f'opening-{layout}.bmp'
        args=[str(root/'bench/build/lssim.exe'),'map','-d','-f','1','-o',str(bmp)]
        if layout=='landscape':args+=['-l']
        p=subprocess.run(args,cwd=root,env=env,capture_output=True,text=True,check=True)
        assert 'opening map...' in p.stdout,p.stdout
        assert 'carto open ' not in p.stdout,p.stdout
        im=Image.open(bmp).convert('RGB')
        if layout=='landscape':im=im.transpose(Image.Transpose.ROTATE_270)
        im.save(bmp.with_suffix('.png'));bmp.unlink()
    for mode in ('smooth','braille'):
        for case in ('sd','embedded','internal-fail','arena-fail'):
            env={k:v for k,v in os.environ.items() if not k.startswith('LSSIM_CARTO') and k!='LSSIM_CTILE'}
            env.update(LSSIM_CARTO_BOUNDARY_TEST='1',LSSIM_CARTOCORE=mode,LSSIM_CARTO_CENTER='0,0',LSSIM_CARTO_ZOOM='22')
            if case!='embedded':env['LSSIM_CARTO_DIR']=str(maps)
            if case=='internal-fail':env['LSSIM_CARTO_FAIL_INTERNAL']='1'
            if case=='arena-fail':env['LSSIM_CARTO_FAIL_ARENA']='1'
            name=f'startup-{mode}-{case}';bmp=out/(name+'.bmp')
            p=subprocess.run([str(root/'bench/build/lssim.exe'),'map','-l','-d','-f','8','-o',str(bmp)],
                             cwd=root,env=env,capture_output=True,text=True,check=True)
            text=p.stdout+p.stderr;(out/(name+'.txt')).write_text(text)
            assert ' mi across' in text,text
            max_z=(mini if case=='embedded' else compact).read_bytes()[29]
            assert f'z{max_z} ' in text,text
            if case=='arena-fail':
                assert 'CartoCore map unavailable' in text and '.ctile map' in text,text
                assert text.count('carto MAP: failed step=')==1,text
                assert 'hot=' in text and 'cold=' in text and 'free_internal=' in text,text
            else:
                assert 'CartoCore map unavailable' not in text,text
                assert 'carto MAP: recentred' in text,text
                assert 'carto arenas' in text,text
                if case!='embedded':assert 'carto open /sdcard/maps/franklin_compact.ctile' in text,text
                if case=='internal-fail':assert 'rejected internal allocation' in text and 'PSRAM fallback' in text,text
            im=Image.open(bmp).convert('RGB').transpose(Image.Transpose.ROTATE_270)
            im.save(out/(name+'.png'));bmp.unlink()
    for mode in ('smooth','braille'):
        env={k:v for k,v in os.environ.items() if not k.startswith('LSSIM_CARTO') and k!='LSSIM_CTILE'}
        env.update(LSSIM_CARTOCORE=mode,LSSIM_CARTO_DIR=str(maps))
        for script in ('', '@down,@enter,~8'):
            bmp=temp/'picker.bmp'
            args=[str(root/'bench/build/lssim.exe'),'map','-k','m','-f','8','-d','-o',str(bmp)]
            if script:args+=['-x',script]
            p=subprocess.run(args,cwd=root,env=env,capture_output=True,text=True,check=True)
            if script:assert 'carto open /sdcard/maps/mini.ctile' in p.stdout,p.stdout
            else:assert 'CTILE MAPS' in p.stdout and 'franklin_compact.ctile' in p.stdout,p.stdout
print('PASS: firmware header, covering/largest SD selection, empty saved selection, outside centre, zoom clamp, internal fallback, bounded diagnostics and CTILE placeholder')
