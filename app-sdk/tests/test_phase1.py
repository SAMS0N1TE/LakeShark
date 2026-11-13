"""Targeted host tests: python -m unittest discover -s app-sdk/tests -p test_phase1.py."""
import json
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
SDK = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SDK))
import apidoc
import api_registry
import lap


class Phase1(unittest.TestCase):
    def test_library_boundaries(self):
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp)/('libraries.exe' if os.name=='nt' else 'libraries')
            cc = shutil.which('gcc') or 'C:/mingw64/bin/gcc.exe'
            subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-I'+str(SDK/'lib'), '-I'+str(SDK/'include'),
                            str(SDK/'tests/test_sdk_libraries.c'), *[str(SDK/'lib'/n) for n in ('lap_csv.c','lap_json.c','lap_loop.c')], '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_registry_covers_allowlist(self):
        calls = [e for e in apidoc.entries() if e.get('exported')]
        exports = {n for n in (SDK/'exports.txt').read_text().split() if n.startswith(('lap_', 'l3_'))}
        self.assertEqual(exports, {e['name'] for e in calls})
        self.assertFalse([e['name'] for e in calls if e['simulator']['status']=='missing'])
        for e in calls:
            self.assertRegex(e['since'], r'^\d+\.\d+$')
            self.assertIn(e['simulator']['status'], ('real', 'faked', 'limited', 'missing'))
            self.assertTrue(e['simulator']['reason'])
        by = {e['name']: e for e in calls}
        self.assertEqual(by['lap_tcp_connect']['since'], '1.3')
        self.assertEqual(by['lap_seen_next']['since'], '2.2')
        self.assertEqual(by['lap_ui_keys']['since'], '2.1')
        for name in ('lap_radio_modes','lap_packet_open','lap_edges_open','lap_scan_start','lap_channels_query','lap_record_start','lap_replay_read','lap_tx_packet','lap_nfc_write'):
            self.assertEqual(by[name]['since'], '2.4')
        # Radio captures and microphone WAV recording are separate API 2.4 services.
        for name in ('lap_record_start', 'lap_record_stop'):
            self.assertEqual(by[name]['capability'], 'LAP_CAP_RADIO_RX')
        for name in ('lap_audio_record_start', 'lap_audio_record_stop'):
            self.assertEqual(by[name]['since'], '2.4')
            self.assertEqual(by[name]['capability'], 'LAP_CAP_MIC')
        for name in ('lap_caps','lap_api_version','lap_input_next','lap_text_input','lap_save_read','lap_settings_set'):
            self.assertEqual(by[name]['since'], '2.3')
        self.assertIn('retained',by['lap_sd_read']['pointer_contract'])

    def test_generated_files_current(self):
        data=json.loads((SDK/'docs/api.json').read_text())
        self.assertEqual(data['api_version'], '.'.join(map(str,api_registry.version(SDK))))
        self.assertEqual(data['calls'], sorted([e for e in apidoc.entries() if e.get('exported')], key=lambda e:e['name']))

    def test_header_api_version(self):
        major, minor=api_registry.version(SDK)
        self.assertTrue(lap.supports_api(major,minor))
        self.assertFalse(lap.supports_api(major,minor+1))
        self.assertTrue(lap.supports_api(2,3))
        self.assertTrue(lap.supports_api(2,4))
        self.assertTrue(lap.supports_api(2,5))
        self.assertTrue(lap.supports_api(2,6))
        self.assertTrue(lap.supports_api(2,7))
        self.assertFalse(lap.supports_api(2,8))
        self.assertTrue(lap.supports_api(1,3))
        self.assertFalse(lap.supports_api(1,4))

    def test_frozen_assets_include_libraries_and_registry(self):
        spec=importlib.util.spec_from_file_location('studio_packager',SDK.parent/'host/lsterm/build_app.py')
        module=importlib.util.module_from_spec(spec); spec.loader.exec_module(module)
        with tempfile.TemporaryDirectory() as tmp:
            module.OUT=tmp
            staged=Path(module.stage_sdk())
            for name in ('api_registry.py','lib/lap_loop.c','lib/lap_json.h','lib/lap_csv.c','tools/sim_alloc.h','docs/api.json'):
                self.assertTrue((staged/name).is_file(),name)

    def test_nested_source_layout(self):
        with tempfile.TemporaryDirectory() as tmp:
            p=Path(tmp); (p/'src/nested').mkdir(parents=True); (p/'build').mkdir()
            for f in ('main.c', 'src/a.c', 'src/nested/b.c', 'build/ignored.c'): (p/f).touch()
            self.assertEqual([str(f.relative_to(p)).replace('\\','/') for f in lap.project_sources(p)], ['main.c','src/a.c','src/nested/b.c'])


if __name__ == '__main__': unittest.main()
