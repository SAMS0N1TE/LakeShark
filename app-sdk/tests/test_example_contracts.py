"""Build ALL templates/bundled apps with lap.py, then enforce simulator contracts.

python app-sdk/tests/test_example_contracts.py --frames 40 --build-dir bench/build
Artifacts and logs stay under bench/build/sdk-contracts. No board or GUI access.
Every template and bundled app must pass the current firmware contract.
"""
import argparse
import json
import os
from pathlib import Path
import re
import subprocess
import shutil
import sys
SDK=Path(__file__).resolve().parents[1]
sys.path.insert(0,str(SDK))
import lap
import lsdk

BAD=re.compile(r'simulator contract:.*(?:EINVAL|permission denied|flush failed)|\b(?:EINVAL|EACCES|EPERM)\b', re.I)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--frames',type=int,default=80)
    parser.add_argument('--build-dir',type=Path,default=SDK.parent/'bench/build')
    args=parser.parse_args(); bdir=args.build_dir.resolve(); work=bdir/'sdk-contracts'; work.mkdir(parents=True,exist_ok=True)
    env=lsdk.host_env()
    apps=list(sorted((SDK/'apps').iterdir())); apps=[p for p in apps if p.is_dir() and lap.project_sources(p)]
    templates=[]
    for t in sorted((SDK/'templates').iterdir()):
        if not t.is_dir(): continue
        dest=work/('template_'+t.name)
        # Always test the current template, including binary assets. This directory
        # is generated test input; logs and results live elsewhere under work.
        if dest.exists():
            assert dest.resolve().parent==work.resolve() and dest.name.startswith('template_')
            shutil.rmtree(dest)
        lsdk.new_app(dest.name,t.name,work,'SDK contract test')
        templates.append(dest)
    subprocess.run(['cmake','-S',str(SDK.parent/'bench'),'-B',str(bdir),'-G','Ninja','-DLS_SANITIZE=OFF',
                    '-DLAPSIM_EXTRA_APPS='+';'.join(p.as_posix() for p in templates)], env=env,check=True)
    subprocess.run(['cmake','--build',str(bdir),'--target','lssim','-j','8'],env=env,check=True)
    exe=bdir/('lssim.exe' if os.name=='nt' else 'lssim')
    results=[]
    for app in templates+apps:
        log=[]; failure=''; run_dir=work/('run_'+app.name); run_dir.mkdir(exist_ok=True)
        try:
            lap.build(app,work/(app.name+'.lap'),log=log)
            for scenario,script in [('launch',''),('lifecycle','5:hide;15:show;22:hide;30:stop')]:
                run_env=dict(env,LAPSIM=app.name,LAPSIM_SCRIPT=script,LAPSIM_CONSENT='grant')
                # Avoid inherited host modes and bitmap-per-frame capture.
                for key in ('LAPSIM_DLL','LSSIM_FRAMES','LAPSIM_RADIO'): run_env.pop(key,None)
                r=subprocess.run([str(exe),'lap','-A','40','-f',str(args.frames)],cwd=run_dir,env=run_env,
                                 capture_output=True,text=True,errors='replace',timeout=90,
                                 creationflags=subprocess.CREATE_NO_WINDOW if os.name=='nt' else 0)
                text=r.stdout+r.stderr; log.append(scenario+'\n'+text)
                bad=BAD.search(text)
                if r.returncode or bad:
                    detail=text[bad.start():].splitlines()[0] if bad else 'exit %d' % r.returncode
                    failure+=' %s: %s' % (scenario,detail)
        except (ValueError,OSError,subprocess.SubprocessError) as e:
            failure=str(e); log.append(getattr(e,'output',str(e)))
        status='FAIL' if failure else 'PASS'
        (work/(app.name+'.log')).write_text('\n'.join(log),encoding='utf-8')
        result=dict(app=app.name,status=status,reason=failure); results.append(result); print(status,app.name,failure,flush=True)
    (work/'results.json').write_text(json.dumps(results,indent=2)+'\n')
    return int(any(r['status']=='FAIL' for r in results))


if __name__=='__main__': sys.exit(main())
