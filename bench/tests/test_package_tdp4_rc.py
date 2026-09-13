import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('package_tdp4_rc', Path(__file__).parents[1]/'tools/package_tdp4_rc.py')
package = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package)


class PackageValidation(unittest.TestCase):
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


if __name__=='__main__':unittest.main()
