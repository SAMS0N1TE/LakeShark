"""Round 4 evidence, using identical recipes/compiler for the round 3 baseline and worktree.

python bench/tools/l3bench_round4.py [--baseline REV] [--runs 5]
Outputs only to ignored bench/build-lap3d-round4. Does not run other tests.
"""
import argparse
import json
import statistics
import os
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
ENGINE = ROOT / 'components/apps/tui/lap3d'
OUT = ROOT / 'bench/build-lap3d-round4'


def run(args, **kwargs):
    p = subprocess.run(list(map(str, args)), capture_output=True, text=True, **kwargs)
    if p.returncode:
        raise RuntimeError(p.stdout + p.stderr)
    return p.stdout


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline', default='93ca3f27')
    parser.add_argument('--runs', type=int, default=5)
    parser.add_argument('--target', action='store_true', help='audit P4 sizes and stack from the existing compile database')
    parser.add_argument('--profile', action='store_true', help='capture 50-frame kind timings and scene memory')
    args = parser.parse_args()
    original = OUT / 'original'
    original.mkdir(parents=True, exist_ok=True)
    files = run(['git', 'ls-tree', '-r', '--name-only', args.baseline,
                 'components/apps/tui/lap3d']).splitlines()
    for name in files:
        text = run(['git', 'show', args.baseline + ':' + name])
        text = text.replace('../../../../app-sdk/include/lap3d.h',
                            (ROOT / 'app-sdk/include/lap3d.h').as_posix())
        (original / Path(name).name).write_text(text)
    samples, stages, picture = {}, {}, []
    for label, source in [('before', original), ('after', ENGINE)]:
        out = OUT / label
        out.mkdir(exist_ok=True)
        (out / 'dumps').mkdir(exist_ok=True)
        exe = out / 'l3bench.exe'
        flags = ['gcc', '-O2', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                 '-Wno-unused-parameter', '-Wdouble-promotion',
                 '-I' + str(ROOT / 'app-sdk/include'), '-I' + str(ROOT / 'bench/tools')]
        run(flags + list(source.glob('*.c')) + [ROOT / 'bench/tools/l3bench.c',
             ROOT / 'bench/shims/lap3d_io.c', '-lm', '-o', exe])
        for _ in range(args.runs):
            text = run([exe, out, out / 'dumps'])
            for line in text.splitlines():
                f = line.split()
                if f and f[0] in ('portrait', 'landscape'):
                    samples.setdefault((label, f[0], f[1]), []).append(int(f[2]))
                    for index, stage in enumerate(('shadow', 'raster', 'shade', 'resolve'), 3):
                        stages.setdefault((label, f[0], f[1], stage), []).append(int(f[index]))
        (out / 'timing.txt').write_text(text)
        if args.profile or args.target:
            from l3bench_audit import host, target
        if args.profile:
            dest = OUT / (label + '-profile')
            exe = host(source, dest, profile=True)
            env = dict(os.environ, L3BENCH_FRAMES='50')
            (dest / 'profile.txt').write_text(run([exe, dest], env=env))
        if args.target:
            database = ROOT / 'build_tdp4_lronly/compile_commands.json'
            target(source, out / 'target', json.loads(database.read_text()))
    for o in range(2):
        for scene in range(14):
            name = f'{o}-{scene}.bin'
            a = (OUT / 'before/dumps' / name).read_bytes()
            b = (OUT / 'after/dumps' / name).read_bytes()
            n = len(a) // 2
            picture.append(dict(orientation=o, scene=scene, dots=sum(x != y for x, y in zip(a[:n], b[:n])),
                                tags=sum(x != y for x, y in zip(a[n:], b[n:]))))
    report = {'baseline': run(['git', 'rev-parse', args.baseline]).strip(),
              'median_us': {' '.join(k): statistics.median(v) for k, v in samples.items()},
              'median_stages_us': {' '.join(k): statistics.median(v) for k, v in stages.items()},
              'picture_differences': picture}
    (OUT / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps(report, indent=2))


if __name__ == '__main__':
    main()
