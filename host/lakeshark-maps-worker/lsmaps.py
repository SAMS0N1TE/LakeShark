"""LakeShark map worker: builds US map cells and publishes them to the map host
so the terminalbay.com globe fills in. Any number of PCs can run it at once;
they share the work through a D1 ledger on the owner's Cloudflare account.

  LakeSharkMaps.exe            build until stopped (Ctrl-C finishes the current step)
  LakeSharkMaps.exe status     overall progress and who is working
  LakeSharkMaps.exe token      enter the Cloudflare token file again
"""
import argparse
import json
import os
from pathlib import Path
import re
import shutil
import signal
import socket
import sys
import threading
import time
import traceback
import urllib.request
import uuid

FROZEN = getattr(sys, 'frozen', False)
HERE = Path(__file__).resolve().parent
ASSETS = Path(getattr(sys, '_MEIPASS', HERE))
# Frozen: CartoCore's builder rides inside the EXE. From source: point at a checkout.
CARTOCORE = ASSETS if FROZEN else Path(os.environ.get('LSMAPS_CARTOCORE', 'C:/Users/DEV/CartoCore-worker'))
sys.path.insert(0, str(CARTOCORE / 'tools'))
sys.path.insert(0, str(HERE))
if FROZEN:
    # CartoCore's tools find their repo (headers, digests of their own source) from
    # __file__, so import them from the bundled .py files, not PyInstaller's copies.
    # The frozen copies still make PyInstaller collect what they import.
    import importlib.machinery

    class _CartoCoreTools(importlib.machinery.PathFinder):
        NAMES = {'build_us', 'build_cells', 'terrarium_dem', 'tile_budget', 'index_us', 'host_parts',
                 'split_zoom', 'generalize_overview', 'make_outlines', 'portable_paths'}

        @classmethod
        def find_spec(cls, name, path=None, target=None):
            if name in cls.NAMES:
                return importlib.machinery.PathFinder.find_spec(name, [str(ASSETS / 'tools')])
            return None
    sys.meta_path.insert(0, _CartoCoreTools)

import cloud
import ledger

VERSION = '1.0.0'
SOURCE_URL = 'https://download.geofabrik.de/north-america/us-latest.osm.pbf'
MAP_HOST = 'https://preview.cartocore-maps.pages.dev/'
REGION_GIB = 4.2   # extraction budget the builder's cuts need (pilot peaks 3.78 and 3.95 GiB)
STOP_NOTE ='Stopping after the current step (press Ctrl-C again to quit at once)...'
HEADERS = '''/cells/*
  Access-Control-Allow-Methods: GET, HEAD, OPTIONS
  Access-Control-Allow-Headers: Range
  Access-Control-Expose-Headers: Content-Range, Content-Length, Accept-Ranges, ETag
  Cache-Control: public, max-age=0, must-revalidate, no-transform
'''


def get(url, headers=None, timeout=60):
    """urlopen with a real User-Agent (Cloudflare refuses Python's default one)."""
    req = urllib.request.Request(url, headers={'User-Agent': f'LakeSharkMaps/{VERSION}', **(headers or {})})
    return urllib.request.urlopen(req, timeout=timeout)


def say(message):
    print(time.strftime('%H:%M ') + message, flush=True)


def tool(name):
    """Native tools: bundled in the EXE, or beside the CartoCore checkout."""
    for path in (CARTOCORE / 'bin' / name, CARTOCORE / 'build' / name):
        if path.is_file():
            return path
    raise SystemExit(f'Missing {name} (looked in {CARTOCORE}/bin and /build)')


def default_work():
    if Path('C:/LakeSharkMaps').exists():
        return Path('C:/LakeSharkMaps')
    import ctypes
    drives = [Path(f'{c}:/') for c in 'CDEFGHIJKLMNOPQRSTUVWXYZ' if ctypes.windll.kernel32.GetDriveTypeW(f'{c}:\\') == 3]
    return max(drives, key=lambda p: shutil.disk_usage(p).free) / 'LakeSharkMaps'


# ---- setup: token, ledger, work folder ----

