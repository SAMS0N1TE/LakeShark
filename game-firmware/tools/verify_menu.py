"""Exercise the landscape menu, settings and selected games over LakeShark UART."""

from argparse import ArgumentParser
from pathlib import Path
from re import search
from time import monotonic, sleep

from serial import Serial


BUTTON_KEYS = dict(zip(("Alt", "A", "X", "Y", "B", "Stick"), "56789k"))


class Board:
    def __init__(self, port, log):
        self.port = port
        self.log = log

    def command(self, keys):
        for key in keys.encode("ascii"):
            self.port.write(bytes((key,)))
            sleep(0.06)  # One command per displayed frame, including a slow map frame.

    def expect(self, pattern, timeout=5, accept=None, poll_status=True):
        deadline = monotonic() + timeout
        poll = 0
        while monotonic() < deadline:
            if poll_status and monotonic() >= poll:
                self.port.write(b"?")
                poll = monotonic() + 0.3
            text = self.port.readline().decode("utf-8", errors="replace").strip()
            if not text:
                continue
            self.log.write(text + "\n")
            if "Guru Meditation" in text or "assert failed" in text:
                panic_deadline = monotonic() + 1.5
                while monotonic() < panic_deadline:
                    self.log.write(self.port.readline().decode("utf-8", errors="replace"))
                raise RuntimeError(text)
            result = search(pattern, text)
            if result and (accept is None or accept(result)):
                print(text, flush=True)
                return result
        raise RuntimeError(f"Board did not report {pattern!r}")

    def reset(self):
        self.port.dtr = False
        self.port.rts = True
        sleep(0.15)
        self.port.rts = False
        self.port.reset_input_buffer()


def verify_settings(board):
    board.command("SSSSSSSSg")
    match = board.expect(r"page=settings row=0.*brightness=(\d+)")
    brightness = int(match[1])
    key = "A" if brightness > 20 else "D"
    board.command(key)
    changed = brightness - 5 if key == "A" else brightness + 5
    board.expect(fr"page=settings row=0.*brightness={changed}\b")
    board.command("q")
    board.expect(fr"page=library .*brightness={changed}\b")

    board.reset()
    board.expect(fr"page=library row=0 .*brightness={changed}\b", timeout=8)
    board.command("SSSSSSSSg")
    board.expect(fr"page=settings row=0.*brightness={changed}\b")
    board.command("D" if key == "A" else "A")
    board.command("SSSSSSg")
    board.expect(r"page=range", timeout=5)
    board.command("q")
    board.expect(r"page=settings row=6")
    board.command("Sg")
    board.expect(r"page=bindings row=0")
    board.command("g")
    board.expect(r"page=bind row=0")
    board.command("4")  # Physical Menu button cancels capture.
    board.expect(r"page=bindings row=0")
    board.command("S")
    bindings = board.expect(r"page=bindings row=1.*gba_a=(\w+) gba_b=(\w+)")
    original_a, original_b = bindings.groups()
    board.command("g" + BUTTON_KEYS[original_b])
    swapped = fr"gba_a={original_b} gba_b={original_a}\b"
    board.expect(r"page=bindings row=1.*" + swapped)
    board.command("qq")
    board.expect(fr"page=library .*brightness={brightness}\b.*" + swapped)
    board.reset()
    board.expect(r"page=library row=0 .*" + swapped, timeout=8)
    board.command("SSSSSSSSgSSSSSSSgSg" + BUTTON_KEYS[original_a])
    restored = fr"gba_a={original_a} gba_b={original_b}\b"
    board.expect(r"page=bindings row=1.*" + restored)
    board.command("qq")
    board.expect(fr"page=library .*brightness={brightness}\b.*" + restored)
    board.reset()
    board.expect(fr"page=library row=0 .*brightness={brightness}\b.*" + restored, timeout=8)


def main():
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("port")
    parser.add_argument("--log", type=Path, default=Path("menu-verification.log"))
    parser.add_argument("--gba", action="store_true")
    parser.add_argument("--settings", action="store_true")
    args = parser.parse_args()
    with Serial(args.port, 115200, timeout=0.12) as port, args.log.open("w", encoding="utf-8") as log:
        board = Board(port, log)
        board.expect(r"page=library", timeout=8)
        board.command("5")
        board.expect(r"page=settings row=0")
        board.command("9")
        board.expect(r"page=library row=0")
        if args.settings:
            verify_settings(board)
        board.command("1")
        board.expect(r"flight: TIDAL RELAY state=0")
        board.command("4")
        board.expect(r"page=paused row=0")
        board.command("6")
        board.expect(r"flight: TIDAL RELAY state=0")
        board.command("4")
        board.expect(r"page=paused row=0")
        board.command("9")
        board.expect(r"flight: TIDAL RELAY state=0")
        board.command("2")
        board.expect(r"flight: SALVAGE RUN state=0")
        if args.gba:
            board.command("b")
            board.expect(r"gba: video_frames=[1-9]\d*")
        board.command("m")
        board.expect(r"page=library row=0")


if __name__ == "__main__":
    main()
