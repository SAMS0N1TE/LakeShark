"""Build ble-ids.bin, the Bluetooth and MAC name file every BLE app reads
(format: app-sdk/lib/lap_ids.h).

    python build_ble_ids.py SRC_DIR OUT.bin

SRC_DIR holds the sources as published (any missing one is skipped):
  company_identifiers.yaml, service_uuids.yaml, member_uuids.yaml,
  characteristic_uuids.yaml, descriptors.yaml, appearance_values.yaml
      Bluetooth SIG assigned numbers, bitbucket.org/bluetooth-SIG/public
      (assigned_numbers/)
  nordic_service_uuids.json, nordic_characteristic_uuids.json,
  nordic_descriptor_uuids.json, nordic_company_ids.json
      github.com/NordicSemiconductor/bluetooth-numbers-database (v1/)
  bleak_uuids.py
      github.com/hbldh/bleak, bleak/uuids.py (MIT)
  ws_packet-btatt.c
      github.com/wireshark/wireshark, epan/dissectors/packet-btatt.c
      (GPL-2.0-or-later): its table of vendor 128-bit UUIDs
  oui.csv
      IEEE MA-L registry, standards-oui.ieee.org/oui/oui.csv
plus ble_names_extra.txt next to this script (LakeShark's own list, in the
owner's ble-names.txt format), which wins over everything else.

Earlier sources win over later ones for the same ID: extra, SIG, Nordic,
bleak, Wireshark.
"""
import csv, datetime, json, os, re, struct, sys, unicodedata

EVERY = 64
K_COMPANY, K_SERVICE, K_OUI, K_APPEAR, K_CHAR, K_DESC, K_U128 = 1, 2, 3, 4, 5, 6, 7
SIG_BASE = bytes.fromhex("00001000800000805f9b34fb")

TAIL = re.compile(r"[,.]?\s*\b(co\.?,?\s*ltd\.?|company\s+limited|co\.?|ltd\.?|limited|inc\.?|incorporated|corp\.?|corporation|"
                  r"gmbh|ag|s\.?a\.?|s\.?r\.?l\.?|b\.?v\.?|llc|l\.?l\.?c\.?|plc|pty|oy|ab|as|a/s|kg|k\.?k\.?|sas|spa|s\.p\.a\.|"
                  r"technology|technologies|electronics|electronic)\s*\.?$", re.I)


def ascii_name(s, short=False):
    s = unicodedata.normalize("NFKD", str(s)).encode("ascii", "ignore").decode()
    s = re.sub(r"\s+", " ", s.replace('"', "")).strip(" ,.")
    if short:
        for _ in range(4):
            t = TAIL.sub("", s).strip(" ,.")
            if t == s or len(t) < 3:
                break
            s = t
    return s[:31] if s else "?"


def uuid_bytes(text):
    """16 bytes in written order, or None."""
    h = text.strip().lower()
    if h.startswith("0x"):
        h = h[2:]
    h = h.replace("-", "")
    if not re.fullmatch(r"[0-9a-f]+", h or "x"):
        return None
    if len(h) in (4, 8):
        return int(h, 16).to_bytes(4, "big") + SIG_BASE
    if len(h) == 32:
        return bytes.fromhex(h)
    return None


def uuid_short(b):
    return int.from_bytes(b[2:4], "big") if b[:2] == b"\0\0" and b[4:] == SIG_BASE else None


def fnv(b):
    h = 2166136261
    for x in b:
        h = ((h ^ x) * 16777619) & 0xFFFFFFFF
    return h


class Tables:
    def __init__(self):
        self.t = {k: {} for k in (K_COMPANY, K_SERVICE, K_OUI, K_APPEAR, K_CHAR, K_DESC, K_U128)}
        self.u128 = {}          # key -> uuid bytes, to catch hash collisions
        self.count = {}

    def put(self, kind, key, name, src):
        name = ascii_name(name, kind in (K_COMPANY, K_OUI))
        if not name or name == "?" or key in self.t[kind]:
            return
        self.t[kind][key] = name
        self.count[src] = self.count.get(src, 0) + 1

    def attr(self, kind, uuid_text, name, src):
        b = uuid_bytes(uuid_text)
        if b is None:
            return
        s = uuid_short(b)
        if s is not None:
            self.put(kind, s, name, src)
            return
        k = fnv(b)
        if k in self.u128 and self.u128[k] != b:
            raise SystemExit("hash collision: %s and %s" % (self.u128[k].hex(), b.hex()))
        self.u128[k] = b
        self.put(K_U128, k, name, src)


