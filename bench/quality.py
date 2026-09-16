"""Small, offline behavior index and fail-closed hardware release check."""
import argparse
import sys
import unittest
import copy
import tempfile
from unittest.mock import patch
import hashlib
import json
import re
import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
REQUIRED_CASES = ('p25_loaded_5min', 'flipper_open_close_10', 'fm_p25_switch_10',
                  'wifi_retry_under_load', 'alerts_all_off', 'alerts_independent',
                  'flipper_relaunch_no_crash', 'touch_audio_usable', 'no_blue_flash',
                  'ui_progress_under_load', 'no_unexpected_restart', 'memory_headroom',
                  'universal_source_coverage', 'gps_recording')


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def require_release(data, artifact, evidence):
    """Reject absent, stale, incomplete or artifact-mismatched hardware runs."""
    validate(data)
    unresolved = [i['id'] for i in data['incidents']
                  if i['release_blocker'] and i['state'] != 'verified']
    if unresolved:
        raise ValueError('unresolved incidents: ' + ', '.join(unresolved))
    if not artifact or not evidence:
        raise ValueError('release requires --artifact and --evidence')
    record = json.loads(Path(evidence).read_text())
    if record.get('source_digest') != source_digest():
        raise ValueError('hardware run source is stale')
    if record.get('firmware_sha256') != sha256(artifact):
        raise ValueError('hardware run firmware differs from release artifact')
    if record.get('board') != 'T-Display-P4' or not record.get('observer'):
        raise ValueError('hardware board and observer required')
    if record.get('sdr_active') is not True or record.get('duration_seconds', 0) < 300:
        raise ValueError('at least five minutes with active SDR required')
    audits = record.get('feature_audits', [])
    if not audits:
        raise ValueError('per-feature all-radio audit required')
    for audit in audits:
        validate_feature_audit(data, audit, record['source_digest'], release=True)
    for case in REQUIRED_CASES:
        result = record.get('cases', {}).get(case, {})
        if result.get('result') != 'pass' or not result.get('observation'):
            raise ValueError('missing observed hardware case: ' + case)
    for source in data['universal_sources']:
        cases = record.get('source_cases', {}).get(source['source'], {})
        for operation in ('recording', 'waterfall'):
            case = cases.get(operation, {})
            if case.get('result') != 'pass' or not case.get('observation') or not case.get('data_format'):
                raise ValueError('missing source coverage: ' + source['source'] + '/' + operation)
    for name in ('log', 'flipper_app', 'sdkconfig'):
        item = record.get(name, {})
        path = Path(evidence).resolve().parent / item.get('path', '')
        if not path.is_file() or sha256(path) != item.get('sha256'):
            raise ValueError('missing or mismatched evidence file: ' + name)
    if sha256(Path(artifact).parent / 'sdkconfig') != record['sdkconfig']['sha256']:
        raise ValueError('evidence configuration differs from artifact build')
    if not record.get('flipper_firmware') or not record.get('c6_firmware'):
        raise ValueError('paired Flipper and C6 firmware identities required')
    log = Path(evidence).resolve().parent / record['log']['path']
    text = log.read_text(errors='replace')
    analysis = analyze(text)
    if len(analysis['ui_frame_samples']) < 2:
        raise ValueError('repeated UI progress evidence required')
    if not analysis['late_samples'] or analysis['finding'] == 'review-required':
        raise ValueError('hardware log is absent or contains unresolved failures')
    if 'mode_P25_rtl_1' not in text or 'ble=ready' not in text or 'rtl_0' in text:
        raise ValueError('capture does not establish connected Flipper and available P25 SDR')
    if not re.search(r'app=P25 park=0 rx=active[^\r\n]*present=1 stream=1', text):
        raise ValueError('detailed receiver status must confirm active P25 streaming')
    require_memory_evidence(memory_report(), record, text)
    for incident in data['incidents']:
        if incident['release_blocker']:
            c = incident['clearance']
            for key in ('source_digest', 'firmware_sha256'):
                if c[key] != record[key]:
                    raise ValueError('incident clearance differs from hardware run')
            if c['log_sha256'] != record['log']['sha256']:
                raise ValueError('incident log differs from hardware run')


def require_memory_evidence(report, record, text):
    review = record.get('memory_review', {})
    if review.get('diff_sha256') != report['diff_sha256'] or not review.get('placement_rationale'):
        raise ValueError('memory review is absent or stale')
    samples = re.findall(r'memory: internal_free=(\d+) internal_largest=(\d+) internal_min=(\d+) dma_free=(\d+) dma_largest=(\d+) dma_min=(\d+) psram_free=(\d+)', text)
    if len(samples) < 2:
        raise ValueError('memory snapshots before and after loaded run required')
    for key, index in [('required_internal_largest', 1), ('required_dma_largest', 4)]:
        budget = review.get(key)
        if type(budget) is not int or budget <= 0:
            raise ValueError('explicit justified allocation budget required: ' + key)
        if min(int(row[index]) for row in samples) < budget:
            raise ValueError('measured headroom below allocation budget: ' + key)


