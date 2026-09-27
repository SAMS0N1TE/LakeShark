"""Record the board's screen as a video.

The board streams its character grid ('tui rec'), only the cells that
change, over the console at a faster rate for the length of the recording.
This puts the frames back together, draws each one with the same renderer
the board uses (bench lssim, 'play'), and hands them to ffmpeg.

    python tools/ls_record.py -o compass.mp4 --seconds 20
    python tools/ls_record.py -o find.mp4 \\
        --scene 6 "call ui.screen 9" "tui key 1" \\
        --scene 6 "tui key 2" "find view dial" \\
        --scene 6 "find view radar"

A scene is a length in seconds and the console commands that set it up.
The board takes console commands only between recordings, so each scene's
commands run first, then it is recorded, and the scenes are joined without
the gaps. With no --scene, one scene of --seconds after --before.

The map's picture comes along whenever it changes, at most four times a
second. Other pixels an app paints itself (a waterfall) are not sent: the
video shows what those cells hold. Needs bench/build/lssim.exe (the host
bench build) and ffmpeg.
"""
import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile
import time

import serial

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ls_console  # noqa: E402  (the board is found by serial number there)

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LSSIM = os.path.join(ROOT, "bench", "build", "lssim.exe" if os.name == "nt" else "lssim")
NATIVE_W, NATIVE_H = 568, 1232
BEGIN = re.compile(r"tui: rec begin cols (\d+) rows (\d+) cw (\d+) ch (\d+) font (\d+) theme (.+?) "
                   r"daylight (\d) rotation (\d) seconds (\d+) baud (\d+)")
FRAME_MS = 40


def unique_path(path):
    """`path`, or `name_2.ext`, `name_3.ext`... whichever does not exist yet."""
    if not path or not os.path.exists(path):
        return path
    stem, ext = os.path.splitext(path)
    n = 2
    while os.path.exists("%s_%d%s" % (stem, n, ext)):
        n += 1
    return "%s_%d%s" % (stem, n, ext)


def ffmpeg():
    for p in ("ffmpeg", r"C:\ffmpeg\bin\ffmpeg.exe"):
        try:
            subprocess.run([p, "-version"], capture_output=True, check=True)
            return p
        except (OSError, subprocess.CalledProcessError):
            continue
    sys.exit("ffmpeg not found")


def open_board(port):
    s = serial.serial_for_url(port, do_not_open=True)
    s.baudrate = 115200
    s.timeout = 0.05
    s.dtr = False          # low before open: on a CH343 the host asserting them resets the board
    s.rts = False
    s.open()
    try:
        s.set_buffer_size(rx_size=1 << 22, tx_size=4096)
    except (AttributeError, NotImplementedError):
        pass
    return s


def record_scene(s, seconds, baud, cmds):
    """Run the scene's commands, then record it. Returns the begin line and the stream."""
    for cmd in [""] + cmds:
        s.write(cmd.encode() + b"\r\n")
        time.sleep(1.5 if cmd else 0.5)
        s.read(1 << 20)
    s.reset_input_buffer()
    s.write(("tui rec %d %d\r\n" % (seconds, baud)).encode())
    # The board waits 300 ms after its begin line before the first frame:
    # change rate as soon as that line is whole.
    head, text = None, b""
    end = time.time() + 5
    while time.time() < end and not head:
        text += s.read(256)
        at = text.find(b"tui: rec begin")
        m = BEGIN.search(text.decode("latin-1"))
        if m and b"\n" in text[at:]:
            head = m
    if not head:
        sys.exit("the board did not start recording:\n" + text.decode("latin-1")[-400:])
    s.baudrate = baud
    # For a script driving something else in time with the scene.
    print("recording %d s" % seconds, flush=True)
    data = bytearray()
    start = time.time()
    while time.time() - start < seconds + 8:
        data += s.read(1 << 16)
        if b"tui: rec end" in data[-4096:]:
            break
    time.sleep(0.2)
    s.baudrate = 115200
    return head, data.decode("latin-1")