def short_kind(u):
    """Where a bare 16-bit UUID from a mixed list belongs."""
    if 0x2900 <= u <= 0x29FF:
        return K_DESC
    if 0x2A00 <= u <= 0x2BFF:
        return K_CHAR
    return K_SERVICE


def load_yaml(path):
    import yaml
    return yaml.safe_load(open(path, encoding="utf-8"))


def extra(t, path):
    words = {"company": K_COMPANY, "service": K_SERVICE, "uuid": K_SERVICE, "char": K_CHAR, "desc": K_DESC,
             "maker": K_OUI, "oui": K_OUI, "appearance": K_APPEAR}
    for n, line in enumerate(open(path, encoding="utf-8"), 1):
        line = re.split(r"(?:^|\s)#", line, 1)[0].strip()
        if not line:
            continue
        parts = line.split(None, 2)
        if len(parts) < 3 or parts[0] not in words:
            raise SystemExit("%s:%d: not understood" % (path, n))
        kind, key, name = words[parts[0]], parts[1], parts[2]
        if kind in (K_SERVICE, K_CHAR, K_DESC):
            if uuid_bytes(key) is None:
                raise SystemExit("%s:%d: bad UUID" % (path, n))
            t.attr(kind, key, name, "extra")
        elif kind == K_OUI:
            t.put(kind, int(key.replace(":", "").replace("-", ""), 16), name, "extra")
        else:
            t.put(kind, int(key, 0), name, "extra")


def sig(t, src):
    p = lambda f: os.path.join(src, f)
    if os.path.exists(p("company_identifiers.yaml")):
        for e in load_yaml(p("company_identifiers.yaml"))["company_identifiers"]:
            t.put(K_COMPANY, int(e["value"]), e["name"], "sig")
    for f, kind in (("service_uuids.yaml", K_SERVICE), ("member_uuids.yaml", K_SERVICE),
                    ("characteristic_uuids.yaml", K_CHAR), ("descriptors.yaml", K_DESC)):
        if os.path.exists(p(f)):
            for e in load_yaml(p(f))["uuids"]:
                name = ascii_name(e["name"], f.startswith("member"))
                t.put(kind, int(e["uuid"]), name, "sig")
    if os.path.exists(p("appearance_values.yaml")):
        for c in load_yaml(p("appearance_values.yaml"))["appearance_values"]:
            cat = int(c["category"])
            t.put(K_APPEAR, cat << 6, c["name"], "sig")
            for s in c.get("subcategory") or []:
                t.put(K_APPEAR, (cat << 6) | int(s["value"]), s["name"], "sig")


def nordic(t, src):
    for f, kind in (("nordic_service_uuids.json", K_SERVICE), ("nordic_characteristic_uuids.json", K_CHAR),
                    ("nordic_descriptor_uuids.json", K_DESC)):
        path = os.path.join(src, f)
        if os.path.exists(path):
            for e in json.load(open(path, encoding="utf-8")):
                t.attr(kind, e["uuid"], e["name"], "nordic")
    path = os.path.join(src, "nordic_company_ids.json")
    if os.path.exists(path):
        for e in json.load(open(path, encoding="utf-8")):
            t.put(K_COMPANY, int(e["code"]), e["name"], "nordic")