def analyze(text):
    gaps = [int(x) for x in re.findall(r'(?:max_gap=|max=)(\d+)', text)]
    late = [int(x) for x in re.findall(r'(?:late=|refresh gaps: total=)(\d+)', text)]
    errors = sorted(set(re.findall(
        r'^.*(?:underrun happens|ESP_ERR_INVALID_ARG|Guru Meditation|panic|'
        r'Stack protection fault|CORRUPT HEAP|HardFault|assert failed|'
        r'FuriCrash|Stack overflow).*$', text, re.M)))
    ui_frames = [int(a) + int(b) for a, b in re.findall(
        r'tui: frames on core 0 (\d+), core 1 (\d+) since the last look', text)]
    uptime = [int(x) for x in re.findall(r'up[_=](\d+)s', text)]
    if any(b < a for a, b in zip(uptime, uptime[1:])):
        errors.append('Board uptime decreased during capture')
    if any(n == 0 for n in ui_frames):
        errors.append('UI frame counter made no progress between probes')
    audio_calls = []
    for line in text.splitlines():
        if 'P25QUAL:' not in line:
            continue
        counters = dict(re.findall(r'\b(under|drop|vox)=(\d+)', line))
        if 'under' in counters and 'drop' in counters:
            audio_calls.append({k: int(v) for k, v in counters.items()})
            if int(counters['under']) or int(counters['drop']):
                errors.append('P25 audio requires review: ' + line.strip())
    stuck_since, last_queue = None, None
    for stamp, voice, queued in re.findall(r'\((\d+)\) P25TEL:[^\r\n]*\bvox=(\d+)[^\r\n]*\bringB=(\d+)', text):
        sample = (int(voice), int(queued))
        if sample != last_queue or not sample[1]:
            stuck_since = int(stamp)
            last_queue = sample
        elif int(stamp) - stuck_since >= 2000:
            errors.append('P25 nonempty audio queue unchanged for at least 2 seconds')
            break
    # These include end-of-call drain and missing voice frames, so do not
    # diagnose scheduling starvation solely from an underrun count.
    # Cumulative values can include earlier failures: this is a review signal,
    # not automatic attribution to a particular action or proof of visual state.
    return {'late_samples': late, 'max_gap_us': max(gaps, default=None),
            'errors': errors, 'p25_audio_calls': audio_calls, 'ui_frame_samples': ui_frames, 'hardware_clearance': False,
            'finding': 'review-required' if errors or any(late) else 'inconclusive'}


def validate(data):
    if data.get('schema') != 1:
        raise ValueError('unsupported regression schema')
    validate_radio_inventory(data)
    contracts = data['contracts']
    ids = {c['id'] for c in contracts}
    if len(ids) != len(contracts):
        raise ValueError('duplicate contract')
    incident_ids = set()
    for c in contracts:
        if not c['expected'] or not c['checks']:
            raise ValueError('empty behavior contract')
        for p in c['paths'] + [c['history']]:
            path = (ROOT / p).resolve()
            if not path.is_relative_to(ROOT) or not path.is_file():
                raise ValueError('missing or external contract path: ' + p)
    for incident in data['incidents']:
        if incident['id'] in incident_ids or incident['contract'] not in ids:
            raise ValueError('invalid incident identity')
        incident_ids.add(incident['id'])
        if incident['state'] not in ('open', 'candidate', 'verified'):
            raise ValueError('invalid incident state')
        if incident['state'] == 'verified':
            c = incident.get('clearance') or {}
            required = ('source_digest', 'firmware_sha256', 'log_sha256',
                        'board', 'observer', 'procedure', 'result')
            if not all(c.get(k) for k in required) or c['result'] != 'pass':
                raise ValueError('verified incident lacks hardware evidence')
            if any(not re.fullmatch(r'[0-9a-f]{64}', c[k]) for k in
                   ('source_digest', 'firmware_sha256', 'log_sha256')):
                raise ValueError('clearance hashes must be SHA256')
    return data


def source_digest():
    # Hash tracked build inputs, including dirty contents. Notes and build
    # products cannot silently substitute for the firmware source being tested.
    files = subprocess.check_output(['git', 'ls-files', '--cached', '--others', '--exclude-standard', '-z'], cwd=ROOT).decode().split('\0')
    h = hashlib.sha256()
    for name in sorted(filter(None, files)):
        p = ROOT / name
        if (name.startswith(('components/', 'main/', 'boards/', 'managed_components/', 'cmake/'))
                or name.startswith('sdkconfig')
                or name in ('CMakeLists.txt', 'dependencies.lock', 'partitions.csv', 'bench/configs.json')
                or name.startswith('bench/patches/')):
            h.update(name.encode() + b'\0')
            h.update(p.read_bytes())
    return h.hexdigest()


MEMORY_MARKERS = re.compile(r'heap_caps_|(?:malloc|calloc|realloc)\s*\(|EXT_RAM|MALLOC_CAP_|DMA_ATTR|DRAM_ATTR|IRAM_ATTR|xTaskCreate|StackType_t|uxTaskGetSystemState|vTaskGetInfo|CONFIG_SPIRAM|cache_line|CACHE_LINE|intr_priority')

