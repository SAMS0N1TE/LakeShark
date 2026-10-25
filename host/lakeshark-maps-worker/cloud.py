"""Cloudflare for the map worker: the owner's token, the shared D1 ledger and
incremental Pages deploys of the map host.

The token never leaves this module: it is read from the file the owner keeps
(or from a DPAPI-protected copy in the user profile), sent only to
api.cloudflare.com, and never printed or written next to the EXE.
"""
import base64
import ctypes
import datetime
import json
import mimetypes
import os
from pathlib import Path
import time
import urllib.error
import urllib.request
import uuid

API = 'https://api.cloudflare.com/client/v4'
PROFILE = Path(os.environ.get('APPDATA') or Path.home()) / 'LakeSharkMaps'
SETTINGS = PROFILE / 'settings.json'
DATABASE = 'lakeshark-maps'
# D1's free tier allows 100,000 rows written a day for the whole account,
# the website's own writes included, and blocks every write past it until
# 00:00 UTC. Each PC stops at this many, counted from D1's own rows_written
# and kept across restarts, then waits for the next UTC day.
WRITE_BUDGET = int(os.environ.get('LSMAPS_D1_WRITES', '20000'))
WRITES = PROFILE / 'd1-writes.json'
PROJECT = 'cartocore-maps'
BRANCH = 'preview'


class CloudError(RuntimeError):
    pass


def _utc_day():
    return datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%d')


def _seconds_to_utc_midnight():
    now = datetime.datetime.now(datetime.timezone.utc)
    nxt = (now + datetime.timedelta(days=1)).replace(hour=0, minute=0, second=0, microsecond=0)
    return (nxt - now).total_seconds()


# ---- settings and the token ----

def load_settings():
    try:
        return json.loads(SETTINGS.read_text(encoding='utf8'))
    except (OSError, ValueError):
        return {}


def save_settings(settings):
    PROFILE.mkdir(parents=True, exist_ok=True)
    tmp = SETTINGS.with_name(SETTINGS.name + '.tmp')
    tmp.write_text(json.dumps(settings, indent=2, sort_keys=True) + '\n', encoding='utf8')
    os.replace(tmp, SETTINGS)


class _Blob(ctypes.Structure):
    _fields_ = [('cbData', ctypes.c_uint32), ('pbData', ctypes.POINTER(ctypes.c_char))]


def _dpapi(data, protect):
    """Windows DPAPI: only this Windows user on this PC can read it back."""
    crypt = ctypes.windll.crypt32
    kernel = ctypes.windll.kernel32
    buf = ctypes.create_string_buffer(data, len(data))
    blob_in = _Blob(len(data), ctypes.cast(buf, ctypes.POINTER(ctypes.c_char)))
    blob_out = _Blob()
    fn = crypt.CryptProtectData if protect else crypt.CryptUnprotectData
    if not fn(ctypes.byref(blob_in), None, None, None, None, 0, ctypes.byref(blob_out)):
        raise CloudError('Windows could not protect/unprotect the stored token')
    try:
        return ctypes.string_at(blob_out.pbData, blob_out.cbData)
    finally:
        kernel.LocalFree(blob_out.pbData)


def _clean(text):
    return text.strip().strip('"').strip("'").strip()


def read_token(settings):
    if settings.get('token_file'):
        path = Path(settings['token_file'])
        if path.is_file():
            token = path.read_text(encoding='utf8').strip()
            if token:
                return token
    if settings.get('token_dpapi'):
        return _dpapi(base64.b64decode(settings['token_dpapi']), False).decode()
    return None


def ask_token(settings, reason=None):
    """First run (or a moved file): ask for the token file. The answer is kept
    in the user profile; a pasted token is stored DPAPI-encrypted instead."""
    if reason:
        print(reason)
    print('LakeShark Maps needs the Cloudflare API token (the cfat_... file, e.g. CF_TB_API.txt).')
    print('Type or drag in the path to that file. If this PC has no copy, paste the token itself;')
    print('it is then stored encrypted for this Windows user only.')
    while True:
        answer = _clean(input('Token file: '))
        if not answer:
            continue
        if answer.startswith('cfat_') or (len(answer) > 30 and not any(c in answer for c in '\\/:.')):
            settings.pop('token_file', None)
            settings['token_dpapi'] = base64.b64encode(_dpapi(answer.encode(), True)).decode()
            return answer
        path = Path(os.path.expandvars(os.path.expanduser(answer)))
        if path.is_file() and path.read_text(encoding='utf8').strip():
            settings.pop('token_dpapi', None)
            settings['token_file'] = str(path.resolve())
            return path.read_text(encoding='utf8').strip()
        print('No token in that file; try again.')


