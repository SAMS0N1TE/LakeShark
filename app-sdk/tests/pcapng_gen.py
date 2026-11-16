"""Synthetic pcapng files for the capture tests: 802.11 with radiotap
(LINKTYPE 127), Bluetooth LE LL with PHDR (LINKTYPE 256) and LoRaTap
(LINKTYPE 270).

Everything here is made up for tests. Addresses use locally administered
02:xx prefixes unless a test needs a global one, SSIDs start with TEST-, and
key material is zero or a fixed pattern. Nothing is captured from the air."""
import struct

BCAST = b'\xff' * 6


def mac(text):
    return bytes(int(x, 16) for x in text.split(':'))


def _block(kind, body):
    pad = (-len(body)) % 4
    total = 12 + len(body) + pad
    return struct.pack('<II', kind, total) + body + b'\0' * pad + struct.pack('<I', total)


def _ch_mhz(ch):
    return 2484 if ch == 14 else 2407 + 5 * ch if ch <= 13 else 5000 + 5 * ch


def radiotap(ch, rssi, rate=2):
    """flags, rate, channel, antenna signal: the layout TAP writes."""
    present = 0x2 | 0x8 | 0x20 | (0x4 if rate else 0)
    body = bytes([0, rate & 0xFF]) + struct.pack('<HH', _ch_mhz(ch), 0x0080 if ch <= 14 else 0x0100) + struct.pack('<b', rssi)
    return struct.pack('<BBHI', 0, 0, 8 + len(body), present) + body


def hdr(fc0, a1, a2, a3, seq=0, fc1=0):
    return bytes([fc0, fc1]) + b'\0\0' + a1 + a2 + a3 + struct.pack('<H', (seq & 0xFFF) << 4)


def ie(eid, data):
    return bytes([eid, len(data)]) + data


RSN_PSK = ie(48, bytes.fromhex('0100' '000fac04' '0100' '000fac04' '0100' '000fac02' '0000'))
RSN_SAE = ie(48, bytes.fromhex('0100' '000fac04' '0100' '000fac04' '0100' '000fac08' 'c000'))
RSN_MIXED = ie(48, bytes.fromhex('0100' '000fac04' '0100' '000fac04' '0200' '000fac02000fac08' '8000'))
RSN_EAP = ie(48, bytes.fromhex('0100' '000fac04' '0100' '000fac04' '0100' '000fac01' '0000'))
SEC = {'open': b'', 'wpa2': RSN_PSK, 'wpa3': RSN_SAE, 'wpa2/3': RSN_MIXED, 'eap': RSN_EAP, 'wep': b''}


def beacon(bssid, ssid, ch, sec='wpa2', seq=0, probe_resp_to=None, hidden=False):
    capab = 0x0001 | (0x0010 if sec != 'open' else 0)
    fixed = b'\0' * 8 + struct.pack('<HH', 100, capab)
    name = (b'\0' * len(ssid) if ssid else b'') if hidden else ssid.encode()
    body = fixed + ie(0, name) + ie(1, bytes([0x82, 0x84, 0x8b, 0x96])) + ie(3, bytes([ch])) + SEC[sec]
    if probe_resp_to:
        return hdr(0x50, probe_resp_to, bssid, bssid, seq) + body
    return hdr(0x80, BCAST, bssid, bssid, seq) + body


def probe_req(src, ssid='', seq=0):
    return hdr(0x40, BCAST, src, BCAST, seq) + ie(0, ssid.encode()) + ie(1, bytes([0x82, 0x84]))


def deauth(src, dst, bssid, reason=7, seq=0, disassoc=False):
    return hdr(0xA0 if disassoc else 0xC0, dst, src, bssid, seq) + struct.pack('<H', reason)


def auth(src, dst, bssid, seq_no=1, seq=0):
    return hdr(0xB0, dst, src, bssid, seq) + struct.pack('<HHH', 0, seq_no, 0)


def assoc_req(sta, bssid, ssid, seq=0, sec='wpa2'):
    return hdr(0x00, bssid, sta, bssid, seq) + struct.pack('<HH', 0x0431, 10) + ie(0, ssid.encode()) + SEC[sec]


def eapol_key(msg, ap, sta, pmkid=False, seq=0):
    """One 4-way handshake message as a QoS data frame in the clear. The
    nonces, MIC and PMKID are fixed test patterns, not keys."""
    info = {1: 0x008A, 2: 0x010A, 3: 0x13CA, 4: 0x030A}[msg]
    kd = b''
    if pmkid:
        kd = bytes([0xDD, 20]) + bytes.fromhex('000fac04') + bytes(range(1, 17))
    key = bytes([2]) + struct.pack('>HH', info, 16) + struct.pack('>Q', msg) + bytes([0x11]) * 32 + b'\0' * 16 + b'\0' * 8 + b'\0' * 8 + \
        (b'\0' * 16 if msg == 1 else bytes([0x22]) * 16) + struct.pack('>H', len(kd)) + kd
    eap = bytes([2, 3]) + struct.pack('>H', len(key)) + key
    llc = bytes.fromhex('aaaa03000000888e')
    if msg in (1, 3):
        h = hdr(0x88, sta, ap, ap, seq, fc1=0x02)        # from DS
    else:
        h = hdr(0x88, ap, sta, ap, seq, fc1=0x01)        # to DS
    return h + b'\0\0' + llc + eap


def qos_data(src, dst, bssid, n=64, seq=0):
    return hdr(0x88, bssid, src, dst, seq, fc1=0x41) + b'\0\0' + bytes(8) + bytes(n)


