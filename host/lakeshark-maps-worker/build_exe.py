"""Build dist/LakeSharkMaps.exe: one file, no Python needed on the target PC.

Inputs (defaults are this workshop PC's paths):
  --cartocore  CartoCore checkout on work/maps-worker-20261023 (US builder + portable launcher)
  --osmium     osmium.exe from conda-forge osmium-tool (its DLLs are collected beside it)
  --mingw      MSYS2 UCRT64 (gcc 15, mingw32-make, objdump) for the native tools
  --ne         folder with Natural Earth ne_10m_admin_0_countries / admin_1 GeoJSON

Run it with a Python that has requirements.txt installed:
  python build_exe.py
"""
import argparse
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
TOOLS = ('build_us.py', 'build_cells.py', 'terrarium_dem.py', 'tile_budget.py', 'index_us.py', 'host_parts.py',
         'split_zoom.py', 'generalize_overview.py', 'make_outlines.py', 'portable_paths.py')


def run(*args, **kw):
    print('>', ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True, **kw)


def closure(exe, dest, objdump):
    """Copy an executable and every non-system DLL it imports, recursively."""
    queue, seen = [exe], set()
    system = Path(os.environ['SystemRoot']) / 'System32'
    while queue:
        path = queue.pop()
        if path.name.lower() in seen:
            continue
        seen.add(path.name.lower()); shutil.copy2(path, dest / path.name)
        text = subprocess.check_output([str(objdump), '-p', str(path)], text=True, errors='replace')
        for name in re.findall(r'DLL Name:\s*(\S+)', text):
            if name.lower() in seen or name.lower().startswith(('api-ms-', 'ext-ms-')):
                continue
            for folder in (exe.parent, objdump.parent):
                if (folder / name).is_file():
                    queue.append(folder / name); break
            else:
                if not (system / name).is_file():
                    raise SystemExit(f'Unresolved DLL for {path.name}: {name}')
    return seen


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--cartocore', type=Path, default=Path('C:/Users/DEV/CartoCore-worker'))
    ap.add_argument('--osmium', type=Path, default=Path('C:/Users/DEV/.lsmaps-build/osm/Library/bin/osmium.exe'))
    ap.add_argument('--mingw', type=Path, default=Path('C:/msys64/ucrt64'))
    ap.add_argument('--cmake', default=shutil.which('cmake') or 'C:/Program Files/CMake/bin/cmake.exe')
    ap.add_argument('--ne', type=Path, default=Path('C:/Users/DEV/CartoCore/data/places/cache'))
    ap.add_argument('--skip-native', action='store_true', help='Reuse CARTOCORE/build executables')
    a = ap.parse_args()
    cc = a.cartocore.resolve()
    env = dict(os.environ, PATH=str(a.mingw / 'bin') + os.pathsep + os.environ['PATH'])
    if not a.skip_native:
        run(a.cmake, '-S', cc, '-B', cc / 'build', '-G', 'MinGW Makefiles', '-DCMAKE_BUILD_TYPE=Release',
            f'-DCMAKE_C_COMPILER={a.mingw / "bin/gcc.exe"}', f'-DCMAKE_MAKE_PROGRAM={a.mingw / "bin/mingw32-make.exe"}', env=env)
        run(a.cmake, '--build', cc / 'build', '--target', 'cartocore_cli', 'cartocore_validate_cells', '-j', '8', env=env)
    # Everything the EXE carries, laid out the way the builder expects its REPO.
    pack = HERE / 'build' / 'payload'
    shutil.rmtree(pack, ignore_errors=True)
    (pack / 'bin').mkdir(parents=True); (pack / 'tools').mkdir(); (pack / 'data/ne').mkdir(parents=True)
    objdump = a.mingw / 'bin/objdump.exe'
    for name in ('cartocore.exe', 'cartocore_validate_cells.exe'):
        closure(cc / 'build' / name, pack / 'bin', objdump)
    closure(a.osmium, pack / 'bin', objdump)
    clean = dict(os.environ, PATH=str(Path(os.environ['SystemRoot']) / 'System32'))
    run(pack / 'bin/osmium.exe', '--version', env=clean, stdout=subprocess.DEVNULL)
    shutil.copytree(cc / 'portable/selftest', pack / 'data/selftest')
    for name in TOOLS:
        shutil.copy2(cc / 'tools' / name, pack / 'tools' / name)
    (pack / 'src/ingest').mkdir(parents=True); shutil.copy2(cc / 'src/ingest/pbf.c', pack / 'src/ingest/pbf.c')
    (pack / 'include/cartocore').mkdir(parents=True)
    shutil.copy2(cc / 'include/cartocore/zoom_policy.h', pack / 'include/cartocore/zoom_policy.h')
    for name in ('ne_10m_admin_0_countries.geojson', 'ne_10m_admin_1_states_provinces.geojson'):
        shutil.copy2(a.ne / name, pack / 'data/ne' / name)
    commit = subprocess.check_output(['git', '-C', str(cc), 'rev-parse', 'HEAD'], text=True).strip()
    (pack / 'CARTOCORE_COMMIT.txt').write_text(commit + '\n')
    adds = []
    for item in ('bin', 'tools', 'src', 'include', 'data', 'CARTOCORE_COMMIT.txt'):
        adds += ['--add-data', f'{pack / item}{os.pathsep}{item if (pack / item).is_dir() else "."}']
    hidden = []
    for name in ('build_us', 'build_cells', 'terrarium_dem', 'tile_budget', 'index_us', 'host_parts', 'split_zoom',
                 'generalize_overview', 'make_outlines', 'portable_paths', 'tifffile', 'blake3',
                 'PIL.TiffImagePlugin', 'PIL.PngImagePlugin'):
        hidden += ['--hidden-import', name]
    run(sys.executable, '-m', 'PyInstaller', '--noconfirm', '--clean', '--onefile', '--console', '--name', 'LakeSharkMaps',
        '--distpath', HERE / 'dist', '--workpath', HERE / 'build/pyinstaller', '--specpath', HERE / 'build',
        '--paths', pack / 'tools', '--paths', HERE, *hidden,
        '--exclude-module', 'matplotlib', '--exclude-module', 'pytest', '--exclude-module', 'IPython',
        '--exclude-module', 'tkinter', '--exclude-module', 'imagecodecs',
        '--collect-all', 'zstandard', '--collect-all', 'psutil', *adds, HERE / 'lsmaps.py')
    exe = HERE / 'dist/LakeSharkMaps.exe'
    # Smoke test from a clean PATH: the EXE starts, finds its bundled tools and
    # builds the tiny offline cell twice (this also exercises its helper re-entry).
    for args in (['--help'], ['selftest']):
        out = subprocess.run([str(exe), *args], env=clean, capture_output=True, text=True)
        print(out.stdout[-600:])
        if out.returncode:
            print(out.stderr[-3000:]); raise SystemExit('Smoke test failed: ' + ' '.join(args))
    print(f'Built {exe} ({exe.stat().st_size / 1e6:.0f} MB), CartoCore {commit[:10]}')


if __name__ == '__main__':
    main()
