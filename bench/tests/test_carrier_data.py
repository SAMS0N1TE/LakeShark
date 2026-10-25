"""Defaults/schema/ABI checks for CARRIER's versioned public data boundary."""
import json
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CarrierData(unittest.TestCase):
    def test_combat_rules_v6(self):
        data = json.loads((ROOT / 'data/carrier/rules.json').read_text())
        schema = json.loads((ROOT / 'data/carrier/rules.schema.json').read_text())
        self.assertEqual(data['version'], 6)
        self.assertEqual(schema['properties']['version']['const'], 6)
        self.assertEqual(data['weapons_cool_s'], 2.0)
        self.assertEqual([data[k] for k in ['gimbal_deg', 'turret_deg', 'turret_rate_dps']], [10, 85, 120])
        self.assertEqual(data['mount_damage'], [1, .85, .6])
        self.assertEqual(data['mount_price'], [1, 1.6, 2.8])
        self.assertEqual(data['reverse'], .35)
        self.assertEqual(data['aim_assist_deg'], [0, 4, 12])
        combat = data['combat']
        self.assertEqual(combat['damage'], [[1.2, .8], [.6, 1.3], [.4, 1.6]])
        self.assertEqual(len(combat['weapons']), 6)
        self.assertTrue(all(len(row) == 10 for row in combat['weapons']))
        self.assertEqual(set(combat), set(schema['properties']['combat']['required']))
        self.assertEqual(combat['defence'][13:16], [2.5, 900, 2])
        self.assertEqual(combat['weapons'][2][6], 400)
        self.assertTrue(all(1 <= row[3] <= 4 for row in data['shipyard']['hulls']))

    def test_defaults_match(self):
        data = json.loads((ROOT / 'data/carrier/rules.json').read_text())
        include = (ROOT / 'app-sdk/apps/carrier/rules_default.inc').read_text().splitlines()[1]
        self.assertEqual(data, json.loads(json.loads(include)))
        schema = json.loads((ROOT / 'data/carrier/rules.schema.json').read_text())
        self.assertEqual(set(data), set(schema['required']))
        self.assertEqual(data['version'], 6)   # Aimed hyperspace uses rules v6.
        for key in ['ships', 'commodity_base', 'economy_slope', 'equipment_cost']:
            self.assertEqual(len(data[key]), schema['properties'][key]['minItems'])
        self.assertTrue(all(len(row) == 7 for row in data['ships']))

    def test_aircraft_exports(self):
        names = {'lap_aircraft', 'lap_aircraft_track', 'lap_aircraft_demo'}
        exports = (ROOT / 'app-sdk/exports.txt').read_text().splitlines()
        resolver = re.findall(r'EXPORT\("([^"]+)"', (ROOT / 'components/apps/tui/ls_lap_exports.inc').read_text())
        self.assertEqual(exports, sorted(set(exports)))
        self.assertEqual(exports, resolver)
        self.assertTrue(names <= set(exports))
        header = (ROOT / 'app-sdk/include/lakeshark_app.h').read_text()
        self.assertTrue(all(name in header for name in names))
        self.assertRegex(header, r'LAP_API_MINOR\s+2\b')


if __name__ == '__main__':
    unittest.main()