def connect(settings, ask=False):
    token = None if ask else cloud.read_token(settings)
    reason = None
    while True:
        if not token:
            token = cloud.ask_token(settings, reason)
        try:
            cl = cloud.Cloud(token, settings.get('account'))
            cl.verify()
            break
        except cloud.CloudError as e:
            token, reason = None, f'That token did not work ({e}).'
            settings.pop('account', None)
    settings['account'] = cl.account
    settings.setdefault('worker_id', uuid.uuid4().hex)
    cloud.save_settings(settings)
    return cl


def download_source(work, stop):
    """us-latest.osm.pbf from Geofabrik, resumable, MD5-checked (what build_us.source() expects)."""
    import hashlib
    src = work / 'src'; src.mkdir(parents=True, exist_ok=True)
    target = src / 'us-latest.osm.pbf'; part = src / 'us-latest.osm.pbf.part'
    identity = src / 'us-latest.osm.resume.json'; record = work / 'download.json'
    if target.is_file() and record.is_file() and (src / 'us-latest.osm.pbf.md5').is_file():
        return
    with get(SOURCE_URL + '.md5') as r:
        expected = r.read(4096).decode().split()[0].lower()
    if not re.fullmatch('[0-9a-f]{32}', expected):
        raise RuntimeError('Geofabrik sent an invalid MD5')
    old = json.loads(identity.read_text()) if identity.is_file() else {}
    if not target.is_file():
        if old.get('md5') != expected:
            part.unlink(missing_ok=True)   # Geofabrik published a newer file; start over
        identity.write_text(json.dumps(dict(md5=expected)))
        for attempt in range(20):
            offset = part.stat().st_size if part.exists() else 0
            try:
                with get(SOURCE_URL, {'Range': f'bytes={offset}-'} if offset else {}) as r:
                    total = offset + int(r.headers.get('Content-Length') or 0)
                    if offset and r.status != 206:
                        offset = 0
                    with part.open('ab' if offset else 'wb') as f:
                        last = 0
                        while chunk := r.read(4 * 1024 * 1024):
                            if stop.is_set():
                                raise KeyboardInterrupt
                            f.write(chunk)
                            if time.monotonic() - last > 30:
                                last = time.monotonic()
                                say(f'Downloading US map data: {f.tell() / 1e9:.2f} of {total / 1e9:.2f} GB')
                break
            except (OSError, TimeoutError) as e:
                if attempt == 19:
                    raise
                say(f'Download interrupted ({e}); resuming'); time.sleep(10)
        say('Checking the download (MD5)...')
        md5 = hashlib.md5()
        with part.open('rb') as f:
            while chunk := f.read(8 * 1024 * 1024):
                md5.update(chunk)
        if md5.hexdigest() != expected:
            part.unlink()
            raise RuntimeError('Download was corrupt; deleted it. Run again to download afresh.')
        os.replace(part, target)
    else:
        expected = old.get('md5', expected)
    (src / 'us-latest.osm.pbf.md5').write_text(expected + '  us-latest.osm.pbf\n')
    record.write_text(json.dumps(dict(url=SOURCE_URL, bytes=target.stat().st_size, md5=expected), indent=2) + '\n')


# ---- building with CartoCore's US builder ----

def make_builder(work, jobs, stop):
    """The builder, once this PC has the memory its guard asks for."""
    while True:
        try:
            return _make_builder(work, jobs)
        except RuntimeError as e:
            if not low_memory(e) or stop.is_set():
                raise
            wait_for_memory(stop)
            if stop.is_set():
                raise InterruptedError('stopped while waiting for memory')


