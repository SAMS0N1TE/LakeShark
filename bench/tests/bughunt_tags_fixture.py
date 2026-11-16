"""A synthetic Bluetooth advertising capture for BUGHUNT's tag watch (once
BLE WATCH) and BLE EXPLORER.

pcapng with one LINKTYPE_BLUETOOTH_LE_LL_WITH_PHDR (256) interface, the
format lssim replays through LAPSIM_CAPTURE. Every frame is made up here:
trackers of each kind the app names, phones, beacons and a few named
devices, over fourteen minutes. One AirTag changes address half way.

    python bench/tests/bughunt_tags_fixture.py out.pcapng
"""
import struct
import sys

AA = 0x8E89BED6


def addr(text):
    """aa:bb:cc:dd:ee:ff as displayed -> the little-endian bytes on air."""
    return bytes(int(x, 16) for x in reversed(text.split(':')))


def ad(*structures):
    out = b''
    for kind, value in structures:
        out += bytes([len(value) + 1, kind]) + value
    return out


def flags(v=0x06):
    return (0x01, bytes([v]))


def name(text):
    return (0x09, text.encode())


def uuids(*u):
    return (0x03, b''.join(struct.pack('<H', x) for x in u))


def service_data(u, data):
    return (0x16, struct.pack('<H', u) + data)


def maker(company, data):
    return (0xFF, struct.pack('<H', company) + data)


def findmy(status, key_byte, separated=True):
    """Apple offline finding: 0x12, 25 bytes when separated, 2 when near the owner."""
    if separated:
        body = bytes([status]) + bytes((key_byte + i) & 0xFF for i in range(22)) + bytes([0x01, 0x00])
    else:
        body = bytes([status, 0x01])
    return maker(0x004C, bytes([0x12, len(body)]) + body)


def ll_record(channel_index, rssi, pdu_type, tx_random, adv_addr, data):
    payload = adv_addr + data
    header = bytes([(pdu_type & 0x0F) | (0x40 if tx_random else 0), len(payload)])
    phdr = struct.pack('<BbbBIH', channel_index, rssi, -98, 0, 0, 0x0003)
    return phdr + struct.pack('<I', AA) + header + payload + b'\0\0\0'


def block(kind, body):
    body += b'\0' * (-len(body) % 4)
    total = len(body) + 12
    return struct.pack('<II', kind, total) + body + struct.pack('<I', total)


def pcapng(records):
    """records: (time_us, bytes) in time order."""
    out = block(0x0A0D0D0A, struct.pack('<IHHq', 0x1A2B3C4D, 1, 0, -1))
    out += block(1, struct.pack('<HHI', 256, 0, 0))
    for t, rec in records:
        out += block(6, struct.pack('<IIIII', 0, t >> 32, t & 0xFFFFFFFF, len(rec), len(rec)) + rec)
    return out


T0 = 1_792_756_800_000_000      # 2026-10-23 12:00 UTC, the simulator's date
MIN = 60_000_000


