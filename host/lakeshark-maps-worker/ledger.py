"""The shared ledger in D1 and the incremental map-host publish.

Every PC claims blocks of z8 cells from D1, builds them locally, and publishes
only its new cell parts. A publish takes the D1 lock, rebuilds the small
global files (catalog, states, coverage) from the ledger, uploads whatever
Cloudflare does not already hold, and deploys the full manifest, so no PC ever
needs the other PCs' files and nothing already live is dropped.
"""
import hashlib
import json
import os
from pathlib import Path
import struct
import time

import cloud

LEASE = 2 * 3600          # a claimed block returns to the pool after this without a heartbeat
LOCK = 30 * 60            # publish lock lifetime
MAX_ATTEMPTS = 3

SCHEMA = [
    'CREATE TABLE IF NOT EXISTS meta (k TEXT PRIMARY KEY, v TEXT NOT NULL)',
    '''CREATE TABLE IF NOT EXISTS blocks (id TEXT PRIMARY KEY, ord INTEGER NOT NULL, cells TEXT NOT NULL,
       states TEXT NOT NULL, status TEXT NOT NULL DEFAULT 'todo', worker TEXT, lease_until INTEGER,
       attempts INTEGER NOT NULL DEFAULT 0, error TEXT, updated INTEGER)''',
    '''CREATE TABLE IF NOT EXISTS cells (id INTEGER PRIMARY KEY, x INTEGER NOT NULL, y INTEGER NOT NULL,
       bytes INTEGER NOT NULL, sha256 TEXT NOT NULL, record TEXT NOT NULL, worker TEXT, source TEXT, built INTEGER)''',
    'CREATE TABLE IF NOT EXISTS files (path TEXT PRIMARY KEY, hash TEXT NOT NULL, sha256 TEXT NOT NULL, bytes INTEGER NOT NULL)',
    '''CREATE TABLE IF NOT EXISTS workers (id TEXT PRIMARY KEY, name TEXT, status TEXT, now TEXT, detail TEXT,
       cells INTEGER NOT NULL DEFAULT 0, seen INTEGER)''',
]
# Rebuilt from the ledger on every publish; everything else is carried as is.
GLOBAL_FILES = ('cells/catalog.ccm', 'cells/catalog.json', 'cells/states.json', 'manifest.json',
                'site/coverage.json', 'site/build.json')
CELL_ID = lambda x, y: 1 + x * 256 + y


def now():
    return int(time.time())


def sha256_file(path):
    with Path(path).open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def dumps(value):
    """The builder's JSON style (build_us.atomic_json)."""
    return json.dumps(value, indent=2, sort_keys=True, ensure_ascii=False) + '\n'


