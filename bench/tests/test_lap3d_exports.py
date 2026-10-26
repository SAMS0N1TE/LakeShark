"""Check every non-inline lap3d declaration reaches the sorted ELF ABI."""
from pathlib import Path
import re
import unittest
ROOT = Path(__file__).resolve().parents[2]
class Lap3dExports(unittest.TestCase):
    def test_all_public_functions(self):
        header = (ROOT/'app-sdk/include/lap3d.h').read_text()
        clean = re.sub(r'/\*.*?\*/', '', header, flags=re.S)
        functions = set(re.findall(r'\b(l3_\w+)\s*\([^;{}]*\)\s*;', clean))
        functions -= {'l3_add', 'l3_sub', 'l3_scale', 'l3_dot', 'l3_cross'}
        exports = (ROOT/'app-sdk/exports.txt').read_text().splitlines()
        table = re.findall(r'EXPORT\("([^"]+)",\s*(\w+)\)', (ROOT/'components/apps/tui/ls_lap_exports.inc').read_text())
        self.assertEqual(exports, sorted(set(exports)))
        self.assertEqual(exports, [name for name, _ in table])
        self.assertEqual(functions, {name for name in exports if name.startswith('l3_')})
        self.assertTrue(all(dict(table)[name] == name for name in functions))
    def test_api_version(self):
        minor = re.search(r'LAP_API_MINOR\s+(\d+)\b', (ROOT/'app-sdk/include/lakeshark_app.h').read_text())
        self.assertIsNotNone(minor)
        self.assertGreaterEqual(int(minor[1]), 2)
        self.assertIn('API 1.2', (ROOT/'app-sdk/README.md').read_text())
if __name__ == '__main__':
    unittest.main()
