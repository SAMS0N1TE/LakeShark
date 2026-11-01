"""Host integration and safety checks. All hardware is replaced before use.

Run: python -m unittest discover -s app-sdk/mcp/tests -v
"""
import base64
import importlib.util
import io
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import unittest

from fakes import SDK, FakeBoard, fake_serial_module, serial_opener

sys.path.insert(0, str(SDK))
sys.path.insert(0, str(SDK.parent / "host" / "lsterm"))
spec = importlib.util.spec_from_file_location("lakeshark_mcp", SDK / "mcp" / "server.py")
mcp = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mcp)


class Client:
    def __init__(self, workspace, port, mode="bridge", real=False):
        args = [sys.executable, str(SDK / "lsdk.py"), "mcp", "--workspace", str(workspace)] if real else [
            sys.executable, str(Path(__file__).with_name("fakes.py")), mode, str(workspace)]
        env = dict(os.environ, LSDK_BRIDGE_PORT=str(port), PYTHONIOENCODING="utf-8",
                   LSSIM_FRAMES="must-be-cleared", LSSIM_FEED_OUT="must-be-cleared")
        self.proc = subprocess.Popen(args, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     text=True, encoding="utf-8", env=env)
        self.messages, self.errors, self.seq, self.progress = queue.Queue(), [], 0, []
        def reader():
            for line in self.proc.stdout:
                try:
                    self.messages.put(json.loads(line))
                except ValueError:
                    self.messages.put({"invalid_stdout": line})
        def errors():
            self.errors.extend(self.proc.stderr)
        self.readers = [threading.Thread(target=reader), threading.Thread(target=errors)]
        for t in self.readers:
            t.start()

    def send(self, message):
        self.proc.stdin.write(json.dumps(message) + "\n")
        self.proc.stdin.flush()

    def request(self, method, params=None):
        self.seq += 1
        self.send({"jsonrpc": "2.0", "id": self.seq, "method": method, "params": params or {}})
        while True:
            try:
                response = self.messages.get(timeout=15)
            except queue.Empty:
                raise AssertionError("stdio timeout: " + "".join(self.errors))
            if response.get("method") == "notifications/progress":
                self.progress.append(response)
                continue
            assert response.get("id") == self.seq, response
            return response

    def initialize(self, version="2025-06-18"):
        response = self.request("initialize", {"protocolVersion": version, "capabilities": {}, "clientInfo": {"name": "host-test", "version": "1"}})
        self.send({"jsonrpc": "2.0", "method": "notifications/initialized"})
        return response

    def call(self, name, args=None, error=False, progress=False):
        params = {"name": name, "arguments": args or {}}
        if progress:
            params["_meta"] = {"progressToken": "upload"}
        r = self.request("tools/call", params)
        assert "result" in r, r
        r = r["result"]
        assert r["isError"] == error, r
        return json.loads(r["content"][0]["text"]), r

    def close(self):
        self.proc.stdin.close()
        try:
            self.proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait(timeout=5)
        for t in self.readers:
            t.join(timeout=2)
        self.proc.stdout.close()
        self.proc.stderr.close()


class HostTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.board = FakeBoard()
        fake_serial_module(cls.board)
        import lsweb
        cls.web = lsweb
        cls.server = lsweb.serve(lambda: "FAKE", 0)
        cls.port = cls.server.server_address[1]
        cls.thread = threading.Thread(target=cls.server.serve_forever)
        cls.thread.start()
        assert cls.server.server_address[0] == "127.0.0.1"
        assert lsweb.BOARD.up.wait(3)

    @classmethod
    def tearDownClass(cls):
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join()

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="ls-mcp-")
        self.addCleanup(self.temp.cleanup)
        self.work = Path(self.temp.name)

    def client(self, **kw):
        client = Client(self.work, self.port, **kw)
        self.addCleanup(client.close)
        return client

    def image(self, result, size=None):
        from PIL import Image
        item = result["content"][1]
        self.assertEqual(item["mimeType"], "image/png")
        data = base64.b64decode(item["data"], validate=True)
        with Image.open(io.BytesIO(data)) as im:
            self.assertEqual(im.format, "PNG")
            if size:
                self.assertEqual(im.size, size)
            im.verify()

    def test_each_tool_over_stdio_real_bridge_fake_serial_and_stub_sim(self):
        c = self.client()
        self.assertEqual(c.initialize()["result"]["protocolVersion"], "2025-06-18")
        tools = c.request("tools/list")["result"]["tools"]
        self.assertEqual(len(tools), 14)
        c.call("sdk_docs", {"query": "lap_clear"})
        api, _ = c.call("sdk_api", {"symbol": "lap_clear"})
        self.assertIn("lap_clear", api["matches"][0]["signature"])
        self.assertTrue(api["matches"][0]["doc"])
        c.call("new_app", {"name": "demo", "template": "blank"})
        apps, _ = c.call("list_apps")
        self.assertEqual(apps["apps"][0]["app"], "demo")
        c.call("write_file", {"app": "demo", "path": "notes/test.txt", "content": "hello π"})
        content, _ = c.call("read_file", {"app": "demo", "path": "notes/test.txt"})
        self.assertEqual(content["content"], "hello π")
        built, _ = c.call("build", {"app": "demo"})
        self.assertTrue(built["ok"])
        self.assertEqual(Path(built["lap"]).stat().st_size, built["size"])
        sim, r = c.call("sim_run", {"app": "demo", "frames": 5, "orientation": "landscape"})
        self.image(r, (16, 12))
        self.assertIn("stub lssim", sim["log"])
        events = [{"type": "key", "key": "o"}, {"type": "tap", "x": 20, "y": 30},
                  {"type": "tilt", "ax": 0.2, "ay": -0.3}, {"type": "swipe", "direction": "L"}, {"type": "rx", "count": 4}]
        sim, r = c.call("sim_input", {"app": "demo", "events": events})
        self.image(r)
        self.assertIn("6:key=o", sim["script"])
        self.assertIn("rx=4", sim["log"])
        installed, _ = c.call("install", {"app": "demo"}, progress=True)
        self.assertEqual(installed["via"], "bridge")
        self.assertTrue(installed["progress"])
        self.assertTrue(c.progress)
        self.assertIn("demo.lap", self.board.apps)
        self.assertIn("desk open demo", self.board.lines_in)
        console, _ = c.call("board_console", {"line": "status"})
        self.assertIn("second line", console["text"])
        _, r = c.call("board_screen")
        self.image(r)
        _, r = c.call("board_input", {"events": [{"type": "key", "key": "@enter"}, {"type": "tap", "x": 40, "y": 64}]})
        self.image(r)
        lines = [s for s in self.board.lines_in if s.startswith("term ev ")]
        self.assertTrue(all(len(s) <= 200 for s in lines))
        records = b"".join(bytes.fromhex(s[8:]) for s in lines)
        self.assertEqual(len(records) % 24, 0)
        self.assertEqual(records[0:3], bytes([1, 0, 0]))
        self.assertEqual(int.from_bytes(records[17:19], "little"), 6)
        self.web.heard("[app demo] available history")
        self.web.heard("[app other] hidden")
        logs, _ = c.call("board_logs", {"app": "demo", "n": 2})
        self.assertIn("[app demo] available history", logs["lines"])
        self.assertTrue(all("[app demo]" in s for s in logs["lines"]))
        resources = c.request("resources/list")["result"]["resources"]
        self.assertGreaterEqual(len(resources), 5)
        for resource in resources:
            self.assertTrue(c.request("resources/read", {"uri": resource["uri"]})["result"]["contents"][0]["text"])

    def test_serial_fallback_is_fake(self):
        c = self.client(mode="serial")
        c.initialize()
        c.call("new_app", {"name": "demo"})
        data, _ = c.call("install", {"app": "demo", "open": False})
        self.assertEqual(data["via"], "serial")
        _, r = c.call("board_screen")
        self.image(r)
        console, _ = c.call("board_console", {"line": "status"})
        self.assertIn("second line", console["text"])
        c.call("board_input", {"events": [{"type": "key", "key": "a"}]})
        logs, _ = c.call("board_logs", {"n": 10})
        self.assertEqual(logs["via"], "serial")

    def test_guards_and_validation_before_board_access(self):
        c = self.client()
        c.initialize()
        before = len(self.board.lines_in)
        for line in ("sd format", "sd rm /x", "crumb clear", "crash clear", "ota install", "flash image", "mesh tx on", "lora tx x", "lora lrtx-send", "mesh dm 1 hi", "say hello", "tx on", "cellreport test", "cellreport target 123"):
            data, _ = c.call("board_console", {"line": line}, error=True)
            self.assertIn("confirm=true", data["error"])
        for line in ("status\nsd format", "status;sd format", "status\r", "status\x00", "x" * 201):
            c.call("board_console", {"line": line}, error=True)
        c.call("board_console", {"line": "sd format", "confirm": "true"}, error=True)
        for args in ({"events": [{"type": "tilt", "ax": 1, "ay": 1}]}, {"events": [{"type": "tap"}]}, {"events": [{"type": "key", "key": "@invalid"}]}):
            c.call("board_input", args, error=True)
        self.assertEqual(len(self.board.lines_in), before)
        c.call("board_console", {"line": "sd format", "confirm": True})
        self.assertIn("sd format", self.board.lines_in[before:])

    def test_file_boundaries_and_structured_build_errors(self):
        c = self.client()
        c.initialize()
        c.call("new_app", {"name": "demo"})
        for path in ("../secret", "/absolute", "C:/secret", "C:secret", "a/../../secret", "a\\secret", ".git/config", "file:stream", "NUL", "CON.txt"):
            c.call("write_file", {"app": "demo", "path": path, "content": "no"}, error=True)
            c.call("read_file", {"app": "demo", "path": path}, error=True)
        for app in ("..", "../escape", "C:/escape"):
            c.call("new_app", {"name": app}, error=True)
        outside = self.work / "outside.txt"
        outside.write_text("outside")
        os.link(outside, self.work / "demo" / "linked.txt")
        c.call("write_file", {"app": "demo", "path": "linked.txt", "content": "no"}, error=True)
        c.call("read_file", {"app": "demo", "path": "linked.txt"}, error=True)
        self.assertEqual(outside.read_text(), "outside")
        (self.work / "demo" / "linked.txt").unlink()
        c.call("write_file", {"app": "demo", "path": "main.c", "content": "INVALID"})
        data, _ = c.call("build", {"app": "demo"}, error=True)
        self.assertEqual(data["problems"][0]["line"], 7)
        self.assertEqual(data["problems"][0]["col"], 3)
        self.assertEqual(data["problems"][0]["severity"], "error")
        c.call("sdk_api", {"symbol": "does_not_exist"}, error=True)

    def test_replay_failures_and_limits(self):
        c = self.client()
        c.initialize()
        c.call("new_app", {"name": "demo"})
        c.call("sim_input", {"app": "demo", "events": [{"type": "key", "key": "o"}]}, error=True)
        c.call("sim_run", {"app": "demo", "frames": 0}, error=True)
        c.call("sim_run", {"app": "demo", "frames": 1})
        data, _ = c.call("sim_input", {"app": "demo", "events": [{"type": "rx", "count": 99999}]}, error=True)
        self.assertIn("runtime failure", data["log"])
        c.call("sim_input", {"app": "demo", "events": [{"type": "tilt", "ax": 0, "ay": 0, "az": 0}]}, error=True)
        c.call("sim_input", {"app": "demo", "events": [{"type": "rx", "count": 1, "frame": 2000}]}, error=True)

    def test_board_png_crc_failure(self):
        c = self.client()
        c.initialize()
        self.board.bad_crc = True
        try:
            data, _ = c.call("board_screen", error=True)
            self.assertIn("CRC", data["error"])
        finally:
            self.board.bad_crc = False

    def test_install_failure_keeps_progress(self):
        c = self.client()
        c.initialize()
        c.call("new_app", {"name": "demo"})
        self.board.corrupt = True
        try:
            data, _ = c.call("install", {"app": "demo"}, error=True, progress=True)
            self.assertFalse(data["ok"])
            self.assertIn("SHA256", data["error"])
            self.assertTrue(data["progress"])
            self.assertTrue(c.progress)
        finally:
            self.board.corrupt = False

    def test_published_schemas_match_discovery(self):
        self.assertEqual(json.loads((SDK / "mcp" / "tools.json").read_text(encoding="utf-8")), {"tools": mcp.TOOLS})

    def test_install_success_but_open_failure(self):
        c = self.client()
        c.initialize()
        c.call("new_app", {"name": "demo"})
        self.board.open_error = True
        try:
            data, _ = c.call("install", {"app": "demo"}, error=True)
            self.assertTrue(data["installed"])
            self.assertIn("busy", data["open_error"])
            self.assertTrue(data["progress"])
        finally:
            self.board.open_error = False

    def test_symlink_escape(self):
        app = self.work / "demo"
        app.mkdir()
        (app / "main.c").write_text("source")
        outside = self.work / "outside"
        outside.mkdir()
        try:
            (app / "escape").symlink_to(outside, target_is_directory=True)
        except OSError as e:
            self.skipTest("symlink creation unavailable: " + str(e))
        sdk = mcp.LakeShark(self.work)
        with self.assertRaises(ValueError):
            sdk.file("demo", "escape/secret")
        with self.assertRaises(ValueError):
            sdk.safe_tree(app)

    def test_real_launcher_protocol_errors_and_resources(self):
        c = self.client(real=True)
        self.assertEqual(c.request("tools/list")["error"]["code"], -32000)
        self.assertEqual(c.initialize("future-version")["result"]["protocolVersion"], mcp.VERSIONS[-1])
        self.assertEqual(c.request("ping")["result"], {})
        self.assertEqual(c.request("unknown")["error"]["code"], -32601)
        self.assertEqual(c.request("tools/call", {"name": "unknown"})["error"]["code"], -32602)
        self.assertEqual(c.request("resources/read", {"uri": "file:///secret"})["error"]["code"], -32002)
        self.assertEqual(c.request("resources/templates/list")["result"], {"resourceTemplates": []})
        # No notification or client response may receive a reply or run a tool.
        c.send({"jsonrpc": "2.0", "method": "tools/call", "params": {"name": "new_app", "arguments": {"name": "surprise"}}})
        c.send({"jsonrpc": "2.0", "id": 999, "result": {}})
        self.assertEqual(c.request("ping")["result"], {})
        self.assertFalse((self.work / "surprise").exists())
        c.proc.stdin.write("{broken\n")
        c.proc.stdin.flush()
        self.assertEqual(c.messages.get(timeout=3)["error"]["code"], -32700)
        c.call("sdk_api", {"symbol": "lap_clear"})
        c.call("list_apps", {"extra": 1}, error=True)

    def test_wire_contract_and_quiet_rpc(self):
        records = mcp.wire_events([{"type": "tap", "x": 40, "y": 64}, {"type": "release_all"}])
        self.assertEqual([r[1] for r in records], [3, 4, 6])
        self.assertEqual(len(records[0]), 24)
        self.assertEqual(records[0][7:15], bytes([4, 0, 3, 0, 40, 0, 64, 0]))
        board = FakeBoard()
        link = mcp.lapboard.SerialLink("FAKE", opener=lambda port: serial_opener(board))
        self.assertEqual(link.rpc(["term ev 00"], None, 0.05), [])
        self.assertEqual(link.rpc(["status"], None, 1), ["status: fake board", "status: second line"])


if __name__ == "__main__":
    unittest.main()