class Ledger:
    def __init__(self, cl, worker_id, worker_name):
        self.cl = cl
        self.worker = worker_id
        self.name = worker_name

    # ---- meta ----

    def meta(self, *keys):
        marks = ','.join('?' * len(keys))
        rows = self.cl.rows(f'SELECT k, v FROM meta WHERE k IN ({marks})', *keys)
        return {r['k']: json.loads(r['v']) for r in rows}

    def set_meta(self, **values):
        self.cl.batch([('INSERT INTO meta (k, v) VALUES (?, ?) ON CONFLICT(k) DO UPDATE SET v = excluded.v',
                        [k, json.dumps(v, sort_keys=True)]) for k, v in values.items()])

    # ---- workers and claims ----

    BEAT_EVERY = 600            # an unchanged worker row is rewritten this often
    LEASE_EVERY = 1800          # claimed blocks' leases are renewed this often (LEASE is 2 h)

    def heartbeat(self, status, now_states=None, detail='', cells=None):
        """Every row written counts against D1's daily free tier, so a beat with
        nothing new is skipped until the worker row or the leases are due."""
        t = now()
        said = (status, json.dumps(now_states or []), detail[:300])
        last = getattr(self, '_beat', None)
        lease_due = t - getattr(self, '_lease_at', 0) >= self.LEASE_EVERY
        if not cells and last and last[0] == said and t - last[1] < self.BEAT_EVERY and not lease_due:
            return
        self._beat = (said, t)
        statements = [('''INSERT INTO workers (id, name, status, now, detail, seen) VALUES (?, ?, ?, ?, ?, ?)
            ON CONFLICT(id) DO UPDATE SET name = excluded.name, status = excluded.status, now = excluded.now,
            detail = excluded.detail, seen = excluded.seen''',
            [self.worker, self.name, status, json.dumps(now_states or []), detail[:300], t]),
        ]
        if lease_due:
            self._lease_at = t
            statements.append(("UPDATE blocks SET lease_until = ? WHERE worker = ? AND status IN ('claimed', 'built')",
                               [t + LEASE, self.worker]))
        if cells:
            statements.append(('UPDATE workers SET cells = cells + ? WHERE id = ?', [cells, self.worker]))
        self.cl.batch(statements)

    def claim(self, count, preferred=(), only=False, exclude=()):
        """Atomically take up to count blocks: preferred ids first, then (unless only) plan
        order, never one in exclude (blocks too big for this PC)."""
        got = []
        skip = json.dumps(sorted(exclude))
        for _ in range(count):
            t = now()
            for pool in ((json.dumps(sorted(preferred)),) if preferred else ()) + (() if only else (None,)):
                where = ("(status = 'todo' OR (status IN ('claimed', 'built') AND lease_until < ?2))"
                         ' AND id NOT IN (SELECT value FROM json_each(?4))')
                if pool:
                    where += ' AND id IN (SELECT value FROM json_each(?5))'
                rows, _ = self.cl.sql(
                    f'''UPDATE blocks SET status = 'claimed', worker = ?1, lease_until = ?3, updated = ?2
                        WHERE id = (SELECT id FROM blocks WHERE {where} ORDER BY ord LIMIT 1)
                        RETURNING id, cells, states''', *([self.worker, t, t + LEASE, skip] + ([pool] if pool else [])))
                if rows:
                    row = rows[0]
                    got.append(dict(id=row['id'], cells=[tuple(c) for c in json.loads(row['cells'])],
                                    states=json.loads(row['states'])))
                    break
            else:
                break
        return got

    def release(self, block_ids, error=None):
        """Give blocks back. With an error the attempt counts; three failures park the block."""
        if not block_ids:
            return
        marks = ','.join('?' * len(block_ids))
        if error:
            self.cl.sql(f'''UPDATE blocks SET attempts = attempts + 1, error = ?, worker = NULL, lease_until = NULL, updated = ?,
                status = CASE WHEN attempts + 1 >= {MAX_ATTEMPTS} THEN 'failed' ELSE 'todo' END
                WHERE worker = ? AND status IN ('claimed', 'built') AND id IN ({marks})''',
                        str(error)[:500], now(), self.worker, *block_ids)
        else:
            self.cl.sql(f'''UPDATE blocks SET status = 'todo', worker = NULL, lease_until = NULL, updated = ?
                WHERE worker = ? AND status = 'claimed' AND id IN ({marks})''', now(), self.worker, *block_ids)

    def mark_built(self, block_ids):
        if block_ids:
            marks = ','.join('?' * len(block_ids))
            self.cl.sql(f"UPDATE blocks SET status = 'built', updated = ? WHERE worker = ? AND id IN ({marks})",
                        now(), self.worker, *block_ids)

    def built_ids(self):
        return {r['id'] for r in self.cl.rows('SELECT id FROM cells')}

    def counts(self):
        rows = self.cl.rows('SELECT status, count(*) AS n FROM blocks GROUP BY status')
        out = {r['status']: r['n'] for r in rows}
        cells = self.cl.rows('SELECT count(*) AS n, coalesce(sum(bytes), 0) AS b FROM cells WHERE id > 0')[0]
        return out, cells['n'], cells['b']

    # ---- publish lock ----

    def lock(self, wait=True, say=print):
        while True:
            t = now()
            _, meta = self.cl.sql('''INSERT INTO meta (k, v) VALUES ('publish_lock', ?1)
                ON CONFLICT(k) DO UPDATE SET v = excluded.v
                WHERE json_extract(meta.v, '$.until') < ?2 OR json_extract(meta.v, '$.worker') = ?3''',
                json.dumps(dict(worker=self.worker, name=self.name, until=t + LOCK)), t, self.worker)
            if meta.get('changes'):
                return True
            if not wait:
                return False
            holder = self.meta('publish_lock').get('publish_lock', {})
            say(f'Waiting for {holder.get("name", "another PC")} to finish publishing...')
            time.sleep(20)

    def unlock(self):
        self.cl.sql("DELETE FROM meta WHERE k = 'publish_lock' AND json_extract(v, '$.worker') = ?", self.worker)

    # ---- the site's view ----

    def coverage(self, states, cells):
        """Same schema as build_us.publish_coverage / make_coverage.py."""
        by = {}
        for c in cells.values():
            if c['id']:
                by[(c['x'], c['y'])] = c['bytes']
        out_states, lattice = [], {}
        for code, row in sorted(states.items()):
            need = [tuple(c) for c in row['cells']]
            built = [c for c in need if c in by]
            for c in need:
                lattice[c] = c in by
            out_states.append(dict(code=code, name=row.get('name', code), tier=row.get('tier', 'plus'),
                                   need=len(need), built=len(built), bytes=sum(by[c] for c in built),
                                   partial=len(row.get('partial') or []), bbox=row.get('bbox')))
        return dict(generated=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()), states=out_states,
                    cells=[[x, y, int(v), by.get((x, y), 0)] for (x, y), v in sorted(lattice.items())])

    def build_status(self, coverage):
        """build.json as the page reads it: status, done/total, the states in progress."""
        t = now()
        active = [w for w in self.cl.rows('SELECT status, now FROM workers WHERE seen > ?', t - 900)
                  if w['status'] in ('building', 'extracting', 'publishing', 'downloading')]
        done = sum(1 for c in coverage['cells'] if c[2])
        names = {s['code']: s for s in coverage['states']}
        current = {}
        for w in active:
            for code in json.loads(w['now'] or '[]'):
                s = names.get(code)
                if s:
                    current[code] = dict(code=code, name=s['name'], built=s['built'], need=s['need'])
        status = 'paused'
        if active:
            status = 'building' if any(w['status'] in ('building', 'publishing') for w in active) else 'extracting'
        return dict(status=status, done=done, total=len(coverage['cells']), now=list(current.values()),
                    workers=len(active), updated=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()))

    # ---- publish ----

    def publish(self, stage, new_cells, say=print, message=None, dry_run=False):
        """new_cells: [dict(id, x, y, bytes, sha256, record, block, source)] with record parts
        relative to stage. Returns the deployment URL. A dry run stops before uploading
        and returns the would-be manifest {path: row}."""
        stage = Path(stage)
        if not dry_run:
            self.lock(say=say)
        try:
            files = {r['path']: r for r in self.cl.rows('SELECT path, hash, sha256, bytes FROM files')}
            cells = {}
            for r in self.cl.rows('SELECT id, x, y, bytes, sha256, record FROM cells'):
                r['record'] = json.loads(r['record']); cells[r['id']] = r
            meta = self.meta('states', 'outlines', 'catalog_header', 'headers', 'part_bytes')
            states, part_bytes = meta['states'], meta['part_bytes']
            local = {}   # path -> file on this PC to upload if Cloudflare lacks it
            for c in new_cells:
                old = cells.get(c['id'])
                if old:
                    for p in old['record']['parts']:
                        files.pop(p['file'], None)
                for p in c['record']['parts']:
                    path = stage / p['file']
                    files[p['file']] = dict(path=p['file'], hash=cloud.pages_hash(path), sha256=sha256_file(path),
                                            bytes=path.stat().st_size)
                    local[p['file']] = path
                cells[c['id']] = c
            # Global files from the ledger.
            out = stage / 'global'
            records = sorted((c['record'] for c in cells.values()), key=lambda r: r['id'])
            def put(name, data):
                path = out / name
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_bytes(data if isinstance(data, bytes) else data.encode('utf8'))
                files[name] = dict(path=name, hash=cloud.pages_hash(path), sha256=sha256_file(path), bytes=path.stat().st_size)
                local[name] = path
            header = bytearray(bytes.fromhex(meta['catalog_header']))
            struct.pack_into('<I', header, 8, len(records))
            rows = b''.join(struct.pack('<II32sII', r['id'], r['bytes'], bytes.fromhex(r['sha256']),
                                        r.get('part_bytes', r['parts'][0]['bytes']), len(r['parts'])) for r in records)
            header[84:116] = hashlib.sha256(bytes(header[:84]) + rows).digest()
            put('cells/catalog.ccm', bytes(header) + rows)
            put('cells/catalog.json', dumps(dict(format='CCMAPS2', index=None, host_part_bytes=part_bytes, cells=records)))
            found = {(c['x'], c['y']): c['bytes'] for c in cells.values() if c['id']}
            index = {}
            for code, row in sorted(states.items()):
                installed = [tuple(c) for c in row['cells'] if tuple(c) in found]
                index[code] = dict(row, code=code, bytes=sum(found[c] for c in installed), built_cells=len(installed),
                                   complete=len(installed) == len(row['cells']), host_part_bytes=part_bytes)
            put('cells/states.json', dumps(dict(index, outlines=meta['outlines'])))
            coverage = self.coverage(states, cells)
            put('site/coverage.json', json.dumps(coverage, separators=(',', ':')))
            put('site/build.json', dumps(self.build_status(coverage)))
            listed = sorted((p for p in files if p != 'manifest.json'))
            put('manifest.json', dumps(dict(
                host_part_bytes=part_bytes, cell_count=len(found), required_cells=len(set(
                    tuple(c) for s in states.values() for c in s['cells'])), missing_cells=None,
                complete=False, publisher='LakeShark map worker', parts=sum(len(r['parts']) for r in records),
                files=[dict(file=p, bytes=files[p]['bytes'], sha256=files[p]['sha256']) for p in listed],
                total_bytes=sum(files[p]['bytes'] for p in listed), file_count=len(listed) + 1,
                state_index='cells/states.json')))
            if len(files) > 19000:
                raise RuntimeError(f'{len(files)} files: near the Pages 20,000-file limit; owner review needed')
            if dry_run:
                return files, coverage
            # Upload only what Cloudflare lacks. Anything missing that is not on this PC
            # means the store lost a live file; stop rather than deploy a hole.
            jwt = self.cl.upload_token()
            missing = self.cl.missing(jwt, {f['hash'] for f in files.values()})
            lost = [p for p, f in files.items() if f['hash'] in missing and p not in local]
            if lost:
                raise RuntimeError(f'Cloudflare no longer holds {len(lost)} live files (e.g. {lost[0]}); not deploying')
            todo = [(files[p]['hash'], path, cloud.content_type(p)) for p, path in local.items() if files[p]['hash'] in missing]
            if todo:
                total = sum(Path(t[1]).stat().st_size for t in todo)
                say(f'Uploading {len(todo)} files ({total / 1e6:.1f} MB)...')
                self.cl.upload(jwt, todo, progress=lambda a, b: say(f'  uploaded {a}/{b}'))
            self.cl.upsert_hashes(jwt, sorted({f['hash'] for f in files.values()}))
            dep = self.cl.deploy({'/' + p: f['hash'] for p, f in files.items()}, meta['headers'],
                                 message or f'{self.name}: {len(new_cells)} cells')
            for _ in range(60):
                stage_ = (dep.get('latest_stage') or {})
                if stage_.get('name') == 'deploy' and stage_.get('status') == 'success':
                    break
                if stage_.get('status') == 'failure':
                    raise RuntimeError('Pages deployment failed: ' + json.dumps(stage_))
                time.sleep(5)
                dep = self.cl.deployment(dep['id'])
            # Record what is live now. Files first, so cells never point at parts the ledger lacks.
            self._record(files, new_cells, coverage, dep)
            return dep.get('url'), coverage
        finally:
            if not dry_run:
                self.unlock()

    def _record(self, files, new_cells, coverage, dep):
        # Only what changed: rewriting every live file on each publish used up
        # D1's 100,000 free row writes a day within a few publishes.
        live = {r['path']: r['hash'] for r in self.cl.rows('SELECT path, hash FROM files')}
        rows = [dict(p=f['path'], h=f['hash'], s=f['sha256'], b=f['bytes']) for f in files.values()
                if live.get(f['path']) != f['hash']]
        statements = []
        for i in range(0, len(rows), 400):
            statements.append(('''INSERT INTO files (path, hash, sha256, bytes)
                SELECT json_extract(value, '$.p'), json_extract(value, '$.h'), json_extract(value, '$.s'), json_extract(value, '$.b')
                FROM json_each(?) WHERE true
                ON CONFLICT(path) DO UPDATE SET hash = excluded.hash, sha256 = excluded.sha256, bytes = excluded.bytes''',
                               [json.dumps(rows[i:i + 400])]))
        gone = sorted(set(live) - set(files))
        for i in range(0, len(gone), 400):
            statements.append(('DELETE FROM files WHERE path IN (SELECT value FROM json_each(?))', [json.dumps(gone[i:i + 400])]))
        for c in new_cells:
            statements.append(('''INSERT OR REPLACE INTO cells (id, x, y, bytes, sha256, record, worker, source, built)
                VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)''', [c['id'], c['x'], c['y'], c['bytes'], c['sha256'],
                json.dumps(c['record'], sort_keys=True), self.worker, c.get('source'), now()]))
        blocks = sorted({c['block'] for c in new_cells if c.get('block')})
        if blocks:
            statements.append(("UPDATE blocks SET status = 'done', error = NULL, updated = ? WHERE id IN (SELECT value FROM json_each(?))",
                               [now(), json.dumps(blocks)]))
        statements.append(('INSERT INTO meta (k, v) VALUES (?, ?) ON CONFLICT(k) DO UPDATE SET v = excluded.v',
                           ['coverage', json.dumps(coverage, separators=(',', ':'))]))
        statements.append(('INSERT INTO meta (k, v) VALUES (?, ?) ON CONFLICT(k) DO UPDATE SET v = excluded.v',
                           ['deploy', json.dumps(dict(id=dep.get('id'), url=dep.get('url'), by=self.name, at=now(),
                                                      cells=len(new_cells)))]))
        for i in range(0, len(statements), 40):
            self.cl.batch(statements[i:i + 40])
