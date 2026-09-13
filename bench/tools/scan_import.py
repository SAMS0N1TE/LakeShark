"""Convert local CSV/JSON or an approved RadioReference query into a P4 scan list."""
import argparse
import csv
import getpass
import io
import json
import math
import os
from pathlib import Path
import sys
import time
import urllib.request
import xml.etree.ElementTree as ET

RR_NS = 'http://api.radioreference.com/soap2'
SOAP = 'http://schemas.xmlsoap.org/soap/envelope/'
MAX_INPUT = 4 * 1024 * 1024


def number(value, low, high, label):
    try:
        result = float(value)
    except (ValueError, TypeError):
        raise ValueError('Invalid ' + label) from None
    if not math.isfinite(result) or not low <= result <= high:
        raise ValueError('Out of range: ' + label)
    return result


def normalize(row, lat=None, lon=None, radius=None, zone=0):
    r = {str(k).lower().replace(' ', '_'): v for k, v in row.items()}
    mode = str(r.get('mode', '')).upper()
    modes = {'P25': 'P25', 'NFM': 'NFM', 'FMN': 'NFM', 'FM': 'NFM'}
    if mode not in modes or str(r.get('encrypted', r.get('enc', '0'))).lower() not in ('0', 'false', '', 'none'):
        raise ValueError('Unsupported or encrypted mode: ' + mode)
    hz = number(r['frequency_hz'], 1000000, 2000000000, 'frequency') if 'frequency_hz' in r else number(r.get('frequency', r.get('out')), 1, 2000, 'MHz') * 1000000
    if abs(hz - round(hz)) > .001:
        raise ValueError('Frequency must resolve to whole Hz')
    name = str(r.get('name') or r.get('alpha_tag') or r.get('alpha') or r.get('description') or r.get('descr') or f'{hz / 1e6:.4f}')
    name = ''.join(c if 32 <= ord(c) <= 126 and c not in '|"\\' else '_' for c in name).strip()[:15]
    if not name:
        raise ValueError('Empty channel name')
    latitude = number(r.get('latitude', lat), -90, 90, 'latitude')
    longitude = number(r.get('longitude', lon), -180, 180, 'longitude')
    distance = number(r.get('radius_km', radius), .001, 500, 'coverage radius km')
    z = number(r.get('zone', zone), 0, 7, 'zone')
    if z != int(z):
        raise ValueError('Zone must be an integer')
    priority = str(r.get('priority', '0')).lower()
    if priority not in ('0', '1', 'true', 'false'):
        raise ValueError('Priority must be boolean')
    return dict(name=name, frequency_hz=round(hz), mode=modes[mode], zone=int(z),
                latitude=latitude, longitude=longitude, radius_km=distance,
                priority=int(priority in ('1', 'true')))


def encode(rows):
    if not 1 <= len(rows) <= 64:
        raise ValueError('Select 1–64 channels; nothing is silently truncated')
    seen = set()
    lines = ['LSCAN1']
    for r in rows:
        key = r['frequency_hz'], r['mode'], r['zone']
        if key in seen:
            raise ValueError('Duplicate frequency/mode/zone; refine the selection')
        seen.add(key)
        lines.append('{name}|{frequency_hz}|{mode}|{zone}|{latitude:.7f}|{longitude:.7f}|{radius_km:.3f}|{priority}'.format(**r))
    return '\n'.join(lines) + '\n'