def _make_builder(work, jobs):
    import hashlib
    import build_us as us
    a = argparse.Namespace(root=work, pbf=work / 'src/us-latest.osm.pbf', engine=tool('cartocore.exe'),
                           validator=tool('cartocore_validate_cells.exe'), osmium=tool('osmium.exe'),
                           admin1=CARTOCORE / 'data/ne/ne_10m_admin_1_states_provinces.geojson',
                           admin0=CARTOCORE / 'data/ne/ne_10m_admin_0_countries.geojson',
                           places=None, ram_gib=11, jobs=jobs, extract_ram_gib=5, extract_pass_outputs=4,
                           prune_extracts=False, split_zoom=False, host_part_bytes=us.PART_BYTES)
    b = us.Build(a)
    log = b.logpath
    def quiet(message):   # the full log stays in the work folder; the console gets the milestones
        text = time.strftime('%Y-%m-%dT%H:%M:%S%z') + ' ' + message
        with log.open('a', encoding='utf8') as f:
            f.write(text + '\n')
        if re.search(r': verified \d+ cells|SKIP|failed|RAM guard|Extraction plan|Memory:', message):
            say(message if len(message) < 220 else message[:220] + '...')
    b.log = quiet
    b.memory_policy()
    source = b.source()
    bc, REPO = us.bc, us.REPO
    identity = dict(source=source['sha256'], admin1=bc.digest(a.admin1), admin0=bc.digest(a.admin0),
                    engine=bc.digest(a.engine), validator=bc.digest(a.validator),
                    scripts={p.name: bc.digest(p) for p in [Path(us.__file__), REPO/'tools'/'build_cells.py', REPO/'tools'/'terrarium_dem.py',
                                                            REPO/'tools'/'index_us.py', REPO/'tools'/'generalize_overview.py', REPO/'tools'/'tile_budget.py']},
                    halo=us.HALO, extraction='smart', zstd=us.zstd.__version__)
    b.tile_signature = hashlib.sha256(json.dumps(dict(source=source['sha256'], writer=bc.digest(REPO/'src/ingest/pbf.c'),
        build_cells=bc.digest(REPO/'tools'/'build_cells.py'), tile_budget=bc.digest(REPO/'tools'/'tile_budget.py'),
        terrain=bc.digest(REPO/'tools'/'terrarium_dem.py'), geometry=bc.digest(REPO/'tools'/'index_us.py'), halo=us.HALO,
        zstd=us.zstd.__version__), sort_keys=True).encode()).hexdigest()
    b.signature = hashlib.sha256(json.dumps(identity, sort_keys=True).encode()).hexdigest()
    b.overview_signature = None
    us.atomic_json(work / 'config.json', dict(signature=b.signature, **identity))
    return us, b, source


def plan_blocks(us, b):
    states, plan = us.make_plan(b.a.admin1, b.a.admin0)
    return states, plan


def package(us, b, results, stage, source_sha):
    """Cut each verified cell into the host's 2 MiB parts, as the builder's assembly does (unsplit)."""
    out = []
    for result in results:
        for r in result['files']:
            if r['cell'] == 'overview':
                continue
            _, x, y = map(int, r['cell'].split('/'))
            owner = ledger.CELL_ID(x, y)
            src = b.root / 'blocks' / result['block'] / r['file']
            record = us.host_asset(src, stage / 'cells' / '8' / str(x) / f'{y}.ctile', stage, owner, r['sha256'], us.PART_BYTES)
            out.append(dict(id=owner, x=x, y=y, bytes=record['bytes'], sha256=record['sha256'], record=record,
                            block=result['block'], source=source_sha))
    return out


class Heart(threading.Thread):
    """Renews this PC's claims and tells the site what it is doing."""
    def __init__(self, led):
        super().__init__(daemon=True)
        self.led = led; self.status = 'idle'; self.now = []; self.detail = ''; self.wake = threading.Event()

    def set(self, status, now=None, detail=''):
        self.status, self.detail = status, detail
        if now is not None:
            self.now = now
        self.wake.set()

    def run(self):
        while True:
            try:
                self.led.heartbeat(self.status, self.now, self.detail)
            except Exception as e:
                say(f'(heartbeat failed: {e})')
            self.wake.wait(120); self.wake.clear()


def low_memory(error):
    return bool(re.search(r'RAM guard|GiB available|available RAM', str(error)))


def wait_for_memory(stop, minutes=10):
    import psutil
    free = psutil.virtual_memory().available / 2**30
    say(f'Not enough free memory on this PC ({free:.1f} GB free; one job needs about 6 GB). '
        f'Close other programs to start sooner. Trying again in {minutes} minutes.')
    stop.wait(minutes * 60)


def progress_line(led):
    counts, cells, size = led.counts()
    cov = led.meta('coverage').get('coverage') or {}
    total = len(cov.get('cells', [])) or 1140
    return f'Overall: {cells} of {total} cells built ({100 * cells / total:.1f}%), {size / 1e9:.1f} GB on the map host'


