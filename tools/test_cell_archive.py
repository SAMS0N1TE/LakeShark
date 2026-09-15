import tempfile
import unittest
import zlib
from pathlib import Path
from cell_console import Console

class TransferTests(unittest.TestCase):
    def test_device_readback_requires_complete_size_and_matching_crc(self):
        console=Console.__new__(Console)
        response='CFILE BEGIN 1280000\nCFILE END 1280000 f1ca9c00\n'
        console.command=lambda *a,**kw:response
        self.assertTrue(console.check_file('capture.cu8',1280000,'f1ca9c00'))
        for broken in ('CFILE BEGIN 1280000\n',
                       response.replace('END 1280000','END 640000'),
                       response.replace('f1ca9c00','00000000'),
                       'CFILE ERROR read\n'):
            console.command=lambda *a,**kw:broken
            with self.assertRaises(ValueError):console.check_file('capture.cu8',1280000,'f1ca9c00')

    def test_verified_bytes_and_corruption(self):
        data=b'{"lte":{"found":false}}\n'
        response=f'CFILE BEGIN {len(data)}\nCF 0 {data.hex()}\nCFILE END {len(data)} {zlib.crc32(data):08x}\n'
        console=Console.__new__(Console)
        with tempfile.TemporaryDirectory() as directory:
            path=Path(directory)/'capture.json'
            console.command=lambda *a,**kw:response
            self.assertEqual(console.download('capture.json',path),data)
            original=path.read_bytes()
            for broken in (response.replace('CF 0','CF 1'),response.replace(data.hex(),'00'+data.hex()[2:]),response.split('CFILE END')[0]):
                console.command=lambda *a,**kw:broken
                with self.assertRaises(ValueError):console.download('capture.json',path)
                self.assertEqual(path.read_bytes(),original)

if __name__=='__main__':unittest.main()
