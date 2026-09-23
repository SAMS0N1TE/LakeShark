"""Push a local file onto the board's SD card over the console.

Chunked base64, because the console splits arguments on whitespace and a
P25 profile is text full of newlines. Verifies the byte count the board
reports against what was sent, and lists the card afterwards - a transfer
that says it worked and did not is worse than one that fails.
"""
import sys, time, base64, serial

local = sys.argv[1]
remote = sys.argv[2]
port = sys.argv[3] if len(sys.argv) > 3 else "COM13"
CHUNK = 96                      # base64 chars per line, well inside the console's limit

data = open(local, "rb").read()

s = serial.Serial()
s.port = port
s.baudrate = 115200
s.timeout = 0.2
s.dtr = False
s.rts = False
s.open()

# Opening the port toggles the auto-reset lines on most hosts, so the board
# is usually mid-boot right here and its log would be read as command
# replies. Wait for the console to stop talking rather than guessing a delay.
_start = _last = time.time()
while time.time() - _start < 45:
    _n = s.in_waiting
    if _n:
        s.read(_n)
        _last = time.time()
    time.sleep(0.1)
    if time.time() - _last > 3 and time.time() - _start > 5:
        break
s.write(b"\r\n")
s.flush()
time.sleep(0.5)
s.reset_input_buffer()


def send(cmd, wait=2.0):
    s.write((cmd + "\r\n").encode())
    s.flush()
    end = time.time() + wait
    buf = b""
    while time.time() < end:
        n = s.in_waiting
        if n:
            buf += s.read(n)
            end = max(end, time.time() + 0.3)
    return buf.decode("utf-8", "replace")


print("truncating %s" % remote, flush=True)
r = send("sd put %s" % remote, 3.0)
if "truncated" not in r:
    print("  unexpected:", r.strip()[-200:])

b64 = base64.b64encode(data).decode()
# Each chunk must be a whole number of base64 quanta or the decoder loses
# the partial group at the boundary.
step = (CHUNK // 4) * 4
sent = 0
for i in range(0, len(b64), step):
    piece = b64[i:i + step]
    r = send("sd put %s %s" % (remote, piece), 3.0)
    if "+" not in r or "bytes ->" not in r:
        print("  FAILED at offset %d: %s" % (i, r.strip()[-200:]))
        s.close()
        raise SystemExit(1)
    got = int(r.split("+")[1].split(" ")[0])
    sent += got
    print("  %d/%d bytes" % (sent, len(data)), flush=True)

print("\nsent %d bytes, file is %d bytes" % (sent, len(data)))
print(send("sd ls", 4.0).strip()[-600:])
s.close()