class Cloud:
    def __init__(self, token, account=None):
        self._token = token
        self.account = account or self._account()
        self.database = None

    def __repr__(self):
        return f'Cloud(account={self.account[:6]}...)'

    # ---- plain API ----

    def _label(self, path):
        """The endpoint for error messages, without ids or query."""
        path = path.split('?')[0]
        for value in (getattr(self, 'account', None), getattr(self, 'database', None)):
            if value:
                path = path.replace(value, '…')
        return path

    def _call(self, method, path, body=None, headers=None, raw=None, auth=None, timeout=120):
        url = path if path.startswith('https://') else API + path
        data = raw
        hdrs = {'Authorization': 'Bearer ' + (auth or self._token), 'User-Agent': 'LakeSharkMaps-worker'}
        if body is not None:
            data = json.dumps(body).encode()
            hdrs['Content-Type'] = 'application/json'
        hdrs.update(headers or {})
        for attempt in range(6):
            req = urllib.request.Request(url, data=data, method=method, headers=hdrs)
            try:
                with urllib.request.urlopen(req, timeout=timeout) as r:
                    payload = json.load(r)
                break
            except urllib.error.HTTPError as e:
                try:
                    payload = json.load(e)
                except ValueError:
                    payload = {}
                if e.code in (429, 500, 502, 503, 504) and attempt < 5:
                    time.sleep(2 * (attempt + 1)); continue
                errors = '; '.join(str(x.get('message')) for x in payload.get('errors', [])) or e.reason
                raise CloudError(f'{method} {self._label(path)}: HTTP {e.code}: {errors}') from None
            except (urllib.error.URLError, TimeoutError, ConnectionError) as e:
                if attempt < 5:
                    time.sleep(2 * (attempt + 1)); continue
                raise CloudError(f'{method} {self._label(path)}: {e}') from None
        if isinstance(payload, dict) and payload.get('success') is False:
            errors = '; '.join(str(x.get('message')) for x in payload.get('errors', []))
            raise CloudError(f'{method} {self._label(path)}: {errors}')
        return payload.get('result') if isinstance(payload, dict) and 'result' in payload else payload

    def _account(self):
        accounts = self._call('GET', '/accounts') or []
        if len(accounts) != 1:
            raise CloudError(f'The token sees {len(accounts)} Cloudflare accounts; expected one')
        return accounts[0]['id']

    def verify(self):
        result = self._call('GET', f'/accounts/{self.account}/tokens/verify')
        if result.get('status') != 'active':
            raise CloudError('The Cloudflare token is not active')

    # ---- D1: the shared ledger every PC claims work from ----

    def open_database(self, create=False):
        rows = self._call('GET', f'/accounts/{self.account}/d1/database?name={DATABASE}') or []
        rows = [r for r in rows if r.get('name') == DATABASE]
        if rows:
            self.database = rows[0]['uuid']
        elif create:
            self.database = self._call('POST', f'/accounts/{self.account}/d1/database', {'name': DATABASE})['uuid']
        else:
            raise CloudError(f'D1 database {DATABASE} does not exist yet; the owner runs "bootstrap" once')
        return self.database

    # ---- the daily write budget ----

    def _writes(self):
        try:
            w = json.loads(WRITES.read_text(encoding='utf8'))
            if w.get('day') == _utc_day():
                return int(w.get('rows', 0))
        except (OSError, ValueError):
            pass
        return 0

    def _count(self, results):
        rows = sum(int((r.get('meta') or {}).get('rows_written') or 0) for r in results if isinstance(r, dict))
        if rows:
            PROFILE.mkdir(parents=True, exist_ok=True)
            WRITES.write_text(json.dumps({'day': _utc_day(), 'rows': self._writes() + rows}), encoding='utf8')

    def _budget(self):
        """Before a write: past today's budget, wait for the next UTC day."""
        while self._writes() >= WRITE_BUDGET:
            wait = _seconds_to_utc_midnight() + 120
            print(f'D1: this PC has written {self._writes():,} rows today (budget {WRITE_BUDGET:,}); '
                  f'pausing writes until 00:02 UTC, {wait / 3600:.1f} h from now. Building carries on.', flush=True)
            time.sleep(min(wait, 3600))

    def writes_today(self):
        return self._writes(), WRITE_BUDGET

    def sql(self, statement, *params):
        """One statement; returns (rows, meta)."""
        if not statement.lstrip().upper().startswith('SELECT'):
            self._budget()
        result = self._call('POST', f'/accounts/{self.account}/d1/database/{self.database}/query',
                            {'sql': statement, 'params': list(params)})
        self._count(result)
        first = result[0]
        if not first.get('success', True):
            raise CloudError('D1: ' + str(first.get('error')))
        return first.get('results') or [], first.get('meta') or {}

    def rows(self, statement, *params):
        return self.sql(statement, *params)[0]

    def batch(self, statements):
        """Several (sql, params) statements in one request, applied as one transaction."""
        if not statements:
            return []
        self._budget()
        result = self._call('POST', f'/accounts/{self.account}/d1/database/{self.database}/query',
                            {'batch': [{'sql': s, 'params': list(p)} for s, p in statements]})
        self._count(result)
        return result

    # ---- Pages: content-addressed uploads, then a deploy from a full manifest ----

    def upload_token(self):
        return self._call('GET', f'/accounts/{self.account}/pages/projects/{PROJECT}/upload-token')['jwt']

    def missing(self, jwt, hashes):
        out = []
        hashes = list(hashes)
        for i in range(0, len(hashes), 1000):
            out += self._call('POST', '/pages/assets/check-missing', {'hashes': hashes[i:i + 1000]}, auth=jwt)
        return set(out)

    def upload(self, jwt, files, progress=None):
        """files: [(hash, path, content_type)]. Buckets stay under 40 MiB of base64."""
        bucket, size, sent = [], 0, 0
        def flush():
            nonlocal bucket, size, sent
            if not bucket:
                return
            self._call('POST', '/pages/assets/upload', bucket, auth=jwt, timeout=600)
            sent += len(bucket)
            if progress:
                progress(sent, len(files))
            bucket, size = [], 0
        for key, path, ctype in files:
            value = base64.b64encode(Path(path).read_bytes()).decode()
            if bucket and (size + len(value) > 40 * 1024 * 1024 or len(bucket) >= 2000):
                flush()
            bucket.append({'key': key, 'value': value, 'metadata': {'contentType': ctype}, 'base64': True})
            size += len(value)
        flush()

    def upsert_hashes(self, jwt, hashes):
        hashes = list(hashes)
        for i in range(0, len(hashes), 5000):
            self._call('POST', '/pages/assets/upsert-hashes', {'hashes': hashes[i:i + 5000]}, auth=jwt)

    def deploy(self, manifest, headers_text, message):
        """Create a Pages deployment on BRANCH from a complete {'/path': hash} manifest."""
        boundary = 'lsmaps' + uuid.uuid4().hex
        parts = []
        def field(name, value, filename=None):
            disp = f'form-data; name="{name}"' + (f'; filename="{filename}"' if filename else '')
            parts.append(f'--{boundary}\r\nContent-Disposition: {disp}\r\n\r\n'.encode() + value + b'\r\n')
        field('manifest', json.dumps(manifest, sort_keys=True).encode())
        field('branch', BRANCH.encode())
        field('commit_message', message.encode())
        field('commit_dirty', b'true')
        if headers_text:
            field('_headers', headers_text.encode(), '_headers')
        body = b''.join(parts) + f'--{boundary}--\r\n'.encode()
        return self._call('POST', f'/accounts/{self.account}/pages/projects/{PROJECT}/deployments',
                          raw=body, headers={'Content-Type': f'multipart/form-data; boundary={boundary}'}, timeout=600)

    def deployment(self, deployment_id):
        return self._call('GET', f'/accounts/{self.account}/pages/projects/{PROJECT}/deployments/{deployment_id}')


def pages_hash(path):
    """Cloudflare Pages asset key: blake3(base64(content) + extension), first 32 hex."""
    import blake3
    path = Path(path)
    data = base64.b64encode(path.read_bytes())
    return blake3.blake3(data + path.suffix[1:].encode()).hexdigest()[:32]


def content_type(path):
    name = str(path)
    if name.endswith('.json'):
        return 'application/json'
    guess = mimetypes.guess_type(name)[0]
    if guess and guess.startswith('text/'):
        return guess
    return guess or 'application/octet-stream'
