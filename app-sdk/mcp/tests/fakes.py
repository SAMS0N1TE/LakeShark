"""Test-only adapters. No port discovery and no real serial implementation."""
import base64
import io
from pathlib import Path
import struct
import sys
import types
import zlib

SDK = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(SDK / "tests"))
from test_lsdk_install import FakeBoard as BaseBoard, FakeSerial


def png():
    from PIL import Image
    out = io.BytesIO()
    Image.new("RGB", (12, 16), "navy").save(out, "PNG")
    return out.getvalue()


class FakeBoard(BaseBoard):
    def __init__(self):
        super().__init__()
        self.bad_crc = False
        self.open_error = False

    def line(self, text):
        if text.startswith("tui png"):
            self.lines_in.append(text)
            data = png()
            self.say("tui: png begin mcp 12x16")
            for i in range(0, len(data), 57):
                self.say("~" + base64.b64encode(data[i:i + 57]).decode())
            self.say("[app demo] interleaved log")
            crc = (zlib.crc32(data) ^ int(self.bad_crc)) & 0xffffffff
            self.say("tui: png end mcp %d bytes crc %08x" % (len(data), crc))
        elif text == "status":
            self.lines_in.append(text)
            self.say("status: fake board")
            self.say("status: second line")
        elif text.startswith("term ev "):
            self.lines_in.append(text)  # deliberately silent, like the firmware
        elif text == "sd format":
            self.lines_in.append(text)
            self.say("sd: formatted fake storage")
        elif text.startswith("desk open ") and self.open_error:
            self.lines_in.append(text)
            self.say("desk: busy, try again")
        else:
            super().line(text)


def fake_serial_module(board):
    FakeSerial.board = board
    serial = types.ModuleType("serial")
    serial.Serial, serial.SerialException = FakeSerial, OSError
    tools = types.ModuleType("serial.tools")
    ports = types.ModuleType("serial.tools.list_ports")
    def forbidden():
        raise AssertionError("test attempted port discovery")
    ports.comports = forbidden
    serial.tools, tools.list_ports = tools, ports
    sys.modules.update({"serial": serial, "serial.tools": tools, "serial.tools.list_ports": ports})


def serial_opener(board):
    port = FakeSerial()
    port.board, port.timeout = board, 0.05
    return port


def elf(app_id):
    """Tiny ELF fixture accepted by the real lap.Elf manifest reader."""
    names = b"\0.shstrtab\0.lap_app\0"
    offset = (172 + len(names) + 3) & ~3
    data = bytearray(offset + 228)
    data[:7] = b"\x7fELF\x01\x01\x01"
    struct.pack_into("<HHIIIIIHHHHHH", data, 16, 3, 243, 1, 0, 0, 52, 2, 52, 0, 0, 40, 3, 1)
    struct.pack_into("<10I", data, 92, 1, 3, 0, 0, 172, len(names), 0, 0, 1, 0)
    struct.pack_into("<10I", data, 132, 11, 1, 2, 0, offset, 228, 0, 0, 4, 0)
    data[172:172 + len(names)] = names
    struct.pack_into("<IHH", data, offset, 0x3150414c, 2, 2)
    at = offset + 8
    for value, size in ((app_id, 32), ("DEMO", 16), ("fake", 32), ("1.0", 16), ("test", 24)):
        raw = value.encode()
        data[at:at + len(raw)] = raw
        at += size
    return bytes(data)


def child(mode, workspace):
    """Launch the actual lsdk mcp command with host adapters injected only here."""
    import lsdk
    import lap
    import lapboard
    import subprocess
    board = FakeBoard()
    fake_serial_module(board)
    original_run = subprocess.run
    stub = Path(__file__).with_name("stub_lssim.py")

    def fake_build(app, output=None, log=None):
        source = next(Path(app).glob("*.c"))
        if "INVALID" in source.read_text():
            raise lap.CompileError(str(source) + ":7:3: error: fake compiler failure\n")
        output = Path(output)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(elf(lsdk.source_id(app)))
        return output

    def sim_build(app, say=print):
        say("stub sim build")
        return lsdk.BuildResult(True, [], "stub build log")

    def run(args, **kw):
        if args and str(args[0]) == str(stub):
            args = [sys.executable, str(stub)] + args[1:]
        return original_run(args, **kw)

    lap.build = fake_build
    lsdk.sim_build, lsdk.sim_copy = sim_build, lambda app: stub
    subprocess.run = run
    if mode == "serial":
        lapboard.find_bridge = lambda: None
        serial_link = lapboard.SerialLink
        lapboard.SerialLink = lambda port=None: serial_link("FAKE", opener=lambda port: serial_opener(board))
        board.say("[app demo] fake serial log")
    raise SystemExit(lsdk.main(["mcp", "--workspace", workspace]))


if __name__ == "__main__":
    sys.path.insert(0, str(SDK))
    child(*sys.argv[1:])