def memory_report():
    """Compare memory-sensitive source changes with the recorded release baseline."""
    data = json.loads((ROOT / 'bench/regressions.json').read_text())
    baseline = data['memory_baseline']
    subprocess.check_output(['git', 'rev-parse', '--verify', baseline + '^{commit}'], cwd=ROOT)
    diff = subprocess.check_output(['git', 'diff', '--no-ext-diff', '--unified=3', baseline,
        '--', 'components', 'main', 'managed_components', 'boards', 'sdkconfig*', 'bench/patches'], cwd=ROOT).decode(errors='replace')
    # Include newly introduced files before they have been staged.
    for name in subprocess.check_output(['git', 'ls-files', '--others', '--exclude-standard'], cwd=ROOT).decode().splitlines():
        if name.startswith(('components/', 'main/', 'managed_components/', 'boards/', 'bench/patches/')):
            diff += '\ndiff --git a/' + name + ' b/' + name + '\n' + '\n'.join('+' + line for line in (ROOT/name).read_text(errors='replace').splitlines())
    changed = {}
    name = ''
    for line in diff.splitlines():
        if line.startswith('diff --git '):
            name = line.split(' b/', 1)[-1]
        elif line[:1] in ('+', '-') and not line.startswith(('+++', '---')) and MEMORY_MARKERS.search(line):
            changed.setdefault(name, []).append(line[:200])
    return dict(baseline=baseline, diff_sha256=hashlib.sha256(diff.encode()).hexdigest(),
                review_required=bool(changed), files=changed,
                note='Heuristic review aid, not proof of memory safety. Record placement rationale and measured internal/DMA headroom; validate LCD, UI and audio under combined load.')


def audit_template(data, feature, digest):
    return dict(feature=feature, source_digest=digest,
        inventory_review='', memory_display_audio_impact='',
        radios=[dict(source=item['source'], decision='unreviewed', reason='',
                     evidence='', checks=[], gap_id='') for item in data['universal_sources']])


def validate_feature_audit(data, audit, digest, release=False):
    if not audit.get('feature') or audit.get('source_digest') != digest:
        raise ValueError('feature audit missing or source is stale')
    for key in ('inventory_review', 'memory_display_audio_impact'):
        if not isinstance(audit.get(key), str) or not audit[key].strip():
            raise ValueError('feature audit requires ' + key)
    rows = audit.get('radios', [])
    required = {x['source'] for x in data['universal_sources']}
    if len(rows) != len(required) or {x.get('source') for x in rows} != required:
        raise ValueError('feature audit must cover every radio exactly once')
    incidents = {x['id']: x for x in data['incidents']}
    for row in rows:
        decision = row.get('decision')
        if decision not in ('covered', 'gap', 'not_applicable'):
            raise ValueError('unreviewed radio: ' + row['source'])
        if any(not isinstance(row.get(k), str) or not row[k].strip() for k in ('reason', 'evidence')):
            raise ValueError('specific reason and evidence required: ' + row['source'])
        if decision == 'covered' and not row.get('checks'):
            raise ValueError('applicable feature requires checks: ' + row['source'])
        if decision == 'gap':
            incident = incidents.get(row.get('gap_id'))
            if not incident or incident['state'] == 'verified':
                raise ValueError('gap must link to unresolved incident: ' + row['source'])
            if release:
                raise ValueError('feature still has an implementation gap: ' + row['source'])
    return audit


def validate_radio_inventory(data):
    """Detect new registered radio identifiers omitted from the audit inventory."""
    field = (ROOT/'components/apps/tui/ls_field.h').read_text()
    enum = re.search(r'typedef enum \{(.*?)\} ls_field_source_t', field, re.S)
    if not enum:
        raise ValueError('cannot identify field radio inventory')
    discovered = set(re.findall(r'LS_FIELD_[A-Z0-9_]+', enum.group(1))) - {'LS_FIELD_NONE', 'LS_FIELD_SOURCES'}
    header = (ROOT/'components/lakeshark/radio/radio_endpoint.h').read_text()
    discovered.update(re.findall(r'^#define (LS_RADIO_ENDPOINT_\w+) "', header, re.M))
    bindings = data.get('radio_inventory_bindings', {})
    missing = discovered - set(bindings)
    if missing:
        raise ValueError('new radio identifiers require feature audit inventory: ' + ', '.join(sorted(missing)))
    names = {x['source'] for x in data['universal_sources']}
    if any(bindings[k] not in names for k in discovered):
        raise ValueError('radio identifier maps to missing source')


