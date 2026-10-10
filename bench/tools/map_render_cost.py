"""Compare warm and moving production MAP host frame costs, all five views."""
from pathlib import Path
import argparse, json, os, re, subprocess, tempfile

root=Path(__file__).resolve().parents[2]
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--before', type=Path, required=True,
                    help='Existing baseline lssim executable saved before the change')
parser.add_argument('--after', type=Path, default=root/'bench/build/lssim.exe',
                    help='Current lssim executable')
args=parser.parse_args()
executables={'before': args.before.resolve(), 'after': args.after.resolve()}
for stage, exe in executables.items():
    if not exe.is_file():
        parser.error(f'{stage} simulator does not exist: {exe}')
out=root/'bench/out/map-visual';out.mkdir(parents=True,exist_ok=True)
results=[]
for stage in ('before','after'):
    exe=executables[stage]
    for view in range(5):
        for z in (10,13,15):
            for moving in (False,True):
                env={k:v for k,v in os.environ.items() if not k.startswith('LSSIM_')}
                env.update(LSSIM_MAP_VIEW=str(view),LSSIM_MAP_ZOOM=str(z),LSSIM_MAP_BUSY='1')
                if view>=3:
                    env.update(LSSIM_CARTOCORE='smooth' if view==3 else 'braille',
                        LSSIM_CARTO_ZOOM=str(z),LSSIM_CTILE='D:/assasdad/CartoCore/data/central_nh_compact.ctile')
                with tempfile.TemporaryDirectory(dir=out) as tmp:
                    env['TEMP']=tmp
                    args=[str(exe),'map','-f','8','-m',str(root/'bench/fixtures/carto/franklin_z12.pmtiles'),
                          '-P' if moving else '-T','120','-o',str(Path(tmp)/'frame.bmp')]
                    p=subprocess.run(args,cwd=root,env=env,capture_output=True,text=True,check=True,timeout=30)
                    us=int(re.search(r'(\d+) us/frame',p.stdout)[1])
                    results.append(dict(stage=stage,view=view,z=z,moving=moving,us=us))
                    assert us<(40000 if moving else 16667),(stage,view,z,moving,us)
                    (out/f'cost-{stage}-v{view}-z{z}-{"pan" if moving else "warm"}.txt').write_text(p.stdout+p.stderr)
(out/'render-cost.json').write_text(json.dumps(results,indent=2))
for moving in (False,True):
    a=max(r['us'] for r in results if r['stage']=='before' and r['moving']==moving)
    b=max(r['us'] for r in results if r['stage']=='after' and r['moving']==moving)
    print(f'PASS {"moving" if moving else "warm"}: peak mean before={a} after={b} us/frame across five views, z10/13/15')