def run(args, settings):
    stop = threading.Event()
    def on_signal(signum, frame):
        if stop.is_set():
            say('Quitting now.'); os._exit(130)
        stop.set(); say(STOP_NOTE)
        (work / 'STOP').touch()
    work = Path(args.work or settings.get('work') or default_work()).resolve()
    work.mkdir(parents=True, exist_ok=True)
    (work / 'STOP').unlink(missing_ok=True)
    settings['work'] = str(work); cloud.save_settings(settings)
    signal.signal(signal.SIGINT, on_signal)
    cl = connect(settings)
    cl.open_database()
    led = ledger.Ledger(cl, settings['worker_id'], socket.gethostname())
    say(f'LakeShark map worker {VERSION} on {led.name}; work folder {work}')
    free = shutil.disk_usage(work).free / 1e9
    if free < 40:
        raise SystemExit(f'Only {free:.0f} GB free in {work}; the US build needs about 60 GB. Use --work on a bigger drive.')
    heart = Heart(led); heart.start()
    heart.set('downloading', detail='US map data')
    say(progress_line(led))
    download_source(work, stop)
    if stop.is_set():
        return
    heart.set('extracting', detail='checking the source')
    say('Verifying the map data (takes a few minutes the first time)...')
    try:
        us, b, source = make_builder(work, args.jobs, stop)
    except InterruptedError:
        led.heartbeat('stopped', [], ''); return
    states, plan = plan_blocks(us, b)
    blocks = {blk['id']: blk for blk in plan['blocks']}
    # Every PC must cut the nation into the same blocks.
    remote = {r['id']: json.loads(r['cells']) for r in cl.rows('SELECT id, cells FROM blocks')}
    if set(remote) != {k for k, v in blocks.items() if v['build_cells']}:
        raise SystemExit('This build plan differs from the shared ledger; update LakeSharkMaps.exe')
    stage = work / 'stage'; stage.mkdir(exist_ok=True)
    pending_path = work / 'unpublished.json'
    heavy_path = work / 'too-big-here.json'   # blocks this PC ran out of memory on: {id: GB needed}
    heavy = json.loads(heavy_path.read_text()) if heavy_path.is_file() else {}
    published = 0
    def flush():
        """Anything built here but not yet live goes out (also after a crash or a stop)."""
        nonlocal published
        pending = json.loads(pending_path.read_text()) if pending_path.is_file() else []
        if not pending:
            return False
        heart.set('publishing', detail=f'{len(pending)} cells')
        say(f'Publishing {len(pending)} cells to the map host...')
        for attempt in range(3):
            try:
                url, coverage = led.publish(stage, pending, say=say)
                break
            except (cloud.CloudError, OSError) as e:
                if attempt == 2:
                    raise
                say(f'Publish failed ({e}); trying again in a minute. The cells stay safe here.')
                time.sleep(60)
        pending_path.unlink(); published += len(pending)
        led.heartbeat('publishing', heart.now, '', cells=len(pending))
        say(f'Live: {len(pending)} new cells ({url})')
        say(progress_line(led))
        shutil.rmtree(stage, ignore_errors=True); stage.mkdir()
        return True
    flush()
    while not stop.is_set() and not (args.once and published):
        try:
            b.a.jobs = args.jobs
            b.memory_policy()   # memory now, not at startup
        except RuntimeError as e:
            if not low_memory(e):
                raise
            heart.set('waiting', [], 'low memory'); wait_for_memory(stop); continue
        built = led.built_ids()
        regions = b.region_plan(plan['blocks'])
        local = [r for r in regions if (work / 'scratch/regions' / (r['id'] + '.osm.pbf')).is_file()]
        preferred = {bid for r in local for bid in r['blocks']}
        if b.extract_budget < REGION_GIB:
            # Both extraction steps peak near 4 GB; with less, only blocks already cut out here.
            preferred = {p.name.split('.')[0] for p in (work / 'scratch/blocks').glob('b*.osm.pbf')}
            if not preferred:
                heart.set('waiting', [], 'low memory'); wait_for_memory(stop); continue
        too_big = {k for k, need in heavy.items() if need > b.worker_ram_gib}
        claims = led.claim(max(1, b.a.jobs), preferred, only=b.extract_budget < REGION_GIB, exclude=too_big)
        if not claims and b.extract_budget < REGION_GIB:
            heart.set('waiting', [], 'low memory'); wait_for_memory(stop); continue
        if not claims:
            counts, cells, _ = led.counts()
            if counts.get('claimed') or counts.get('built'):
                heart.set('idle', [], 'waiting')
                say('Nothing left to claim; other PCs are finishing the last blocks. Checking again in 10 minutes.')
                stop.wait(600); continue
            if counts.get('todo') and too_big:
                heart.set('idle', [], 'waiting')
                say(f'The {counts["todo"]} blocks left need more memory than this PC has free; '
                    'a PC with more RAM will take them. Checking again in 30 minutes.')
                stop.wait(1800); continue
            say('Every block is built or parked. Done.')
            if counts.get('failed'):
                say(f'{counts["failed"]} blocks failed three times; see "status".')
            break
        jobs = []
        for c in claims:
            blk = dict(blocks[c['id']])
            blk['build_cells'] = [cell for cell in c['cells'] if ledger.CELL_ID(*cell) not in built]
            if blk['build_cells']:
                jobs.append(blk)
            else:   # every cell already live (built by an older run)
                cl.sql("UPDATE blocks SET status = 'done', updated = ? WHERE id = ?", ledger.now(), c['id'])
        claims = [c for c in claims if any(j['id'] == c['id'] for j in jobs)]
        if not jobs:
            continue
        names = sorted({s for c in claims for s in c['states']})
        ncells = sum(len(j['build_cells']) for j in jobs)
        heart.set('extracting', names, f'{len(jobs)} blocks')
        say(f'Claimed {", ".join(c["id"] for c in claims)} ({ncells} cells in {", ".join(names)})')
        try:
            say('Cutting the source down to these blocks...')
            b.extract_all(jobs, plan['blocks'])
            heart.set('building', names, f'{ncells} cells')
            say(f'Building {ncells} cells (each takes minutes; detail in {b.logpath})...')
            results, failures, state = [], [], dict(status='building')
            us.build_pending(b, jobs, state, results, failures, lambda: None)
        except Exception as e:
            if stop.is_set() or isinstance(e, InterruptedError):
                led.release([c['id'] for c in claims]); break
            if low_memory(e):   # this PC's problem, not the block's: no strike
                led.release([c['id'] for c in claims])
                wait_for_memory(stop); continue
            led.release([c['id'] for c in claims], error=e)
            say(f'Batch failed: {e}'); traceback.print_exc()
            stop.wait(30); continue
        done = {r['block'] for r in results}
        starved = [f for f in failures if low_memory(f['error'])]
        failures = [f for f in failures if f not in starved]
        if starved:
            # Remember what the block needed; this PC leaves it to PCs with more memory.
            for f in starved:
                peak = max([m['peak_rss'] for m in b.metrics if m['stage'].startswith(f['block'] + '/')] or [0])
                heavy[f['block']] = round(max(peak / 2**30 * 1.1, b.worker_ram_gib + 0.5), 2)
                say(f'{f["block"]} needs about {heavy[f["block"]]:.1f} GB for one cell; leaving it to a PC with more free memory.')
            heavy_path.write_text(json.dumps(heavy, indent=1))
            led.release([f['block'] for f in starved])
        if failures:
            for f in failures:
                say(f'Block {f["block"]} failed: {f["error"]}')
            led.release([f['block'] for f in failures], error='; '.join(f['error'] for f in failures))
        led.release([c['id'] for c in claims if c['id'] not in done and c['id'] not in {f['block'] for f in failures}])
        if not results:
            continue
        new = package(us, b, results, stage, source['sha256'])
        led.mark_built(sorted(done))
        old = json.loads(pending_path.read_text()) if pending_path.is_file() else []
        pending_path.write_text(json.dumps(old + new))
        flush()
    heart.set('stopped' if stop.is_set() else 'idle', [], '')
    led.heartbeat(heart.status, [], '')
    say(f'Stopped. This run published {published} cells.')