def devices():
    """(address, random, on-air PDU type (0 ADV_IND, 2 ADV_NONCONN_IND), start_us, end_us, every_us, rssi(t), data)"""
    wobble = lambda base, slope=0: (lambda t: int(base + slope * t / MIN + ((t // 1_000_000) * 7919 % 7) - 3))
    airtag_a = addr('c4:5a:71:20:9e:13')
    airtag_b = addr('d9:02:4b:66:31:8a')
    return [
        # a stranger's AirTag, separated from its owner, with us all along; new address at 7 minutes
        (airtag_a, 1, 2, 0, 7 * MIN - 1, 2_000_000, wobble(-64, 1), ad(findmy(0x10, 0x31))),
        (airtag_b, 1, 2, 7 * MIN, 14 * MIN, 2_000_000, wobble(-58, 0.5), ad(findmy(0x10, 0x77))),
        # our own iPhone's Find My beacon: near its owner (short form)
        (addr('5e:11:22:33:44:55'), 1, 2, 0, 14 * MIN, 3_000_000, wobble(-48), ad(findmy(0x00, 0, separated=False))),
        # a Tile in a bag nearby, the whole time
        (addr('e1:7d:12:00:41:aa'), 1, 0, 30_000_000, 14 * MIN, 4_000_000, wobble(-77),
         ad(flags(), uuids(0xFEED), service_data(0xFEED, bytes.fromhex('0201a3b2c1d0e9f8')))),
        # a SmartTag that passes by between minutes 2 and 6
        (addr('f2:c8:5b:91:07:3e'), 1, 2, 2 * MIN, 6 * MIN, 3_000_000, wobble(-70),
         ad(flags(), service_data(0xFD5A, bytes.fromhex('1342a1b2c3d4e5f60708')))),
        # a Google Find My Device tag in unwanted-tracking mode, from minute 1
        (addr('c7:44:9a:10:2b:6f'), 1, 2, 1 * MIN, 14 * MIN, 2_500_000, wobble(-66),
         ad(flags(), service_data(0xFEAA, bytes([0x41]) + bytes(range(20)) + b'\x04'))),
        # someone's AirPods Pro out of the case for two minutes
        (addr('6a:2f:0c:51:9d:e2'), 1, 0, 3 * MIN, 5 * MIN, 1_000_000, wobble(-55),
         ad(maker(0x004C, bytes([0x07, 0x19, 0x01, 0x0E, 0x20, 0x55, 0xAA]) + bytes(20)))),
        # Fast Pair headphones, discoverable
        (addr('4c:87:5d:aa:10:02'), 1, 0, 0, 14 * MIN, 1_500_000, wobble(-72), ad(flags(), service_data(0xFE2C, bytes([0x0A, 0x1B, 0x2C])))),
        # phones and laptops
        (addr('7b:3e:10:4d:21:c0'), 1, 0, 0, 14 * MIN, 900_000, wobble(-52),
         ad(flags(0x1A), maker(0x004C, bytes([0x10, 0x05, 0x01, 0x18, 0x8C, 0x41, 0x2E])))),
        (addr('2c:41:a7:93:5f:01'), 1, 2, 0, 14 * MIN, 1_100_000, wobble(-80),
         ad(maker(0x0006, bytes.fromhex('0109200207c3a1e8f04a7d6b5c9e1f2a3b4c5d6e7f')))),
        (addr('00:1a:7d:da:71:13'), 0, 0, 0, 14 * MIN, 1_000_000, wobble(-61),
         ad(flags(), name('Pixel 8'), maker(0x00E0, bytes.fromhex('0100')))),
        (addr('d0:39:72:11:fe:0c'), 0, 0, 0, 14 * MIN, 1_200_000, wobble(-69),
         ad(flags(), name('Forerunner 265'), uuids(0x180D, 0x180F), maker(0x0087, bytes.fromhex('0a00')))),
        (addr('30:ae:a4:07:0d:64'), 0, 0, 0, 14 * MIN, 1_300_000, wobble(-58),
         ad(flags(), name('ESP32-C6 sensor'), uuids(0x181A), maker(0x02E5, bytes.fromhex('01')))),
        (addr('8c:79:f5:12:aa:41'), 0, 2, 0, 14 * MIN, 1_000_000, wobble(-74),
         ad(maker(0x0075, bytes.fromhex('42040180602c5a7e1b0f')))),
        # beacons
        (addr('f8:30:02:a1:5b:c9'), 1, 2, 0, 14 * MIN, 1_000_000, wobble(-83),
         ad(flags(0x04), maker(0x004C, bytes([0x02, 0x15]) + bytes(range(16)) + bytes([0, 1, 0, 2, 0xC5])))),
        (addr('e4:aa:5c:3f:07:91'), 1, 2, 0, 14 * MIN, 1_000_000, wobble(-79),
         ad(flags(0x06), uuids(0xFEAA), service_data(0xFEAA, bytes([0x10, 0xEB, 0x03]) + b'lakeshark' + bytes([0x07])))),
    ]


def build():
    records = []
    for i, (a, rnd, pdu, start, end, every, rssi, data) in enumerate(devices()):
        t = start + (i * 137_000) % every
        n = 0
        while t <= end:
            ch = (0, 12, 39)[n % 3]
            records.append((T0 + t, ll_record(ch, max(-100, min(-20, rssi(t))), pdu, rnd, a, data)))
            n += 1
            t += every
    records.sort(key=lambda r: r[0])
    return pcapng(records)


if __name__ == '__main__':
    data = build()
    with open(sys.argv[1], 'wb') as f:
        f.write(data)
    print(len(data), 'bytes')