def soap_result(data):
    if len(data) > MAX_INPUT or b'<!DOCTYPE' in data.upper() or b'<!ENTITY' in data.upper():
        raise ValueError('Unsupported SOAP response')
    root = ET.fromstring(data)
    ids = {e.get('id'): e for e in root.iter() if e.get('id')}
    budget = 100000
    def unpack(e, visited=()):
        nonlocal budget
        budget -= 1
        if budget < 0 or len(visited) > 32:
            raise ValueError('SOAP result exceeds structural limits')
        ref = e.get('href')
        if ref:
            if not ref.startswith('#') or ref in visited or len(visited) > 32 or ref[1:] not in ids:
                raise ValueError('Invalid SOAP reference')
            return unpack(ids[ref[1:]], visited + (ref,))
        if not len(e):
            return e.text or ''
        children = list(e)
        names = [c.tag.rsplit('}', 1)[-1] for c in children]
        if all(n == 'item' for n in names):
            return [unpack(c, visited) for c in children]
        return {n: unpack(c, visited) for n, c in zip(names, children)}
    for e in root.iter():
        if e.tag.rsplit('}', 1)[-1] == 'Fault':
            raise ValueError('RadioReference rejected the request; check subscription and approved app key')
    result = next((e for e in root.iter() if e.tag.rsplit('}', 1)[-1] == 'return'), None)
    if result is None:
        raise ValueError('Missing SOAP result')
    return unpack(result)


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        raise ValueError('RadioReference endpoint redirected; credentials were not forwarded')


class RadioReference:
    def __init__(self, username, password, app_key):
        self.auth = dict(username=username, password=password, appKey=app_key, version='18', style='rpc')
        if not all((username, password, app_key)):
            raise ValueError('Your RR login and approved application key are required')

    def call(self, method, **parameters):
        envelope = ET.Element('{' + SOAP + '}Envelope')
        envelope.set('xmlns:xsi', 'http://www.w3.org/2001/XMLSchema-instance')
        envelope.set('xmlns:xsd', 'http://www.w3.org/2001/XMLSchema')
        envelope.set('xmlns:rr', RR_NS)
        body = ET.SubElement(envelope, '{' + SOAP + '}Body')
        operation = ET.SubElement(body, '{' + RR_NS + '}' + method)
        operation.set('{' + SOAP + '}encodingStyle', 'http://schemas.xmlsoap.org/soap/encoding/')
        for key, value in parameters.items():
            ET.SubElement(operation, key, {'xsi:type': 'xsd:int'}).text = str(int(value))
        auth = ET.SubElement(operation, 'authInfo')
        auth.set('xsi:type', 'rr:authInfo')
        for key, value in self.auth.items():
            ET.SubElement(auth, key, {'xsi:type': 'xsd:string'}).text = value
        request = urllib.request.Request('https://api.radioreference.com/soap2/index.php',
            data=ET.tostring(envelope, encoding='utf-8', xml_declaration=True),
            headers={'Content-Type': 'text/xml; charset=utf-8', 'SOAPAction': '"' + RR_NS + '#' + method + '"'})
        with urllib.request.build_opener(NoRedirect()).open(request, timeout=30) as response:
            data = response.read(MAX_INPUT + 1)
        return soap_result(data)

    def county(self, county_id, selected, radius_km):
        county = self.call('getCountyInfo', ctid=county_id)
        modes = {str(m['mode']): m['modeName'] for m in self.call('getMode', mode=0)}
        rows, skipped = [], []
        for category in county.get('cats', []):
            for subcat in category.get('subcats', []):
                if selected and int(subcat['scid']) not in selected:
                    continue
                for freq in self.call('getSubcatFreqs', scid=int(subcat['scid'])):
                    record = dict(freq, mode=modes.get(str(freq.get('mode')), 'UNKNOWN'),
                                  latitude=subcat.get('lat') or county.get('lat'),
                                  longitude=subcat.get('lon') or county.get('lon'), radius_km=radius_km)
                    try:
                        rows.append(normalize(record))
                    except ValueError as error:
                        skipped.append({'name': str(freq.get('alpha', '')), 'reason': str(error)})
        return rows, skipped