# ---- status ----

def status(args, settings):
    cl = connect(settings); cl.open_database()
    led = ledger.Ledger(cl, settings['worker_id'], socket.gethostname())
    counts, cells, size = led.counts()
    print(progress_line(led))
    print('Blocks: ' + ', '.join(f'{k} {v}' for k, v in sorted(counts.items())))
    t = ledger.now()
    for w in cl.rows('SELECT name, status, now, detail, cells, seen FROM workers ORDER BY seen DESC'):
        age = (t - (w['seen'] or 0)) // 60
        print(f'  {w["name"]:<20} {w["status"]:<11} {", ".join(json.loads(w["now"] or "[]")):<16} '
              f'{w["cells"]:>5} cells published, seen {age} min ago')
    for r in cl.rows("SELECT id, attempts, error FROM blocks WHERE status = 'failed' OR (status = 'todo' AND attempts > 0)"):
        print(f'  {r["id"]}: {r["attempts"]} failed attempts: {r["error"]}')
    dep = led.meta('deploy').get('deploy')
    if dep:
        print(f'Last publish: {time.strftime("%Y-%m-%d %H:%M", time.localtime(dep["at"]))} by {dep["by"]} ({dep["url"]})')


# ---- check: the ledger against what the map host serves ----

def check(args, settings):
    """Regenerate the host's global files from the ledger (no upload) and compare
    them, and every other file the ledger lists, with what the host serves."""
    import tempfile
    cl = connect(settings); cl.open_database()
    led = ledger.Ledger(cl, settings['worker_id'], socket.gethostname())
    host = args.host.rstrip('/') + '/'
    with tempfile.TemporaryDirectory(prefix='LakeSharkMaps-check-') as tmp:
        files, coverage = led.publish(Path(tmp), [], dry_run=True)
        live = json.loads(get(host + 'manifest.json').read())
        served = {f['file']: f for f in live['files']}
        problems = 0
        for name in ('cells/catalog.ccm', 'cells/catalog.json', 'cells/states.json'):
            ours = (Path(tmp) / 'global' / name).read_bytes()
            theirs = get(host + name).read()
            if name.endswith('.json'):
                same = json.loads(ours) == json.loads(theirs)
            else:
                same = ours == theirs
            print(f'{name:<22} {"same as live" if same else "DIFFERS from live"}')
            problems += not same
        carried = {p: f for p, f in files.items() if p not in ledger.GLOBAL_FILES}
        gone = sorted(set(served) - set(carried) - set(ledger.GLOBAL_FILES))
        added = sorted(set(carried) - set(served))
        changed = sorted(p for p in carried if p in served and served[p]['sha256'] != carried[p]['sha256'])
        print(f'{len(carried)} carried files: {len(gone)} live files missing from the ledger, '
              f'{len(added)} not live yet, {len(changed)} with different content')
        for p in (gone + changed)[:10]:
            print('  ' + p)
        problems += len(gone) + len(changed)
        print(f'Coverage: {sum(c[2] for c in coverage["cells"])} of {len(coverage["cells"])} cells')
    if problems:
        raise SystemExit('CHECK FAILED')
    print('CHECK PASS')


