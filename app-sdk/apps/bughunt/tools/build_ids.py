"""Build BUGHUNT's device-name file (ids.bin) from the public registries.

    python build_ids.py SRC_DIR OUT.bin

SRC_DIR holds the files as published:
  company_identifiers.yaml, member_uuids.yaml, service_uuids.yaml,
  appearance_values.yaml   Bluetooth SIG assigned numbers
                           (bitbucket.org/bluetooth-SIG/public, assigned_numbers/)
  oui.csv                  IEEE MA-L registry (standards-oui.ieee.org/oui/oui.csv)

The file (little endian), read by bh_ids.c with a few small card reads a name:
  header   32 B: "BHID", u16 version 1, u16 tables, u32 pool_off, u32 pool_len,
           u32 built (YYYYMMDD), 12 B zero
  tables   16 B each: u8 kind, u8 key bytes, u16 every, u32 count, u32 rec_off, u32 idx_off
  indexes  per table: the key of every `every`-th record
  records  per table, sorted by key: key (2 or 3 B), u24 offset of the name in the pool
  pool     names, NUL ended, ASCII, at most 31 characters
Kinds: 1 Bluetooth company ID, 2 16-bit service UUID, 3 MAC OUI (24 bits), 4 appearance.
"""
import csv, datetime, os, re, struct, sys, unicodedata
import yaml

EVERY = 64
KIND_COMPANY, KIND_UUID, KIND_OUI, KIND_APPEARANCE = 1, 2, 3, 4

# words that make a maker's name long without telling it apart
TAIL = re.compile(r"[,.]?\s*\b(co\.?,?\s*ltd\.?|company\s+limited|co\.?|ltd\.?|limited|inc\.?|incorporated|corp\.?|corporation|"
                  r"gmbh|ag|s\.?a\.?|s\.?r\.?l\.?|b\.?v\.?|llc|l\.?l\.?c\.?|plc|pty|oy|ab|as|a/s|kg|k\.?k\.?|sas|spa|s\.p\.a\.|"
                  r"technology|technologies|electronics|electronic)\s*\.?$", re.I)


def ascii_name(s, short=False):
    s = unicodedata.normalize("NFKD", str(s)).encode("ascii", "ignore").decode()
    s = re.sub(r"\s+", " ", s.replace('"', "").replace("“", "")).strip(" ,.")
    if short:
        for _ in range(4):
            t = TAIL.sub("", s).strip(" ,.")
            if t == s or len(t) < 3:
                break
            s = t
    return s[:31] if s else "?"


def companies(src):
    d = yaml.safe_load(open(os.path.join(src, "company_identifiers.yaml"), encoding="utf-8"))
    return {int(e["value"]): ascii_name(e["name"], True) for e in d["company_identifiers"]}


def uuids(src):
    out = {}
    for f in ("service_uuids.yaml", "member_uuids.yaml"):
        d = yaml.safe_load(open(os.path.join(src, f), encoding="utf-8"))
        for e in d["uuids"]:
            u = int(e["uuid"])
            if u <= 0xFFFF and u not in out:
                out[u] = ascii_name(e["name"], f.startswith("member"))
    return out


def appearances(src):
    d = yaml.safe_load(open(os.path.join(src, "appearance_values.yaml"), encoding="utf-8"))
    out = {}
    for c in d["appearance_values"]:
        cat = int(c["category"])
        out[cat << 6] = ascii_name(c["name"])
        for s in c.get("subcategory") or []:
            out[(cat << 6) | int(s["value"])] = ascii_name(s["name"])
    return out


def ouis(src):
    out = {}
    with open(os.path.join(src, "oui.csv"), encoding="utf-8", errors="replace", newline="") as f:
        for row in csv.reader(f):
            if len(row) < 3 or row[0] != "MA-L" or not re.fullmatch(r"[0-9A-Fa-f]{6}", row[1]):
                continue
            out[int(row[1], 16)] = ascii_name(row[2], True)
    return out


def build(src, out_path):
    tables = [(KIND_COMPANY, 2, companies(src)), (KIND_UUID, 2, uuids(src)), (KIND_OUI, 3, ouis(src)), (KIND_APPEARANCE, 2, appearances(src))]
    pool, at = bytearray(), {}

    def name_off(n):
        if n not in at:
            at[n] = len(pool)
            pool.extend(n.encode("ascii") + b"\0")
        return at[n]

    head_len = 32 + 16 * len(tables)
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
    off = head_len
    dirs = []
    idx_offs = []
    for b in idx_blobs:
        idx_offs.append(off)
        off += len(b)
    rec_offs = []
    for b in rec_blobs:
        rec_offs.append(off)
        off += len(b)
    pool_off = off
    for (kind, kb, m), io, ro in zip(tables, idx_offs, rec_offs):
        dirs.append(struct.pack("<BBHIII", kind, kb, EVERY, len(m), ro, io))
    built = int(datetime.date.today().strftime("%Y%m%d"))
    head = b"BHID" + struct.pack("<HHIII", 1, len(tables), pool_off, len(pool), built) + bytes(12)
    data = head + b"".join(dirs) + b"".join(idx_blobs) + b"".join(rec_blobs) + bytes(pool)
    open(out_path, "wb").write(data)
    for kind, kb, m in tables:
        print("kind %d: %d names" % (kind, len(m)))
    print("%s: %d bytes, pool %d" % (out_path, len(data), len(pool)))


if __name__ == "__main__":
    build(sys.argv[1], sys.argv[2])
