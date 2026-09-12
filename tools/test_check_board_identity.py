from pathlib import Path
import tempfile
import unittest
from check_board_identity import references, check


class BoardIdentityTests(unittest.TestCase):
    def test_comments_and_literals(self):
        for source in ('/* CONFIG_LS_BOARD_* test), build date/time */',
                       '// CONFIG_LS_BOARD_NANO',
                       '"CONFIG_LS_BOARD_NANO // text"',
                       'R"tag(CONFIG_LS_BOARD_NANO " /* text */)tag"'):
            with self.subTest(source=source):
                self.assertEqual([], references(source))

    def test_real_references(self):
        for source in ('#ifdef CONFIG_LS_BOARD_NANO',
                       '#if defined(CONFIG_LS_BOARD_NANO)',
                       '#elif CONFIG_LS_BOARD_NANO',
                       'int x = CONFIG_LS_BOARD_NANO;',
                       '#define ALIAS CONFIG_LS_BOARD_NANO',
                       '/* comment */ #if CONFIG_LS_BOARD_NANO',
                       '#if CONFIG_LS_\\\nBOARD_NANO'):
            with self.subTest(source=source):
                self.assertEqual(1, len(references(source)))

    def test_continued_comment_and_physical_line(self):
        source = '// continued \\\nCONFIG_LS_BOARD_IGNORED\n#if \\\nCONFIG_LS_BOARD_BAD'
        self.assertEqual([(4, 'CONFIG_LS_BOARD_BAD')], references(source))

    def test_boundaries_and_file_types(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            with self.assertRaises(FileNotFoundError):
                check(root)
            for file in ('components/lakeshark/board/ls_board.h',
                         'components/lakeshark/board_extra/test.hpp', 'main/test.cc'):
                path = root / file
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text('#if CONFIG_LS_BOARD_BAD')
            self.assertEqual(2, len(check(root)))
            (root / 'main/non_utf8.c').write_bytes(b'/* \xfc */\n#if CONFIG_LS_BOARD_BAD')
            self.assertEqual(3, len(check(root)))


if __name__ == '__main__':
    unittest.main()