def publish_now(args, settings):
    """Publish this PC's unpublished cells now, or refresh the host's index files."""
    cl = connect(settings); cl.open_database()
    led = ledger.Ledger(cl, settings['worker_id'], socket.gethostname())
    work = Path(args.work or settings.get('work') or default_work())
    pending_path = work / 'unpublished.json'
    pending = json.loads(pending_path.read_text()) if pending_path.is_file() else []
    say(f'Publishing {len(pending)} cells...')
    url, coverage = led.publish(work / 'stage', pending, say=say)
    pending_path.unlink(missing_ok=True)
    say(f'Live: {url}'); say(progress_line(led))


# ---- selftest: a tiny offline cell, built twice, must match ----

def selftest(args, settings):
    import subprocess
    import tempfile
    from portable_paths import python_command
    fixture = CARTOCORE / ('data/selftest' if FROZEN else 'portable/selftest')
    config = json.loads((fixture / 'fixture.json').read_text())
    hashes = []
    with tempfile.TemporaryDirectory(prefix='LakeSharkMaps-selftest-') as tmp:
        for i in range(2):
            out = Path(tmp) / str(i)
            env = dict(os.environ, CC_SELFTEST_DEM=str(fixture / 'terrain.tif'))
            subprocess.run(python_command(CARTOCORE / 'tools' / 'build_cells.py', '--engine', tool('cartocore.exe'),
                           '--pbf', fixture / 'tiny.osm.pbf', '--cells', config['cell'], '--terrain', 'aws', '--out', out),
                           env=env, check=True, stdout=subprocess.DEVNULL)
            cell = out / (out / 'CURRENT').read_text().strip() / (config['cell'].replace('/', '-') + '.ctile')
            subprocess.run([str(tool('cartocore_validate_cells.exe')), str(cell)], check=True, stdout=subprocess.DEVNULL)
            hashes.append(ledger.sha256_file(cell))
    if hashes[0] != hashes[1]:
        raise SystemExit('SELFTEST FAIL: two builds of the same cell differ')
    same = 'matches' if hashes[0] == config.get('reference_sha256') else 'differs from'
    import build_us as us   # the US builder and its data load, and plan the nation as the ledger does
    states, plan = us.make_plan(CARTOCORE / 'data/ne/ne_10m_admin_1_states_provinces.geojson',
                                CARTOCORE / 'data/ne/ne_10m_admin_0_countries.geojson')
    subprocess.run([str(tool('osmium.exe')), '--version'], check=True, stdout=subprocess.DEVNULL)
    print(f'SELFTEST PASS: {hashes[0][:16]} (repeatable; {same} the CartoCore reference build); '
          f'US plan {plan["cell_count"]} cells in {sum(1 for b in plan["blocks"] if b["build_cells"])} blocks; osmium runs')


