"""Release-surface checks; no hardware or external access."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


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


if __name__ == '__main__':
    unittest.main()
