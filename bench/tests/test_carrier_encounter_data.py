"""Encounter defaults and their embedded copy obey the shipped v1 schema."""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]

class EncounterData(unittest.TestCase):
    def test_defaults(self):
        data = json.loads((ROOT / 'data/carrier/encounters.json').read_text())
        schema = json.loads((ROOT / 'data/carrier/encounters.schema.json').read_text())
        embedded = (ROOT / 'data/carrier/encounters_default.inc').read_text().splitlines()[1]
        self.assertEqual(data, json.loads(json.loads(embedded)))
        self.assertEqual(set(data), set(schema['required']))
        self.assertEqual(data['version'], 1)
        for key in ('cadence', 'chatter_seconds', 'weights', 'messages', 'chatter'):
            spec = schema['properties'][key]
            self.assertEqual(len(data[key]), spec['minItems'])
            self.assertEqual(len(data[key]), spec['maxItems'])
            for value in data[key]:
                item = spec['items']
                if isinstance(value, str):
                    self.assertTrue(re.fullmatch(item['pattern'], value))
                    self.assertGreaterEqual(len(value), item['minLength'])
                    self.assertLessEqual(len(value), item['maxLength'])
                else:
                    self.assertGreaterEqual(value, item['minimum'])
                    self.assertLessEqual(value, item['maximum'])
        self.assertGreaterEqual(len(set(data['chatter'])), 60)
        for lo, hi in zip(data['cadence'][::2], data['cadence'][1::2]):
            self.assertLessEqual(lo, hi)
        self.assertLessEqual(*data['chatter_seconds'])
        self.assertGreater(sum(data['weights']), 0)
        self.assertFalse(re.search(r'elite|thargoid|coriolis|witchspace', '\n'.join(data['chatter']+data['messages']), re.I))

if __name__ == '__main__':
    unittest.main()