# ---- bootstrap: the ledger from what the map host serves today (owner, once) ----

def bootstrap(args, settings):
    import hashlib
    cl = connect(settings); cl.open_database(create=True)
    for statement in ledger.SCHEMA:
        cl.sql(statement)
    if cl.rows('SELECT count(*) AS n FROM cells')[0]['n'] and not args.force:
        raise SystemExit('The ledger already holds cells; bootstrap runs once (use --force to rebuild it from the host)')
    host = args.host.rstrip('/') + '/'
    cache = Path(args.work or default_work()) / 'bootstrap'; cache.mkdir(parents=True, exist_ok=True)
    def fetch(name):
        path = cache / name; path.parent.mkdir(parents=True, exist_ok=True)
        if not path.is_file():
            with get(host + name, timeout=120) as r, path.open('wb') as f:
                shutil.copyfileobj(r, f)
        return path
    manifest = json.loads(fetch('manifest.json').read_text(encoding='utf8'))
    catalog = json.loads(fetch('cells/catalog.json').read_text(encoding='utf8'))
    states_live = json.loads(fetch('cells/states.json').read_text(encoding='utf8'))
    header = fetch('cells/catalog.ccm').read_bytes()[:128]
    if header[:8] != b'CCMAPS2\0':
        raise SystemExit('The host catalog is not CCMAPS2')
    rows = []
    say(f'Hashing {len(manifest["files"])} live files ({manifest["total_bytes"] / 1e6:.0f} MB to download once)...')
    for i, f in enumerate(manifest['files']):
        if f['file'] in ledger.GLOBAL_FILES:
            continue
        path = fetch(f['file'])
        sha = ledger.sha256_file(path)
        if sha != f['sha256'] or path.stat().st_size != f['bytes']:
            path.unlink(); raise SystemExit(f'{f["file"]} on the host differs from its manifest; refetch')
        rows.append(dict(p=f['file'], h=cloud.pages_hash(path), s=sha, b=f['bytes']))
        if i % 50 == 0:
            say(f'  {i}/{len(manifest["files"])}')
    jwt = cl.upload_token()
    missing = cl.missing(jwt, {r['h'] for r in rows} | {'0' * 32})
    if missing != {'0' * 32}:
        raise SystemExit(f'Hash check failed: Cloudflare lacks {len(missing) - 1} live files; not bootstrapping')
    states = {code: {k: s[k] for k in ('name', 'cells', 'bbox', 'tier', 'partial') if k in s}
              for code, s in states_live.items() if len(code) == 2 and isinstance(s, dict)}
    # Blocks come from the builder's own plan; they must cover exactly the states' cells.
    import build_us as us
    plan_states, plan = us.make_plan(CARTOCORE / 'data/ne/ne_10m_admin_1_states_provinces.geojson',
                                     CARTOCORE / 'data/ne/ne_10m_admin_0_countries.geojson')
    want = {tuple(c) for s in states.values() for c in s['cells']}
    have = {tuple(c) for blk in plan['blocks'] for c in blk['build_cells']}
    if want != have:
        raise SystemExit(f'Plan and host states disagree on {len(want ^ have)} cells')
    built = {r['id'] for r in catalog['cells']}
    owners = {}
    for code, s in states.items():
        for c in s['cells']:
            owners.setdefault(tuple(c), set()).add(code)
    statements = [('DELETE FROM files', []), ('DELETE FROM cells', []), ('DELETE FROM blocks', [])]
    for i in range(0, len(rows), 400):
        statements.append(('''INSERT INTO files (path, hash, sha256, bytes) SELECT json_extract(value, '$.p'),
            json_extract(value, '$.h'), json_extract(value, '$.s'), json_extract(value, '$.b') FROM json_each(?)''',
            [json.dumps(rows[i:i + 400])]))
    for r in catalog['cells']:
        x, y = ((r['id'] - 1) // 256, (r['id'] - 1) % 256) if r['id'] else (-1, -1)
        statements.append(('INSERT INTO cells (id, x, y, bytes, sha256, record, worker, source, built) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)',
                           [r['id'], x, y, r['bytes'], r['sha256'], json.dumps(r, sort_keys=True), 'bootstrap',
                            manifest.get('source', {}).get('sha256'), ledger.now()]))
    for order, blk in enumerate(plan['blocks']):
        if not blk['build_cells']:
            continue
        cells = [list(c) for c in blk['build_cells']]
        names = sorted({code for c in cells for code in owners.get(tuple(c), ())})
        done = all(ledger.CELL_ID(*c) in built for c in cells)
        statements.append(('INSERT INTO blocks (id, ord, cells, states, status, updated) VALUES (?, ?, ?, ?, ?, ?)',
                           [blk['id'], order, json.dumps(cells), json.dumps(names), 'done' if done else 'todo', ledger.now()]))
    meta = dict(states=states, outlines=states_live['outlines'], catalog_header=header.hex(), headers=HEADERS,
                part_bytes=catalog['host_part_bytes'], bootstrap=dict(host=host, at=ledger.now(), files=len(rows)))
    for k, v in meta.items():
        statements.append(('INSERT INTO meta (k, v) VALUES (?, ?) ON CONFLICT(k) DO UPDATE SET v = excluded.v',
                           [k, json.dumps(v, sort_keys=True)]))
    for i in range(0, len(statements), 40):
        cl.batch(statements[i:i + 40])
    led = ledger.Ledger(cl, settings['worker_id'], socket.gethostname())
    counts, cells, size = led.counts()
    say(f'Ledger ready: {len(rows)} live files, {cells} cells built, blocks {counts}')


def main():
    if len(sys.argv) > 2 and sys.argv[1] == '_worker':
        # The builder re-enters this EXE for its Python helpers.
        import runpy
        signal.signal(signal.SIGINT, signal.SIG_IGN)
        name = sys.argv[2]
        if name not in ('build_cells', 'terrarium_dem', 'generalize_overview', 'make_outlines'):
            raise SystemExit('Unknown worker')
        sys.argv = [str(ASSETS / 'tools' / (name + '.py')), *sys.argv[3:]]
        runpy.run_path(sys.argv[0], run_name='__main__'); return
    ap = argparse.ArgumentParser(prog='LakeSharkMaps', description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('command', nargs='?', default='run', choices=('run', 'status', 'token', 'selftest', 'check', 'publish', 'bootstrap'))
    ap.add_argument('--work', help='Work folder (about 60 GB); remembered for next time')
    ap.add_argument('--jobs', type=int, default=2, help='Blocks built at once (RAM permitting; default 2)')
    ap.add_argument('--once', action='store_true', help='Stop after the first publish')
    ap.add_argument('--host', default=MAP_HOST, help='bootstrap/check: the map host')
    ap.add_argument('--force', action='store_true', help='bootstrap: rebuild the ledger from the host')
    args = ap.parse_args()
    settings = cloud.load_settings()
    if args.command == 'token':
        connect(settings, ask=True); print('Saved.'); return
    {'run': run, 'status': status, 'selftest': selftest, 'check': check, 'publish': publish_now,
     'bootstrap': bootstrap}[args.command](args, settings)


if __name__ == '__main__':
    interactive = FROZEN and len(sys.argv) == 1
    try:
        main()
    except SystemExit:
        if interactive:
            input('Press Enter to close.')
        raise
    except Exception as e:
        print(f'FAILED: {e}', flush=True)
        if os.environ.get('LSMAPS_DEBUG'):
            traceback.print_exc()
        if interactive:
            input('Press Enter to close.')
        raise SystemExit(1)
    if interactive:
        input('Press Enter to close.')
