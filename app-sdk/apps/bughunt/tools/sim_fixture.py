#!/usr/bin/env python3
"""sim_fixture: a sweep for the simulator, so BUGHUNT's screens can be looked
at full instead of empty.

Writes a pcapng (Wi-Fi radiotap and Bluetooth LE link-layer records, the two
capture sources lssim replays) and prints a LAPSIM_SCRIPT for the LoRa radio's
RSSI: a noise floor with jitter, a steady 433.92 MHz carrier, a cellular
tracker on 835-839 MHz checking in every 75 s for 20 s, and a few irregular
phone bursts in B28. The Bluetooth side has a separated AirTag for the whole
run, a Tile that passes by and a phone; the Wi-Fi side a Hikvision camera
uploading steadily and a phone with a random address.

  python3 sim_fixture.py OUT.pcapng [SECONDS] > script.txt
  LAPSIM=bughunt LAPSIM_CAPTURE=OUT.pcapng LAPSIM_SCRIPT="$(cat script.txt);20:key=r" \\
      lssim lap -A 40 -f 3000 -o out.bmp

Frames are 40 ms (lssim -A 40); the script starts at frame 1.
"""
import struct
import sys

FPS = 25


def block(btype, body):
    pad = (-len(body)) % 4
    total = 12 + len(body) + pad
    return struct.pack("<II", btype, total) + body + b"\0" * pad + struct.pack("<I", total)


def shb():
    return block(0x0A0D0D0A, struct.pack("<IHHq", 0x1A2B3C4D, 1, 0, -1))


def idb(link, snap=4096):
    return block(1, struct.pack("<HHI", link, 0, snap) + struct.pack("<HH", 0, 0))


def epb(ifc, t_us, data, orig=None):
    orig = len(data) if orig is None else orig
    return block(6, struct.pack("<IIIII", ifc, t_us >> 32, t_us & 0xFFFFFFFF, len(data), orig) + data)


def radiotap(rssi, mhz=2437):
    # present: flags(1) rate(2) channel(3) antsignal(5)
    present = (1 << 1) | (1 << 2) | (1 << 3) | (1 << 5)
    body = struct.pack("<BB", 0, 12) + struct.pack("<HH", mhz, 0x00A0) + struct.pack("<b", rssi)
    hdr = struct.pack("<BBHI", 0, 0, 8 + len(body) + 1, present)
    return hdr + body + b"\0"


def mac(s):
    return bytes(int(x, 16) for x in s.split(":"))


def data_up(sta, ap, payload=8):
    fc = struct.pack("<BB", 0x08, 0x01)
    return fc + b"\0\0" + ap + sta + ap + b"\x10\x00" + b"\xAA" * payload


def beacon(bssid, ssid):
    fc = struct.pack("<BB", 0x80, 0x00)
    hdr = fc + b"\0\0" + b"\xff" * 6 + bssid + bssid + b"\x20\x00"
    body = b"\0" * 8 + struct.pack("<HH", 100, 0x0011) + bytes([0, len(ssid)]) + ssid + bytes([3, 1, 6])
    return hdr + body


def ble_ll(addr, ad, rssi, pdu_type=2, random=True):
    adv = addr[::-1]                      # on air: little endian
    payload = adv + ad
    hdr = struct.pack("<BB", pdu_type | (0x40 if random else 0), len(payload))
    phdr = struct.pack("<BbbBIH", 0, rssi, -100, 0, 0x8E89BED6, 0x0002 | 0x0001)
    return phdr + struct.pack("<I", 0x8E89BED6) + hdr + payload + b"\0\0\0"


AIRTAG = bytes([0x1E, 0xFF, 0x4C, 0x00, 0x12, 0x19, 0x10] + list(range(0x40, 0x56)) + [0x01, 0x00])
TILE = bytes([0x03, 0x03, 0xED, 0xFE, 0x05, 0x16, 0xED, 0xFE, 0x01, 0x02])
PHONE = bytes([0x0A, 0xFF, 0x4C, 0x00, 0x10, 0x05, 0x01, 0x18, 0x00, 0x00, 0x00])


def main():
    out = sys.argv[1]
    secs = int(sys.argv[2]) if len(sys.argv) > 2 else 300
    recs = []
    ap, cam, ph = mac("02:aa:00:00:00:01"), mac("44:19:b6:3c:21:7e"), mac("5a:11:22:33:44:55")
    for ms in range(0, secs * 1000, 40):           # the camera: 25 frames a second
        rt = radiotap(-52 - (ms // 700) % 5)
        recs.append((ms * 1000, 0, rt + data_up(cam, ap), len(rt) + 1400))
        if ms % 1000 == 0:
            recs.append((ms * 1000 + 7, 0, radiotap(-61) + beacon(ap, b"HomeNet"), None))
        if ms % 3000 == 120:
            rt = radiotap(-66)
            recs.append((ms * 1000 + 9, 0, rt + data_up(ph, ap), len(rt) + 220))
    tag, tile, phone = mac("c4:7a:11:5e:90:2b"), mac("d9:3f:60:12:aa:41"), mac("4a:10:33:9c:00:7f")
    for ms in range(0, secs * 1000, 2000):
        recs.append((ms * 1000 + 300, 1, ble_ll(tag, AIRTAG, -61 - (ms // 2000) % 4), None))
        if 30000 <= ms < 60000:
            recs.append((ms * 1000 + 600, 1, ble_ll(tile, TILE, -83, 0, False), None))
        recs.append((ms * 1000 + 900, 1, ble_ll(phone, PHONE, -48, 0), None))
    recs.sort(key=lambda r: r[0])
    with open(out, "wb") as f:
        f.write(shb() + idb(127) + idb(256))
        for t, ifc, data, orig in recs:
            f.write(epb(ifc, t, data, orig))

    steps = ["1:iorssi=-1095", "1:ionoise=70", "1:iorssif=433.92,-705"]
    f = lambda s: 1 + int(s * FPS)
    for k in range(secs // 75 + 1):                # the tracker
        on = 10 + 75 * k
        if on >= secs:
            break
        steps += ["%d:iorssif=835,-600" % f(on), "%d:iorssif=837,-640" % f(on), "%d:iorssif=839,-670" % f(on)]
        steps += ["%d:iorssif=835,0" % f(on + 20), "%d:iorssif=837,0" % f(on + 20), "%d:iorssif=839,0" % f(on + 20)]
    for on, dur in ((40, 6), (52, 5), (121, 7), (131, 4), (201, 6)):   # a phone, now and then
        if on < secs:
            steps += ["%d:iorssif=721,-690" % f(on), "%d:iorssif=723,-720" % f(on)]
            steps += ["%d:iorssif=721,0" % f(on + dur), "%d:iorssif=723,0" % f(on + dur)]
    steps.sort(key=lambda s: int(s.split(":")[0]))
    print(";".join(steps))


if __name__ == "__main__":
    main()
