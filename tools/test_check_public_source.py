from pathlib import Path
import tempfile
import unittest
from check_public_source import check


class PublicSourceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        for name in ('README.md', 'LICENSE', 'CMakeLists.txt', 'main/idf_component.yml', 'boards/t_display_p4.defaults'):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('')
        (self.root / 'docs').mkdir()

    def test_minimal_source_passes(self):
        self.assertEqual(check(self.root), [])

    def test_working_material_fails(self):
        for name in ('CLAUDE.md', 'AGENTS.md', 'LAKESHARK_MARKERS.txt', 'migration/old.md', 'bench/run.ps1'):
            path = self.root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('')
            self.assertIn(name, check(self.root))

    def test_broken_document_link_fails(self):
        (self.root / 'README.md').write_text('[Guide](docs/missing.md)')
        self.assertIn('README.md: missing docs/missing.md', check(self.root))

    def test_helix_source_fails(self):
        path = self.root / 'managed_components/chmorgan__esp-libhelix-mp3/LICENSE'
        path.parent.mkdir(parents=True)
        path.write_text('')
        self.assertIn(path.relative_to(self.root).as_posix(), check(self.root))


if __name__ == '__main__':
    unittest.main()
