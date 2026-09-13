import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('scan_import', Path(__file__).parents[1]/'tools/scan_import.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)

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

if __name__=='__main__': unittest.main()
