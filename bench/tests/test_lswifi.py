"""Serial-helper privacy and discovery regressions; no board required."""
import contextlib
import importlib.util
import io
from pathlib import Path
from types import SimpleNamespace
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("lswifi", Path(__file__).parents[1] / "lswifi.py")
wifi = importlib.util.module_from_spec(spec)
spec.loader.exec_module(wifi)


class Port:
    def __init__(self, response):
        self.response = response
        self.data = b""
        self.commands = []

    def reset_input_buffer(self):
        self.data = b""

    def write(self, data):
        self.commands.append(data)
        self.data = b"lakeshark>" if len(self.commands) == 1 else self.response

    @property
    def in_waiting(self):
        return len(self.data)

    def read(self, count):
        data, self.data = self.data[:count], self.data[count:]
        return data


class HelperTests(unittest.TestCase):
    def test_quoted_names_and_control_injection(self):
        self.assertEqual(wifi.quote('My "Net"\\AP'), '"My \\"Net\\"\\\\AP"')
        with self.assertRaises(ValueError):
            wifi.quote("home\nreboot")

    def test_wrapped_echo_is_discarded_and_prompt_in_ssid_is_kept(self):
        secret = "private phrase"
        command = 'wifi add "lakeshark>" "private phrase"'
        response = (f"lakeshark>{command[:24]}\r\n\x1b[32m{command[24:]}\x1b[0m\r\n"
                    "wifi: begin\r\nConnected: lakeshark>\r\nwifi: ESP_OK\r\nlakeshark>").encode()
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            self.assertEqual(wifi.exchange(Port(response), command, secret, 1), 0)
        self.assertEqual(out.getvalue(), "Connected: lakeshark>\n")
        self.assertNotIn(secret, out.getvalue())

    def test_failure_returns_nonzero_without_echo(self):
        out, err = io.StringIO(), io.StringIO()
        port = Port(b'wifi join home "secret123"\r\nwifi: begin\r\nwifi: ESP_FAIL\r\nlakeshark>')
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            self.assertEqual(wifi.exchange(port, 'wifi join home "secret123"', "secret123", 1), 1)
        self.assertEqual(out.getvalue(), "")
        self.assertEqual(err.getvalue(), "ESP_FAIL\n")

    def test_timeout_never_prints_partial_echo(self):
        out = io.StringIO()
        with contextlib.redirect_stdout(out), self.assertRaises(TimeoutError):
            wifi.exchange(Port(b'secret123'), 'wifi join home "secret123"', "secret123", .01)
        self.assertEqual(out.getvalue(), "")

    def test_multiple_configured_boards_require_explicit_port(self):
        configs = SimpleNamespace(read_text=lambda **kw: '{"configs":['
            '{"name":"one","usb_serial":"AAA"},{"name":"two","usb_serial":"BBB"}]}')
        ports = [SimpleNamespace(device="COM1", serial_number="aaa"),
                 SimpleNamespace(device="COM2", serial_number="BBB")]
        self.assertEqual(wifi.find_port(configs, "one", None, ports), "COM1")
        with self.assertRaisesRegex(ValueError, "Several boards.*specify --port"):
            wifi.find_port(configs, None, None, ports)
        self.assertEqual(wifi.find_port(configs, None, "COM3", ports), "COM3")

    def test_controls_are_set_before_open(self):
        events = []
        class Serial:
            def __init__(self, **kwargs):
                self.__dict__.update(kwargs)
                self.is_open = False
            def __setattr__(self, key, value):
                events.append((key, value))
                object.__setattr__(self, key, value)
            def __enter__(self): return self
            def __exit__(self, *args): pass
            def open(self):
                self.assert_controls = (self.dtr, self.rts)
                events.append(("open", self.port))
        serial = SimpleNamespace(Serial=Serial, SerialException=OSError)
        ports = SimpleNamespace(comports=lambda: [])
        with patch.dict("sys.modules", {"serial": serial, "serial.tools": SimpleNamespace(list_ports=ports)}), \
             patch.object(wifi, "exchange", return_value=0):
            self.assertEqual(wifi.main(["--port", "COM7", "status"]), 0)
        self.assertLess(events.index(("dtr", False)), events.index(("open", "COM7")))
        self.assertLess(events.index(("rts", False)), events.index(("open", "COM7")))


if __name__ == "__main__":
    unittest.main()
