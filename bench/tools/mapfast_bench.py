"""Build/run only the focused CartoCore pan test, using vendored MAP sources."""
from pathlib import Path
import subprocess, sys, shutil, os
root=Path(__file__).resolve().parents[2]
out=root/'bench/build-mapfast';out.mkdir(exist_ok=True)
cc=root/'components/cartocore/upstream'
sources=list((cc/'src/core').glob('*.c'))+list((cc/'src/out').glob('*.c'))+list((cc/'src/places').glob('*.c'))
baseline='--baseline' in sys.argv
if baseline:
    saved=out/'baseline';(saved/'include/cartocore').mkdir(parents=True,exist_ok=True)
    for header in (cc/'include/cartocore').glob('*.h'):
        rel=header.relative_to(root).as_posix()
        (saved/'include/cartocore'/header.name).write_bytes(subprocess.check_output(['git','show','HEAD:'+rel],cwd=root))
    old=[]
    for source in sources:
        target=saved/source.relative_to(cc)
        target.parent.mkdir(parents=True,exist_ok=True)
        target.write_bytes(subprocess.check_output(['git','show','HEAD:'+source.relative_to(root).as_posix()],cwd=root))
        old.append(target)
    # Core private includes remain adjacent to their original sources.
    for header in (cc/'src').rglob('*.h'):
        target=saved/header.relative_to(cc);target.parent.mkdir(parents=True,exist_ok=True);shutil.copyfile(header,target)
    shutil.copytree(cc/'third_party',saved/'third_party',dirs_exist_ok=True)
    sources=old
    include=saved/'include'
else:include=cc/'include'
exe=out/('test_carto_pan_before.exe' if baseline else 'test_carto_pan.exe')
subprocess.run(['gcc',*(['-DMAPFAST_BASELINE'] if baseline else []),'-O2','-g','-Wall','-Wextra','-I'+str(include),str(root/'bench/tests/test_carto_pan.c'),*map(str,sources),'-lm','-o',str(exe)],check=True,cwd=root)
p=subprocess.run([str(exe),next((v for v in sys.argv[1:] if not v.startswith('--')),str(root/'data/franklin_mini.ctile'))],cwd=root,env=dict(os.environ,MAPFAST_RGB565=str(out/"map.rgb565")),capture_output=True,text=True)
if p.returncode:print(p.stdout,p.stderr);p.check_returncode()
print(p.stdout,end='')
(out/('pan-before.txt' if baseline else 'pan-after.txt')).write_text(p.stdout)