def bleak(t, src):
    path = os.path.join(src, "bleak_uuids.py")
    if not os.path.exists(path):
        return
    text = open(path, encoding="utf-8").read()
    for m in re.finditer(r"^\s*0x([0-9A-Fa-f]{4})\s*:\s*\"([^\"]+)\"", text, re.M):
        u = int(m.group(1), 16)
        t.put(short_kind(u), u, m.group(2), "bleak")
    for m in re.finditer(r"^\s*\"([0-9a-fA-F]{8}-[0-9a-fA-F-]{27})\"\s*:\s*\"([^\"]+)\"", text, re.M):
        b = uuid_bytes(m.group(1))
        s = uuid_short(b) if b else None
        if s is not None:
            t.put(short_kind(s), s, m.group(2), "bleak")
        else:
            t.attr(K_SERVICE, m.group(1), m.group(2), "bleak")


def wireshark(t, src):
    path = os.path.join(src, "ws_packet-btatt.c")
    if not os.path.exists(path):
        return
    for m in re.finditer(r"\{\s*\"([0-9a-fA-F]{8}-[0-9a-fA-F-]{27})\"\s*,\s*\"([^\"]+)\"", open(path, encoding="utf-8").read()):
        t.attr(K_SERVICE, m.group(1), m.group(2), "wireshark")


def ouis(t, src):
    path = os.path.join(src, "oui.csv")
    if not os.path.exists(path):
        return
    with open(path, encoding="utf-8", errors="replace", newline="") as f:
        for row in csv.reader(f):
            if len(row) >= 3 and row[0] == "MA-L" and re.fullmatch(r"[0-9A-Fa-f]{6}", row[1]):
                t.put(K_OUI, int(row[1], 16), row[2], "ieee")


def write(t, out_path):
    keyb = {K_COMPANY: 2, K_SERVICE: 2, K_OUI: 3, K_APPEAR: 2, K_CHAR: 2, K_DESC: 2, K_U128: 4}
    tables = [(k, keyb[k], m) for k, m in t.t.items() if m]
    pool, at = bytearray(), {}

    def name_off(n):
        if n not in at:
            at[n] = len(pool)
            pool.extend(n.encode("ascii") + b"\0")
        return at[n]

    idx_blobs, rec_blobs = [], []
    for kind, kb, m in tables:
        keys = sorted(m)
        recs = bytearray()
        for k in keys:
            recs += k.to_bytes(kb, "little") + name_off(m[k]).to_bytes(3, "little")
        idx = bytearray()
        for i in range(0, len(keys), EVERY):
            idx += keys[i].to_bytes(kb, "little")
        idx_blobs.append(idx)
        rec_blobs.append(recs)
    assert len(pool) < 1 << 24
    off = 32 + 16 * len(tables)
    idx_offs, rec_offs = [], []
    for b in idx_blobs:
        idx_offs.append(off)
        off += len(b)
    for b in rec_blobs:
        rec_offs.append(off)
        off += len(b)
    pool_off = off
    dirs = [struct.pack("<BBHIII", kind, kb, EVERY, len(m), ro, io) for (kind, kb, m), io, ro in zip(tables, idx_offs, rec_offs)]
    built = int(datetime.date.today().strftime("%Y%m%d"))
    head = b"BHID" + struct.pack("<HHIII", 1, len(tables), pool_off, len(pool), built) + bytes(12)
    data = head + b"".join(dirs) + b"".join(idx_blobs) + b"".join(rec_blobs) + bytes(pool)
    open(out_path, "wb").write(data)
    names = {K_COMPANY: "companies", K_SERVICE: "services", K_OUI: "MAC makers", K_APPEAR: "appearances",
             K_CHAR: "characteristics", K_DESC: "descriptors", K_U128: "vendor 128-bit UUIDs"}
    for kind, kb, m in tables:
        print("%-22s %6d" % (names[kind], len(m)))
    print("by source: " + ", ".join("%s %d" % kv for kv in sorted(t.count.items())))
    print("%s: %d bytes, index %d bytes" % (out_path, len(data), sum(len(b) for b in idx_blobs)))


def build(src, out_path):
    t = Tables()
    extra(t, os.path.join(os.path.dirname(os.path.abspath(__file__)), "ble_names_extra.txt"))
    sig(t, src)
    nordic(t, src)
    bleak(t, src)
    wireshark(t, src)
    ouis(t, src)
    write(t, out_path)


if __name__ == "__main__":
    build(sys.argv[1], sys.argv[2])