def main():
    command = sys.argv[1] if len(sys.argv) > 1 else ''
    if command == 'test':
        load_test_tools()
        suite = unittest.defaultTestLoader.loadTestsFromModule(quality)
        return 0 if unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful() else 1
    if command == 'watch':
        return capture_main()
    if command == 'host':
        return subprocess.call(['pwsh', '-NoProfile', '-File', str(ROOT/'bench/verify.ps1'),
                                '-Level', 'host', *sys.argv[2:]], cwd=ROOT)
    if command == 'package':
        return subprocess.call([sys.executable, str(ROOT/'bench/tools/package_tdp4_rc.py'),
                                *sys.argv[2:]], cwd=ROOT)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=['context', 'check', 'release-check', 'analyze', 'source-digest', 'test', 'watch', 'host', 'package', 'memory', 'sources', 'audit'])
    parser.add_argument('--topic', default='')
    parser.add_argument('--log', type=Path)
    parser.add_argument('--artifact', type=Path)
    parser.add_argument('--evidence', type=Path)
    a = parser.parse_args()
    if a.command == 'analyze':
        if not a.log:
            parser.error('--log required')
        result = analyze(a.log.read_text(encoding='utf-8', errors='replace'))
        result['log_sha256'] = hashlib.sha256(a.log.read_bytes()).hexdigest()
        print(json.dumps(result, indent=2))
        return 1 if result['finding'] == 'review-required' else 0
    if a.command == 'source-digest':
        print(source_digest())
        return 0
    if a.command == 'memory':
        print(json.dumps(memory_report(), indent=2))
        return 0
    data = validate(json.loads((ROOT / 'bench/regressions.json').read_text()))
    if a.command == 'audit':
        if a.evidence:
            validate_feature_audit(data, json.loads(a.evidence.read_text()), source_digest())
            print('Feature audit complete; gaps remain open and hardware validation is separate.')
        else:
            if not a.topic:
                parser.error('--topic <feature> required for audit template')
            print(json.dumps(audit_template(data, a.topic, source_digest()), indent=2))
        return 0
    if a.command == 'sources':
        print(json.dumps(data['universal_sources'], indent=2))
        return 0
    blockers = [i for i in data['incidents'] if i['release_blocker'] and i['state'] != 'verified']
    if a.command == 'check':
        print(f'Regression index valid; {len(blockers)} unresolved release blocker(s). Hardware clearance is separate.')
        return 0
    if a.command == 'release-check':
        try:
            require_release(data, a.artifact, a.evidence)
        except (ValueError, OSError, KeyError, TypeError) as error:
            print('RELEASE BLOCKED:', error)
            return 1
        print('Hardware record and release artifact match. Publication still requires authorization.')
        return 0
    for i in blockers:
        print('OPEN RELEASE BLOCKER:', i['id'], i['summary'])
    for c in data['contracts']:
        if not a.topic or any(t in a.topic.lower() for t in c['topics']):
            print('\n' + c['id'] + ': ' + c['expected'])
            print('Read:', ', '.join(c['paths']))
            print('Verify:', '; '.join(c['checks']))
            print('History:', c['history'])
    return 0




from datetime import datetime, timezone
import time

quality = sys.modules[__name__]
sys.modules.setdefault("quality", quality)