def upload(port, text):
    import serial
    connection = serial.Serial(port=None, baudrate=115200, timeout=.1)
    connection.dtr = connection.rts = False
    connection.port = port
    connection.open()
    def command(line, expected):
        data = (line + '\n').encode('ascii')
        if len(data) > 125:
            raise ValueError('Console command too long')
        connection.reset_input_buffer()
        connection.write(data)
        received = bytearray()
        until = time.monotonic() + 10
        while time.monotonic() < until:
            received.extend(connection.read(4096))
            if expected.encode() in received:
                return
            if b'REJECTED' in received or b'CHCOMMIT FAILED' in received:
                raise ValueError('Device rejected the import; existing list retained')
            if len(received) > 32768:
                received = received[-16384:]
        raise ValueError('No device acknowledgement; inspect ch dump before retrying a commit')
    try:
        command('ch stage', 'CHSTAGE READY')
        for row in text.splitlines()[1:]:
            command('ch row "' + row + '"', 'CHROW OK')
        command('ch commit', 'CHCOMMIT OK')
    except Exception:
        connection.write(b'ch abort\n')
        raise
    finally:
        connection.close()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    source = p.add_mutually_exclusive_group(required=True)
    source.add_argument('--input', type=Path, help='CSV or JSON {channels: [...]}')
    source.add_argument('--rr-county', type=int, help='RadioReference county database ID')
    p.add_argument('--rr-subcat', type=int, action='append', default=[])
    p.add_argument('--latitude', type=float)
    p.add_argument('--longitude', type=float)
    p.add_argument('--radius-km', type=float, help='Explicit coverage radius; required for RR')
    p.add_argument('--zone', type=int, default=0)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--port', help='Optionally install through USB console; replaces the saved list once')
    p.add_argument('--allow-unfiltered', action='store_true', help='Acknowledge that tone/NAC receive filters are not applied')
    args = p.parse_args()
    if args.rr_county is not None:
        if args.radius_km is None:
            p.error('--radius-km is required; imported coverage is a selection boundary, not measured reception')
        if not args.allow_unfiltered:
            p.error('RR tone/NAC filters are not yet applied; use --allow-unfiltered to acknowledge')
        key = os.environ.get('RR_APP_KEY') or getpass.getpass('Approved RR application key: ')
        username = os.environ.get('RR_USERNAME') or input('RR username: ')
        password = os.environ.get('RR_PASSWORD') or getpass.getpass('RR password: ')
        rows, skipped = RadioReference(username, password, key).county(args.rr_county, set(args.rr_subcat), args.radius_km)
    else:
        if args.input.stat().st_size > MAX_INPUT:
            raise ValueError('Input exceeds 4 MB')
        text = args.input.read_text(encoding='utf-8-sig')
        raw = json.loads(text) if args.input.suffix.lower() == '.json' else list(csv.DictReader(io.StringIO(text)))
        if isinstance(raw, dict): raw = raw['channels']
        rows, skipped = [], []
        for record in raw:
            if not args.allow_unfiltered and any(str(record.get(k, '')).strip() not in ('', 'CSQ', '0', 'None') for k in ('Tone', 'tone', 'nac', 'NAC')):
                raise ValueError('Tone/NAC filtering is not implemented; --allow-unfiltered explicitly accepts carrier/any-NAC reception')
            rows.append(normalize(record, args.latitude, args.longitude, args.radius_km, args.zone))
    output = encode(rows)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + '.tmp')
    temporary.write_text(output, encoding='ascii', newline='\n')
    temporary.replace(args.output)
    report = {'channels': rows, 'skipped': skipped, 'receive_filters': 'carrier squelch / any NAC',
              'scope': 'conventional Phase I P25 and analog FM; no Phase II or trunked traffic-channel import'}
    args.output.with_suffix('.report.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(f'Wrote {len(rows)} channels to {args.output}; skipped {len(skipped)} unsupported/encrypted records')
    if args.port:
        upload(args.port, output)
        print('Device import acknowledged; scanning is stopped')


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, KeyError, ET.ParseError) as error:
        print('Import failed: ' + str(error), file=sys.stderr)
        raise SystemExit(1)