def frames_of(head, text, images):
    """Frames as (ms, cells, glass, picture index or -1). Pictures are appended
    to images as (x, y, w, h, pixels wide, high, RGB565 little-endian bytes)."""
    cols, rows = int(head.group(1)), int(head.group(2))
    grid = bytearray(cols * rows * 2)
    nglass = (cols * rows + 7) // 8
    glass, pic, building = bytes(nglass), -1, None
    frames, t, bad = [], None, 0
    prev = bytes(grid)
    row = cols * 2
    for line in text.splitlines():
        line = line.strip("\r")
        if line.startswith("~F "):
            t = int(line[3:])
            prev = bytes(grid)              # a row copy takes the row as it was last frame
        elif line.startswith("~I ") and t is not None:
            try:
                building = ([int(x) for x in line[3:].split()], bytearray())
                if len(building[0]) != 6:
                    raise ValueError
            except ValueError:
                building, bad = None, bad + 1
        elif line.startswith("~P ") and t is not None:
            if building is None:
                continue
            try:
                runs = bytes.fromhex(line[3:])
                px = building[1]
                for i in range(0, len(runs) - 2, 3):
                    px += bytes((runs[i + 2], runs[i + 1])) * runs[i]
            except ValueError:
                building, bad = None, bad + 1
        elif line == "~N" and t is not None:
            pic, building, glass = -1, None, bytes(nglass)
        elif line.startswith("~G ") and t is not None:
            try:
                g = bytes.fromhex(line[3:])
                if len(g) != nglass:
                    raise ValueError
                glass = g
            except ValueError:
                bad += 1
        elif line.startswith("~C ") and t is not None:
            try:
                r, k = (int(x) for x in line[3:].split())
                if not (0 <= r < rows and 0 <= k < rows):
                    raise ValueError
                grid[r * row:(r + 1) * row] = prev[k * row:(k + 1) * row]
            except ValueError:
                bad += 1
        elif line == "~E":
            if building is not None:
                (x, y, w, h, iw, ih), px = building
                if len(px) == iw * ih * 2:
                    images.append((x, y, w, h, iw, ih, bytes(px)))
                    pic = len(images) - 1
                else:
                    bad += 1
                building = None
            if t is not None:
                frames.append((t, bytes(grid), glass if pic >= 0 else bytes(nglass), pic))
            t = None
        elif line.startswith("~") and t is not None:
            try:
                r, c, hexes = line[1:].split(" ", 2)
                r, c = int(r), int(c)
                cells = bytes.fromhex(hexes)
                i = (r * cols + c) * 2
                if r >= rows or i + len(cells) > len(grid):
                    raise ValueError
                grid[i:i + len(cells)] = cells
            except ValueError:
                bad += 1
    return cols, rows, frames, bad


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("-o", "--out", required=True, help="the video to write (.mp4)")
    ap.add_argument("--seconds", type=int, default=20, help="length, with no --scene")
    ap.add_argument("--before", nargs="*", default=[], help="console commands first, with no --scene")
    ap.add_argument("--scene", nargs="+", action="append", default=[], metavar="SECONDS [COMMAND ...]",
                    help="a scene: seconds, then the console commands that set it up")
    ap.add_argument("--fps", type=int, default=50,
                    help="output rate; the board draws about 25 a second, so 50 shows each frame "
                         "within 20 ms of when it was drawn")
    ap.add_argument("--scale", type=int, default=2,
                    help="whole-pixel enlargement: the glass is 568 wide, and a player enlarging it "
                         "smooths the glyphs; at 2 the colour is also sampled at the glass's own pixels")
    ap.add_argument("--crf", type=int, default=12, help="x264 quality, lower is better")
    ap.add_argument("--replay", help="encode this .lsrec again instead of recording")
    ap.add_argument("--gif", action="store_true", help="also write a GIF beside the video, at the glass's own size")
    ap.add_argument("--gif-fps", type=int, default=12)
    ap.add_argument("--baud", type=int, default=2000000)
    ap.add_argument("--port", help="device path or rfc2217:// URL, overriding --serial")
    ap.add_argument("--serial", default="5C84301528", help="the board's USB serial number")
    ap.add_argument("--keep", help="also keep the recording (.lsrec) here")
    ap.add_argument("--log", help="write the console's own lines (everything that is not a frame) here")
    args = ap.parse_args()
    # A take never replaces an earlier one.
    args.out = unique_path(args.out)
    args.keep = unique_path(args.keep)
    args.gif_out = unique_path(os.path.splitext(args.out)[0] + ".gif") if args.gif else None

    if args.replay:
        replay(args)
        return
    scenes = [(int(sc[0]), sc[1:]) for sc in args.scene] or [(args.seconds, args.before)]
    port = args.port or ls_console.find_port(args.serial)
    if not port:
        sys.exit("no board with serial %s" % args.serial)

    s = open_board(port)
    frames, offset, shape, head0, log, images = [], 0, None, None, [], []
    for n, (seconds, cmds) in enumerate(scenes, 1):
        head, text = record_scene(s, seconds, args.baud, cmds)
        cols, rows, got, bad = frames_of(head, text, images)
        log.append("\n".join(l for l in text.splitlines() if not l.startswith("~")))
        end = re.search(r"tui: rec end (\d+) frames (\d+) skipped", text)
        print("scene %d: %d frames over %.1f s, %s skipped by the board, %d bad lines"
              % (n, len(got), (got[-1][0] if got else 0) / 1000, end.group(2) if end else "?", bad), flush=True)
        if not got:
            continue
        if shape and shape != (cols, rows):
            sys.exit("scene %d is %dx%d, the first was %dx%d: the board turned" % (n, cols, rows, *shape))
        shape, head0 = (cols, rows), head0 or head
        first = got[0][0]
        frames += [(offset + fr[0] - first,) + fr[1:] for fr in got]
        offset = frames[-1][0] + FRAME_MS
    s.close()
    if args.log:
        with open(args.log, "w", encoding="utf-8") as f:
            f.write("\n\n".join(log))
    if not frames:
        sys.exit("no frames came through")

    cols, rows = shape
    landscape = cols > rows
    span = frames[-1][0]
    # A fixed rate for the video: each output frame shows the newest grid
    # the board had sent by then.
    out_frames, j = [], 0
    for k in range(int(span * args.fps / 1000) + 1):
        tk = k * 1000 / args.fps
        while j + 1 < len(frames) and frames[j + 1][0] <= tk:
            j += 1
        out_frames.append(frames[j][1:])

    for path in (args.out, args.keep):
        if path and os.path.dirname(os.path.abspath(path)):
            os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    work = tempfile.mkdtemp(prefix="lsrec_")
    rec = args.keep or os.path.join(work, "take.lsrec")
    # LSREC2: the header line, the pictures (six little-endian int16s, then
    # the pixels), then each frame's cells, glass bits and int32 picture index.
    with open(rec, "wb") as f:
        f.write(("LSREC2 %d %d %d %s %s %s fps %d images %d\n" % (cols, rows, int(landscape), head0.group(5),
                                                                  head0.group(6).replace(" ", "_"), head0.group(7),
                                                                  args.fps, len(images))).encode())
        for x, y, w, h, iw, ih, px in images:
            f.write(struct.pack("<6h", x, y, w, h, iw, ih))
            f.write(px)
        for g, glass, pic in out_frames:
            f.write(g)
            f.write(glass)
            f.write(struct.pack("<i", pic))
    encode(rec, landscape, args.fps, args.out, args.scale, args.crf, work, args.gif_out, args.gif_fps)


