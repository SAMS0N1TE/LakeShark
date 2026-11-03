"""Headless validation of native viewer host operations; OS calls are recorded."""
import base64
import pathlib
import sys
import tempfile
import types
import unittest
from unittest import mock
ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'host/lsterm'))
import lsweb
import lsapp


class DesktopHost(unittest.TestCase):
    def test_https_and_bounded_binary_validation(self):
        self.assertEqual(lsweb.desktop_url('https://example.com/a?q=1#part'), 'https://example.com/a?q=1#part')
        for url in ('http://example.com', 'https://', 'https://user:pass@example.com', 'https://example.com/\n', 'https://example.com\\evil', 'javascript:alert(1)'):
            with self.subTest(url=url), self.assertRaises(ValueError):
                lsweb.desktop_url(url)
        data = bytes(range(256)) * 256
        self.assertEqual(lsweb.desktop_file('test.bin', base64.b64encode(data).decode()), data)
        for name, encoded in (('../evil', ''), ('a/b', ''), ('C:evil', ''), ('', ''), ('a', '!!!!'), ('big', base64.b64encode(data+b'x').decode())):
            with self.subTest(name=name), self.assertRaises(ValueError):
                lsweb.desktop_file(name, encoded)

    def test_native_save_dialog_cancel_write_and_browser(self):
        api = lsapp.DesktopHostApi()
        window = mock.Mock()
        api.window = window
        fake_webview = types.SimpleNamespace(FileDialog=types.SimpleNamespace(SAVE='save'))
        with mock.patch.dict(sys.modules, webview=fake_webview):
            window.create_file_dialog.return_value = None
            self.assertEqual(api.save_file('report.bin', 'AAEC/w=='), -8)
            with tempfile.TemporaryDirectory(dir=ROOT/'bench/build') as d:
                destination = pathlib.Path(d)/'chosen.bin'
                window.create_file_dialog.return_value = (str(destination),)
                self.assertEqual(api.save_file('report.bin', 'AAEC/w=='), 1)
                self.assertEqual(destination.read_bytes(), b'\x00\x01\x02\xff')
                self.assertEqual(list(pathlib.Path(d).iterdir()), [destination])
            self.assertEqual(api.save_file('../escape', ''), -5)
        with mock.patch('webbrowser.open', return_value=True) as open_browser:
            self.assertEqual(api.open_url('https://example.com'), 1)
            open_browser.assert_called_once_with('https://example.com', new=2)
            self.assertEqual(api.open_url('http://example.com'), -5)
            self.assertEqual(open_browser.call_count, 1)


if __name__ == '__main__':
    unittest.main()