class RegressionTests(unittest.TestCase):
    def test_memory_evidence_rejects_stale_missing_and_insufficient_headroom(self):
        report = dict(diff_sha256='a'*64)
        record = dict(memory_review=dict(diff_sha256='a'*64,
            placement_rationale='Measured next allocations require 4096 internal and 8192 DMA',
            required_internal_largest=4096, required_dma_largest=8192))
        line = 'memory: internal_free=20000 internal_largest=12000 internal_min=15000 dma_free=16000 dma_largest=9000 dma_min=10000 psram_free=20000000\n'
        quality.require_memory_evidence(report, record, line*2)
        for log in ('', line, (line*2).replace('dma_largest=9000', 'dma_largest=7000')):
            with self.assertRaises(ValueError):
                quality.require_memory_evidence(report, record, log)
        record['memory_review']['diff_sha256'] = 'b'*64
        with self.assertRaises(ValueError):
            quality.require_memory_evidence(report, record, line*2)

    def test_feature_audit_requires_every_radio_and_tracks_gaps(self):
        data = json.loads((quality.ROOT/'bench/regressions.json').read_text())
        audit = quality.audit_template(data, 'recording', 'a'*64)
        with self.assertRaises(ValueError):
            quality.validate_feature_audit(data,audit,'a'*64)
        audit.update(inventory_review='registered radios checked', memory_display_audio_impact='shared renderer: unchanged placement, check loaded UI')
        for row in audit['radios']:
            row.update(decision='covered',reason='shared path',evidence='fixture',checks=['source lifecycle'])
        quality.validate_feature_audit(data,audit,'a'*64)
        for mutate in ('omit', 'duplicate', 'unreviewed', 'reason', 'stale'):
            bad=copy.deepcopy(audit)
            if mutate=='omit': bad['radios'].pop()
            elif mutate=='duplicate': bad['radios'][-1]=bad['radios'][0]
            elif mutate=='unreviewed': bad['radios'][0]['decision']='unreviewed'
            elif mutate=='reason': bad['radios'][0]['reason']=''
            else: bad['source_digest']='b'*64
            with self.subTest(mutate=mutate), self.assertRaises(ValueError):
                quality.validate_feature_audit(data,bad,'a'*64)
        audit['radios'][0].update(decision='gap',gap_id='UNIVERSAL-RADIO-TOOLS-20260915')
        quality.validate_feature_audit(data,audit,'a'*64)
        with self.assertRaises(ValueError):
            quality.validate_feature_audit(data,audit,'a'*64,release=True)

    def test_inventory_does_not_silently_drop_a_registered_radio(self):
        data=json.loads((quality.ROOT/'bench/regressions.json').read_text())
        del data['radio_inventory_bindings']['LS_FIELD_NRF24']
        with self.assertRaises(ValueError):
            quality.validate_radio_inventory(data)

    def test_ui_stall_and_reboot_with_zero_display_gaps(self):
        for evidence in ('tui: frames on core 0 0, core 1 0 since the last look',
                         '+OK up_120s_rst_poweron\n+OK up_5s_rst_poweron'):
            result = quality.analyze('panel: late=0 max_gap=25100 us\n' + evidence)
            self.assertEqual(result['finding'], 'review-required')
            self.assertFalse(result['hardware_clearance'])

    def test_flipper_crash_is_failure(self):
        for message in ('HardFault', 'FuriCrash', 'Stack overflow', 'assert failed'):
            self.assertEqual(quality.analyze(message)['finding'], 'review-required')

    def test_release_evidence_matrix(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            artifact = root / 'firmware.bin'
            artifact.write_bytes(b'candidate')
            record = dict(source_digest='a'*64, firmware_sha256=quality.sha256(artifact),
                          board='T-Display-P4', observer='hardware tester', sdr_active=True,
                          duration_seconds=300, flipper_firmware='87.1', c6_firmware='2.12.9',
                          cases={c: {'result': 'pass', 'observation': 'Observed in fixture run'}
                                 for c in quality.REQUIRED_CASES})
            for name, content in [('log', b'tui: frames on core 0 200, core 1 0 since the last look\ntui: frames on core 0 200, core 1 0 since the last look\npanel: late=0 max_gap=25085\nmode_P25_rtl_1\nble=ready\napp=P25 park=0 rx=active present=1 stream=1'),
                                  ('flipper_app', b'flipper'), ('sdkconfig', b'config')]:
                path = root / name
                path.write_bytes(content)
                record[name] = dict(path=name, sha256=quality.sha256(path))
            data = json.loads((quality.ROOT / 'bench/regressions.json').read_text())
            audit = audit_template(data, 'fixture', record['source_digest'])
            audit.update(inventory_review='fixture', memory_display_audio_impact='fixture')
            for row in audit['radios']:
                row.update(decision='covered', reason='fixture', evidence='fixture', checks=['fixture'])
            record['feature_audits'] = [audit]
            record['source_cases'] = {item['source']: {op: dict(result='pass', observation='fixture', data_format='fixture') for op in ('recording', 'waterfall')} for item in data['universal_sources']}
            for incident in data['incidents']:
                incident.update(state='verified', clearance=dict(
                    source_digest=record['source_digest'], firmware_sha256=record['firmware_sha256'],
                    log_sha256=record['log']['sha256'], board='T-Display-P4', observer='tester',
                    procedure='matrix', result='pass'))
            evidence = root / 'run.json'

            def check(value):
                evidence.write_text(json.dumps(value))
                with patch('quality.source_digest', return_value='a'*64), patch('quality.require_memory_evidence'):
                    quality.require_release(data, artifact, evidence)

            check(record)
            bad = copy.deepcopy(record)
            del bad['source_cases']['GPS/GNSS']['recording']
            with self.assertRaises(ValueError):
                check(bad)
            for key, value in [('sdr_active', False), ('duration_seconds', 299),
                               ('source_digest', 'b'*64), ('firmware_sha256', 'b'*64),
                               ('observer', ''), ('flipper_firmware', '')]:
                with self.subTest(key=key), self.assertRaises(ValueError):
                    check(dict(record, **{key: value}))
            for case in quality.REQUIRED_CASES:
                bad = copy.deepcopy(record)
                del bad['cases'][case]
                with self.subTest(case=case), self.assertRaises(ValueError):
                    check(bad)
            for name in ('log', 'flipper_app', 'sdkconfig'):
                bad = copy.deepcopy(record)
                bad[name]['sha256'] = 'b'*64
                with self.subTest(file=name), self.assertRaises(ValueError):
                    check(bad)
            for log in ('', 'panel: late=2', 'HardFault',
                        'panel: late=0 mode_P25_rtl_0 ble=ready',
                        'panel: late=0 mode_P25_rtl_1 ble=off',
                        'panel: late=0 mode_P25_rtl_1 ble=ready app=ADS-B park=1 stream=0'):
                (root / 'log').write_text(log)
                bad = copy.deepcopy(record)
                bad['log']['sha256'] = quality.sha256(root / 'log')
                with self.subTest(log=log), self.assertRaises(ValueError):
                    check(bad)

    def test_empty_log_cannot_clear_hardware(self):
        result = quality.analyze('')
        self.assertEqual(result['finding'], 'inconclusive')
        self.assertFalse(result['hardware_clearance'])

    def test_zero_late_is_not_visual_clearance(self):
        result = quality.analyze('panel: late=0 max_gap=30102 us')
        self.assertFalse(result['hardware_clearance'])

    def test_p25_audio_requires_review_even_with_normal_display(self):
        result = quality.analyze('panel: late=0\nW P25QUAL: vox=15 under=76 drop=0 dec=104ms')
        self.assertEqual(result['finding'], 'review-required')
        self.assertEqual(result['p25_audio_calls'], [{'vox': 15, 'under': 76, 'drop': 0}])
        self.assertFalse(result['hardware_clearance'])
        self.assertEqual(quality.analyze('P25QUAL: vox=9 under=0 drop=2')['finding'], 'review-required')
        self.assertEqual(quality.analyze('P25QUAL: vox=9 under=0 drop=0')['finding'], 'inconclusive')

    def test_stuck_short_audio_is_reviewed_without_underruns(self):
        lines='\n'.join(f'W ({t}) P25TEL: vox=34 under=190 ringB=5760' for t in (1000, 2000, 3000))
        self.assertEqual(quality.analyze(lines)['finding'], 'review-required')
        self.assertEqual(quality.analyze(lines.replace('ringB=5760','ringB=0'))['finding'], 'inconclusive')
        self.assertEqual(quality.analyze(lines.replace('(3000) P25TEL: vox=34','(3000) P25TEL: vox=35'))['finding'], 'inconclusive')

    def test_real_display_gap_is_flagged(self):
        result = quality.analyze('W rm69a10: refresh gaps: total=5 max=45822 us expected=25085 us')
        self.assertEqual(result['finding'], 'review-required')
        self.assertEqual(result['max_gap_us'], 45822)

    def test_transport_failure_without_display_counters(self):
        self.assertEqual(quality.analyze('E CMD53 ESP_ERR_INVALID_ARG')['finding'], 'review-required')

    def test_verified_requires_evidence(self):
        data = json.loads((quality.ROOT / 'bench/regressions.json').read_text())
        data['incidents'][0]['state'] = 'verified'
        with self.assertRaises(ValueError):
            quality.validate(data)

    def test_release_blocked_on_open_incident(self):
        with patch('sys.argv', ['quality.py', 'release-check']), patch('quality.source_digest', return_value='test'):
            self.assertEqual(quality.main(), 1)


def capture_main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--port', required=True)
    p.add_argument('--p25-telemetry', action='store_true', help='Temporarily enable P25TEL; diagnostic run, not timing acceptance')
    p.add_argument('--memory', action='store_true', help='Probe the new memory command without task enumeration')
    p.add_argument('--seconds', type=int, default=330)
    p.add_argument('--log', type=Path, required=True)
    a = p.parse_args(sys.argv[2:])
    if not 1 <= a.seconds <= 3600:
        p.error('--seconds must be between 1 and 3600')
    import serial
    link = serial.Serial()
    link.port, link.baudrate, link.timeout = a.port, 115200, 0.2
    link.dtr = link.rts = False
    # Exclusive creation prevents replacing a previous evidence file.
    with a.log.open('x', encoding='utf-8') as out:
        try:
            link.open()
            if a.p25_telemetry:
                link.write(b'log P25TEL warn\r\n')
            deadline = time.monotonic() + a.seconds
            next_probe, index = 0, 0
            while time.monotonic() < deadline:
                if time.monotonic() >= next_probe:
                    probes = ('display', 'ble', 'fl SYS', 'fl STAT', 'tui cost') + (('memory',) if a.memory else ())
                    cmd = probes[index % len(probes)]
                    out.write('\nPROBE ' + datetime.now(timezone.utc).isoformat() + ' ' + cmd + '\n')
                    link.write((cmd + '\r\n').encode())
                    next_probe = time.monotonic() + 10
                    index += 1
                text = link.read(max(1, link.in_waiting)).decode(errors='replace')
                if text:
                    out.write(text)
                    out.flush()
        finally:
            if a.p25_telemetry and link.is_open:
                link.write(b'log P25TEL error\r\n')
                link.flush()
            link.close()
    result = analyze(a.log.read_text(encoding='utf-8'))
    print(json.dumps(result, indent=2))
    print('Capture complete. Record observed flashes and test actions separately; no automatic hardware clearance.')
    return 1 if result['finding'] == 'review-required' else 0




# Existing P4 packaging, release surface and scan-import tests live here too.
def load_test_tools():
    import importlib.util
    global package, module
    def load(name):
        spec = importlib.util.spec_from_file_location(name, ROOT/'bench/tools'/f'{name}.py')
        result = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(result)
        return result
    package = load('package_tdp4_rc')
    module = load('scan_import')

class PackageValidation(unittest.TestCase):
    def test_open_incident_stops_package_before_output(self):
        with tempfile.TemporaryDirectory() as temp:
            build = Path(temp) / 'build'
            build.mkdir()
            (build/'sdkconfig').write_text(self.config)
            (build/'project_description.json').write_text('{}')
            (build/'lakeshark.bin').write_bytes(self.image)
            out = Path(temp) / 'out'
            with patch('sys.argv', ['package', '--build', str(build), '--output', str(out),
                                   '--evidence', str(Path(temp)/'missing.json')]), \
                 patch.object(package, 'git', side_effect=['', self.revision]), \
                 patch.object(package, 'validate', return_value=self.identity):
                with self.assertRaisesRegex(ValueError, 'unresolved incidents'):
                    package.main()
            self.assertFalse(out.exists())

    def setUp(self):
        self.revision='a'*40
        self.version='2.1.0-rc1'
        self.identity=self.version+'-g'+'a'*12
        self.config='\n'.join(('CONFIG_LS_BOARD_T_DISPLAY_P4=y',
                              'CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y',
                              'CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="boards/partitions_16m.csv"'))
        self.image=bytearray(256)
        self.image[0]=0xe9
        self.image[32:36]=bytes.fromhex('3254cdab')
        self.image[48:48+len(self.identity)]=self.identity.encode()
        self.metadata={'project_version':self.identity}

    def check(self):
        return package.validate(self.config,self.metadata,self.image,self.revision,self.version)

    def test_matching_app(self):
        self.assertEqual(self.check(),self.identity)

    def test_other_board_or_partition_is_rejected(self):
        for original in ('T_DISPLAY_P4','partitions_16m.csv'):
            config=self.config
            self.config=config.replace(original,'other')
            with self.assertRaises(ValueError):self.check()
            self.config=config

    def test_stale_or_dirty_metadata_is_rejected(self):
        self.metadata['project_version']+='-dirty'
        with self.assertRaises(ValueError):self.check()

    def test_mismatched_binary_is_rejected(self):
        self.image[48]=ord('0')
        with self.assertRaises(ValueError):self.check()

    def test_non_app_image_is_rejected(self):
        self.image[32]=0
        with self.assertRaises(ValueError):self.check()



class ReleaseSurfaceTests(unittest.TestCase):
    def test_excluded_protocol_has_no_production_entrypoint(self):
        forbidden = re.compile(r'FM_MODE_TRAP|FM_FREQ_TRAP|fm_trap_frame_t|'
                               r'trap_(?:create|process|destroy|reset|dispatch)|'
                               r'buildTrapTab|"TRAP"|target[ _-]?tag', re.I)
        for folder in ('main', 'components/apps', 'components/lakeshark'):
            for path in (ROOT / folder).rglob('*'):
                if path.suffix in ('.c', '.cpp', '.h', '.hpp'):
                    with self.subTest(path=str(path.relative_to(ROOT))):
                        self.assertIsNone(forbidden.search(path.read_bytes().decode('latin-1')))
        for name in ('components/lakeshark/apps/fm/trap.c',
                     'components/lakeshark/apps/fm/trap.h',
                     'bench/fixtures/trap_gen.c', 'bench/fixtures/trap_gen.h'):
            self.assertFalse((ROOT / name).exists(), name)

    def test_public_docs_link_to_existing_local_documents(self):
        docs = [ROOT / 'README.md', *(ROOT / 'docs').glob('LCD43_*.md')]
        docs = [p for p in docs if 'PREVIEW_' not in p.name]
        for path in docs:
            for link in re.findall(r'\]\(([^)]+)\)', path.read_text(encoding='utf-8')):
                if '://' in link or link.startswith('#'):
                    continue
                target = link.split('#', 1)[0]
                with self.subTest(document=path.name, link=link):
                    self.assertTrue((path.parent / target).is_file())



class ImportTests(unittest.TestCase):
    def row(self, **extra):
        return dict(name='Local', frequency_hz=154785000, mode='P25', latitude=43.4,
                    longitude=-71.6, radius_km=30, **extra)

    def test_csv_aliases_and_frequency_units(self):
        r=module.normalize({'Frequency':'154.785','Alpha Tag':'Dispatch','Mode':'FMN'},43.4,-71.6,30)
        self.assertEqual(r['frequency_hz'],154785000)
        self.assertEqual(r['mode'],'NFM')
        self.assertIn('Dispatch|154785000|NFM|0|43.4000000|-71.6000000|30.000|0',module.encode([r]))

    def test_coordinates_and_unsupported_modes_are_not_guessed(self):
        for field,value in [('latitude',None),('longitude',float('nan')),('mode','P25 Phase II'),('mode','DMR'),('enc',1)]:
            row=self.row(); row[field]=value
            with self.assertRaises(ValueError): module.normalize(row)

    def test_duplicate_and_overflow_preserve_explicit_selection(self):
        row=module.normalize(self.row())
        with self.assertRaises(ValueError): module.encode([row,row])
        with self.assertRaises(ValueError): module.encode([row]*65)
        with self.assertRaises(ValueError): module.encode([])

    def test_soap_multiref_and_faults(self):
        data=b'<Envelope><Body><reply><return href="#r"/></reply><multiRef id="r"><item href="#i"/></multiRef><multiRef id="i"><mode>1</mode><modeName>FM</modeName></multiRef></Body></Envelope>'
        self.assertEqual(module.soap_result(data),[{'mode':'1','modeName':'FM'}])
        for invalid in [b'<Envelope><Fault><faultstring>secret echo</faultstring></Fault></Envelope>',
                        b'<Envelope><return href="#r"/><ref id="r" href="#r"/></Envelope>',
                        b'<!DOCTYPE data><Envelope/>']:
            with self.assertRaises(ValueError): module.soap_result(invalid)

    def test_console_names_cannot_inject_commands(self):
        row=self.row(); row['name']='A"\\\n| ch clear'
        name=module.normalize(row)['name']
        for c in '"\\\n|': self.assertNotIn(c,name)




class AudioPrebufferTests(unittest.TestCase):
    def test_short_voice_burst_cannot_wait_for_next_call(self):
        import shutil
        source=(ROOT/'components/lakeshark/audio/audio_out.c').read_text()
        body=source.split('/* AUDIO_PREBUFFER_BEGIN: production policy exercised by bench/quality.py. */',1)[1].split('/* AUDIO_PREBUFFER_END */',1)[0]
        header=(ROOT/'components/lakeshark/audio/audio_out.h').read_text()
        defines='\n'.join(re.findall(r'^#define (?:PREBUF_MS|PREBUF_BYTES|AUDIO_RATE_HZ)\s+.*$', source+'\n'+header, re.M))
        code=r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
"""+defines+"\n"+body+r"""
int main(void) {
 int64_t since=-1;
 /* Captured 180ms burst stays below 280ms threshold but must play on time. */
 assert(!audio_prebuffer_ready(5760,false,1000000,&since));
 assert(!audio_prebuffer_ready(5760,false,1279999,&since));
 assert(audio_prebuffer_ready(5760,false,1280000,&since));
 assert(since==-1);
 /* Full buffer and explicit short playback preserve immediate start. */
 assert(audio_prebuffer_ready(8960,false,2000000,&since));
 assert(audio_prebuffer_ready(2,true,2100000,&since));
 assert(!audio_prebuffer_ready(0,true,2200000,&since));
 /* Empty/muted/reset queue cannot transfer a prior deadline to a new call. */
 assert(!audio_prebuffer_ready(640,false,3000000,&since));
 assert(!audio_prebuffer_ready(0,false,3100000,&since));
 assert(!audio_prebuffer_ready(640,false,4000000,&since));
 assert(!audio_prebuffer_ready(640,false,4279999,&since));
 assert(audio_prebuffer_ready(640,false,4280000,&since));
 return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            c=Path(tmp)/'prebuffer.c';exe=Path(tmp)/'prebuffer.exe';c.write_text(code)
            subprocess.run([shutil.which('gcc') or 'gcc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

class UsbBootRecoveryTests(unittest.TestCase):
    def test_production_retry_is_bounded_and_preserves_registered_devices(self):
        import shutil
        source=(ROOT/'components/lakeshark/radio/usb_host.c').read_text()
        body=source.split('/* USB_BOOT_RETRY_BEGIN: also compiled by the consolidated host tests. */',1)[1].split('/* USB_BOOT_RETRY_END */',1)[0]
        code=r"""
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_LOGW(...) ((void)0)
static int calls, fail;
static bool actions[16];
static esp_err_t usb_host_lib_set_root_port_power(bool on) {
 actions[calls++]=on;return fail;
}
"""+body+r"""
int main(void) {
 usb_boot_retry_t r={.next_us=10000000};
 usb_boot_retry_tick(&r,9999999,0);assert(!calls);
 usb_boot_retry_tick(&r,10000000,-1);assert(!calls);
 for(int i=0;i<3;i++) {
  usb_boot_retry_tick(&r,r.next_us,0);assert(r.off);
  assert(calls==2*i+1 && !actions[calls-1]);
  usb_boot_retry_tick(&r,r.next_us-1,0);assert(calls==2*i+1);
  usb_boot_retry_tick(&r,r.next_us,0);assert(!r.off && actions[calls-1]);
 }
 usb_boot_retry_tick(&r,r.next_us,0);assert(r.done && calls==6);
 calls=0;r=(usb_boot_retry_t){0};
 usb_boot_retry_tick(&r,0,1);assert(r.done && !calls);
 usb_boot_retry_tick(&r,100000000,0);assert(!calls);
 calls=0;r=(usb_boot_retry_t){0};fail=1;
 for(int i=0;i<4;i++)usb_boot_retry_tick(&r,r.next_us,0);
 assert(r.done && calls==3 && !r.off);
 calls=0;r=(usb_boot_retry_t){0};fail=0;
 usb_boot_retry_tick(&r,0,0);fail=1;
 usb_boot_retry_tick(&r,r.next_us,0);assert(r.done && calls==2);
 return 0;
}
"""
        with tempfile.TemporaryDirectory() as tmp:
            c=Path(tmp)/'retry.c';exe=Path(tmp)/'retry.exe';c.write_text(code)
            subprocess.run([shutil.which('gcc') or 'gcc','-std=c11','-Wall','-Wextra','-Werror',str(c),'-o',str(exe)],check=True)
            subprocess.run([str(exe)],check=True)

if __name__ == '__main__':
    raise SystemExit(main())