class Pcapng:
    """Write records in order; time in microseconds."""

    def __init__(self, ble=False, lora=False):
        self.links = [127] + ([256] if ble else []) + ([270] if lora else [])
        self.out = bytearray()
        self.out += _block(0x0A0D0D0A, struct.pack('<IHHq', 0x1A2B3C4D, 1, 0, -1))
        for lt in self.links:
            self.out += _block(1, struct.pack('<HHI', lt, 0, 0))
        self.count = 0

    def wifi(self, t_us, frame, ch=6, rssi=-50, rate=2):
        data = radiotap(ch, rssi, rate) + frame
        body = struct.pack('<IIIII', 0, t_us >> 32, t_us & 0xFFFFFFFF, len(data), len(data)) + data
        self.out += _block(6, body)
        self.count += 1

    def ble_adv(self, t_us, addr, ad, rssi=-60, ch=37, pdu=0):
        rf = {37: 0, 38: 12, 39: 39}[ch]
        pdu_hdr = bytes([pdu | 0x40, 6 + len(ad)])
        data = bytes([rf, rssi & 0xFF, 0x80, 0]) + struct.pack('<I', 0x8E89BED6) + struct.pack('<H', 0x0013) + \
            struct.pack('<I', 0x8E89BED6) + pdu_hdr + addr + ad + b'\0\0\0'
        ifc = self.links.index(256)
        body = struct.pack('<IIIII', ifc, t_us >> 32, t_us & 0xFFFFFFFF, len(data), len(data)) + data
        self.out += _block(6, body)
        self.count += 1

    def lora(self, t_us, payload, hz=910525000, sf=7, rssi=-90, snr_db=7.5, sync=0x12, bw=1):
        """LoRaTap v0: 15 bytes, big-endian, RSSI as dBm + 139, SNR in quarter dB."""
        data = struct.pack('>BBHIBBBBBbB', 0, 0, 15, hz, bw, sf, rssi + 139, rssi + 139, rssi + 139, int(snr_db * 4), sync) + payload
        ifc = self.links.index(270)
        body = struct.pack('<IIIII', ifc, t_us >> 32, t_us & 0xFFFFFFFF, len(data), len(data)) + data
        self.out += _block(6, body)
        self.count += 1

    def bytes(self):
        return bytes(self.out)

    def save(self, path):
        with open(path, 'wb') as f:
            f.write(self.out)
        return path


def mixed(short=False):
    """Forty seconds of everything the analyser reports on: two access points,
    a station with retries, a 4-way handshake, a deauth flood, an open twin of
    a protected network, a renamed BSSID, Bluetooth advertisers and LoRa.
    short adds a frame cut off inside its header."""
    p = Pcapng(ble=True, lora=True)
    ap, ap2, twin, sta = mac('02:aa:00:00:00:01'), mac('02:aa:00:00:00:02'), mac('02:ee:00:00:00:66'), mac('02:bb:00:00:00:10')
    tag, ibeacon = mac('c6:05:04:03:02:01')[::-1], mac('d6:15:14:13:12:11')[::-1]
    t = 1_000_000
    for s in range(40):
        p.wifi(t + s * 1_000_000, beacon(ap, 'TEST-LakeNet', 6, seq=s), ch=6, rssi=-48)
        p.wifi(t + s * 1_000_000 + 50_000, beacon(ap2, 'TEST-OpenCafe', 11, sec='open', seq=s), ch=11, rssi=-70)
        for k in range(s % 7):
            f = bytearray(qos_data(sta, ap, ap, n=80 + k * 20, seq=s * 8 + k))
            if k % 3 == 2:
                f[1] |= 0x08                                  # retry
            p.wifi(t + s * 1_000_000 + 100_000 + k * 20_000, bytes(f), ch=6, rssi=-55 - k)
        p.ble_adv(t + s * 1_000_000 + 300_000, tag, bytes([2, 1, 6, 12, 9]) + b'TEST-Tag-01', rssi=-61 - s % 5, ch=37 + s % 3)
        if s % 6 == 0:
            p.ble_adv(t + s * 1_000_000 + 350_000, ibeacon, bytes([2, 1, 6, 26, 0xFF, 0x4C, 0x00, 0x02, 0x15]) + bytes(range(0xA0, 0xB0)) +
                      bytes([0, 1, 0, 2, 0xC5]), rssi=-70, ch=38, pdu=2)
        if s % 4 == 0:
            p.lora(t + s * 1_000_000 + 400_000, bytes.fromhex('ffffffff78563412efcdab8963080000deadbeef0102030405060708'),
                   hz=906875000, sf=11, sync=0x2B, rssi=-101, snr_db=-4.5, bw=2)
        if s % 5 == 0:
            p.lora(t + s * 1_000_000 + 450_000, bytes.fromhex('1100aabbccdd11223344'), rssi=-95)
    for m in (1, 2, 3, 4):
        p.wifi(t + 12_000_000 + m * 5000, eapol_key(m, ap, sta, seq=m), ch=6, rssi=-50)
    for i in range(60):
        p.wifi(t + 20_000_000 + i * 8000, deauth(ap, BCAST, ap, seq=i), ch=6, rssi=-45)
    p.wifi(t + 25_000_000, beacon(twin, 'TEST-LakeNet', 6, sec='open'), ch=6, rssi=-30)
    p.wifi(t + 26_000_000, beacon(ap, 'TEST-Renamed', 6), ch=6, rssi=-48)
    if short:
        p.wifi(t + 27_000_000, bytes.fromhex('80000000ffffffffffff02aa'), ch=6, rssi=-60)
    p.wifi(t + 28_000_000, deauth(sta, ap, ap, disassoc=True), ch=6, rssi=-52)
    return p