def encode(rec, landscape, fps, out, scale, crf, work, gif=None, gif_fps=12):
    """Draw a .lsrec with the board's renderer and encode it."""
    rgb = os.path.join(work, "take.rgb")
    r = subprocess.run([LSSIM, "play", rec, rgb], capture_output=True, text=True)
    print(r.stdout.strip() or r.stderr.strip())
    if r.returncode:
        sys.exit("lssim play failed")
    # Nearest neighbour keeps every glyph pixel a hard-edged square; the
    # yuv420 colour planes are then as fine as the glass itself.
    filters = ["scale=iw*%d:ih*%d:flags=neighbor" % (scale, scale)] if scale > 1 else []
    if landscape:
        filters.append("transpose=2")
    vf = ["-vf", ",".join(filters)] if filters else []
    cmd = [ffmpeg(), "-y", "-loglevel", "error", "-f", "rawvideo", "-pix_fmt", "rgb24",
           "-s", "%dx%d" % (NATIVE_W, NATIVE_H), "-r", str(fps), "-i", rgb] + vf + \
          ["-c:v", "libx264", "-preset", "slow", "-tune", "animation", "-crf", str(crf),
           "-pix_fmt", "yuv420p", "-colorspace", "bt709", "-color_primaries", "bt709",
           "-color_trc", "bt709", "-color_range", "tv", "-movflags", "+faststart", out]
    subprocess.run(cmd, check=True)
    frames = os.path.getsize(rgb) // (NATIVE_W * NATIVE_H * 3)
    print("wrote %s: %d frames at %d fps, %.1f s, %dx scale" % (out, frames, fps, frames / fps, scale), flush=True)
    if gif:
        # The glass uses a small palette, so a 64-colour GIF without dither is exact.
        src = ["-f", "rawvideo", "-pix_fmt", "rgb24", "-s", "%dx%d" % (NATIVE_W, NATIVE_H), "-r", str(fps), "-i", rgb]
        chain = "fps=%d" % gif_fps + (",transpose=2" if landscape else "")
        pal = os.path.join(work, "palette.png")
        subprocess.run([ffmpeg(), "-y", "-loglevel", "error"] + src +
                       ["-vf", chain + ",palettegen=max_colors=64:stats_mode=full:reserve_transparent=0", pal],
                       check=True)
        subprocess.run([ffmpeg(), "-y", "-loglevel", "error"] + src + ["-i", pal, "-lavfi",
                        chain + "[v];[v][1:v]paletteuse=dither=none:diff_mode=rectangle", "-loop", "0", gif],
                       check=True)
        print("wrote %s: GIF at %d fps, %.1f MB" % (gif, gif_fps, os.path.getsize(gif) / 1e6), flush=True)
    os.remove(rgb)


def replay(args):
    with open(args.replay, "rb") as f:
        head = f.readline().decode("latin-1").split()
    landscape = head[3] == "1"
    fps = int(head[head.index("fps") + 1]) if "fps" in head else args.fps
    encode(args.replay, landscape, fps, args.out, args.scale, args.crf, tempfile.mkdtemp(prefix="lsrec_"),
           args.gif_out, args.gif_fps)


if __name__ == "__main__":
    main()
