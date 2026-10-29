"""A small zstd frame writer in pure Python (RFC 8878), for build-time text packs.

The build machines run ESP-IDF's Python, which has no zstd module, so the
firmware's text packs are compressed here. Only what a decoder must accept
is written:

  - one frame, Single_Segment (window = content size, at most 64 KiB, which
    is CartoCore's CZ7 limit), content checksum (XXH64) on;
  - one block per frame: compressed when that is smaller, raw otherwise;
  - literals Huffman-coded (weights direct or FSE-compressed), raw when that
    is smaller or there are fewer than two distinct bytes;
  - sequences from a greedy/lazy LZ77 parse, explicit offsets only (no
    repeat codes), all three code tables in Predefined_Mode.

Correctness is checked by decoding with libzstd (bench test test_ls_text,
and decode() below when Python 3.14's compression.zstd is present).
"""

import heapq
import struct

MAGIC = 0xFD2FB528
MAX_FRAME = 65536          # CZ7: windowSize <= 64 KiB; single segment window = content
MIN_MATCH = 4

# ---------------------------------------------------------------- XXH64 ----

_P1 = 0x9E3779B185EBCA87
_P2 = 0xC2B2AE3D27D4EB4F
_P3 = 0x165667B19E3779F9
_P4 = 0x85EBCA77C2B2AE63
_P5 = 0x27D4EB2F165667C5
_M64 = (1 << 64) - 1


def _rotl(x, r):
    return ((x << r) | (x >> (64 - r))) & _M64


def _round(acc, lane):
    acc = (acc + lane * _P2) & _M64
    return (_rotl(acc, 31) * _P1) & _M64


def _merge(acc, val):
    acc ^= _round(0, val)
    return (acc * _P1 + _P4) & _M64


def xxh64(data, seed=0):
    n = len(data)
    i = 0
    if n >= 32:
        v1 = (seed + _P1 + _P2) & _M64
        v2 = (seed + _P2) & _M64
        v3 = seed & _M64
        v4 = (seed - _P1) & _M64
        while i + 32 <= n:
            a, b, c, d = struct.unpack_from('<4Q', data, i)
            v1, v2, v3, v4 = _round(v1, a), _round(v2, b), _round(v3, c), _round(v4, d)
            i += 32
        h = (_rotl(v1, 1) + _rotl(v2, 7) + _rotl(v3, 12) + _rotl(v4, 18)) & _M64
        for v in (v1, v2, v3, v4):
            h = _merge(h, v)
    else:
        h = (seed + _P5) & _M64
    h = (h + n) & _M64
    while i + 8 <= n:
        k, = struct.unpack_from('<Q', data, i)
        h ^= _round(0, k)
        h = (_rotl(h, 27) * _P1 + _P4) & _M64
        i += 8
    if i + 4 <= n:
        k, = struct.unpack_from('<I', data, i)
        h ^= (k * _P1) & _M64
        h = (_rotl(h, 23) * _P2 + _P3) & _M64
        i += 4
    while i < n:
        h ^= (data[i] * _P5) & _M64
        h = (_rotl(h, 11) * _P1) & _M64
        i += 1
    h ^= h >> 33
    h = (h * _P2) & _M64
    h ^= h >> 29
    h = (h * _P3) & _M64
    h ^= h >> 32
    return h


# ------------------------------------------------------------ bitstreams ---

class BackwardBits:
    """zstd's BIT_CStream: bits appended low to high, read back from the end."""

    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def add(self, value, nbits):
        if nbits == 0:
            return
        assert 0 <= value < (1 << nbits), (value, nbits)
        self.acc |= value << self.n
        self.n += nbits
        while self.n >= 8:
            self.out.append(self.acc & 0xFF)
            self.acc >>= 8
            self.n -= 8

    def close(self):
        self.add(1, 1)          # the end mark the decoder looks for
        if self.n:
            self.out.append(self.acc & 0xFF)
            self.acc = 0
            self.n = 0
        return bytes(self.out)


class ForwardBits:
    """Little-endian forward bitstream (FSE table descriptions)."""

    def __init__(self):
        self.acc = 0
        self.n = 0

    def add(self, value, nbits):
        assert 0 <= value < (1 << nbits) or nbits == 0
        self.acc |= value << self.n
        self.n += nbits

    def bytes(self):
        return self.acc.to_bytes((self.n + 7) // 8, 'little')


def highbit(v):
    return v.bit_length() - 1


# ------------------------------------------------------------------- FSE ---

class FseTable:
    """Decoding cells for a normalized distribution, plus the inverse map an
    encoder needs: for a symbol and the state the decoder must reach next,
    the cell to sit in and the bits that lead there."""

    def __init__(self, norm, table_log):
        size = 1 << table_log
        self.log = table_log
        sym = [0] * size
        high = size - 1
        nxt = {}
        for s, c in enumerate(norm):
            if c == -1:
                sym[high] = s
                high -= 1
                nxt[s] = 1
            elif c > 0:
                nxt[s] = c
        step = (size >> 1) + (size >> 3) + 3
        pos = 0
        for s, c in enumerate(norm):
            for _ in range(max(c, 0)):
                sym[pos] = s
                pos = (pos + step) & (size - 1)
                while pos > high:
                    pos = (pos + step) & (size - 1)
        assert pos == 0, 'spread did not return to 0'
        self.cells = []
        self.enc = {}
        for u in range(size):
            s = sym[u]
            ns = nxt[s]
            nxt[s] += 1
            nb = table_log - highbit(ns)
            base = (ns << nb) - size
            self.cells.append((s, nb, base))
            m = self.enc.setdefault(s, {})
            for r in range(1 << nb):
                m[base + r] = (u, r, nb)
        for s, m in self.enc.items():
            assert len(m) == size, 'cells of %d do not tile the table' % s

    def any_state(self, s, need_bits=False):
        for u, (cs, nb, _) in enumerate(self.cells):
            if cs == s and (nb or not need_bits):
                return u
        raise ValueError('symbol %d not in table' % s)


# Predefined distributions (RFC 8878 3.1.1.3.2.2)
LL_NORM = [4, 3, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2,
           2, 3, 2, 1, 1, 1, 1, 1, -1, -1, -1, -1]
ML_NORM = [1, 4, 3, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
           1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, -1, -1,
           -1, -1, -1, -1, -1]
OF_NORM = [1, 1, 1, 1, 1, 1, 2, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
           -1, -1, -1, -1, -1]
LL_TABLE = FseTable(LL_NORM, 6)
ML_TABLE = FseTable(ML_NORM, 6)
OF_TABLE = FseTable(OF_NORM, 5)

LL_BITS = [0] * 16 + [1, 1, 1, 1, 2, 2, 3, 3, 4, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]
LL_BASE = list(range(16)) + [16, 18, 20, 22, 24, 28, 32, 40, 48, 64, 0x80, 0x100, 0x200,
                             0x400, 0x800, 0x1000, 0x2000, 0x4000, 0x8000, 0x10000]
ML_BITS = [0] * 32 + [1, 1, 1, 1, 2, 2, 3, 3, 4, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16]
ML_BASE = list(range(3, 35)) + [35, 37, 39, 41, 43, 47, 51, 59, 67, 83, 99, 0x83, 0x103,
                                0x203, 0x403, 0x803, 0x1003, 0x2003, 0x4003, 0x8003, 0x10003]
assert len(LL_BITS) == len(LL_BASE) == len(LL_NORM)
assert len(ML_BITS) == len(ML_BASE) == len(ML_NORM)


def _code(value, bases):
    c = 0
    for i, b in enumerate(bases):
        if b <= value:
            c = i
        else:
            break
    return c


def ll_code(n):
    return n if n < 16 else _code(n, LL_BASE)


def ml_code(m):
    return m - 3 if m - 3 < 32 else _code(m, ML_BASE)


def write_ncount(norm, table_log):
    """FSE table description (RFC 8878 4.1.1), as FSE_writeNCount writes it."""
    bw = ForwardBits()
    bw.add(table_log - 5, 4)
    remaining = (1 << table_log) + 1
    threshold = 1 << table_log
    nbits = table_log + 1
    s = 0
    prev0 = False
    last = max(i for i, c in enumerate(norm) if c != 0)
    while s <= last and remaining > 1:
        if prev0:
            start = s
            while norm[s] == 0:
                s += 1
            while s >= start + 24:
                start += 24
                bw.add(0xFFFF, 16)
            while s >= start + 3:
                start += 3
                bw.add(3, 2)
            bw.add(s - start, 2)
        count = norm[s]
        s += 1
        mx = (2 * threshold - 1) - remaining
        remaining -= -count if count < 0 else count
        count += 1
        if count >= threshold:
            count += mx
        bw.add(count, nbits - (1 if count < mx else 0))
        prev0 = (count == 1)
        assert remaining >= 1
        while remaining < threshold:
            nbits -= 1
            threshold >>= 1
    assert remaining == 1, 'distribution does not sum to the table'
    return bw.bytes()


def normalize(counts, table_log):
    """Counts -> normalized counts summing to 1 << table_log, every present
    symbol at least 1 (no -1 cells: simpler, and these tables are tiny)."""
    total = sum(counts)
    size = 1 << table_log
    norm = [0] * len(counts)
    for s, c in enumerate(counts):
        if c:
            norm[s] = max(1, (c * size) // total)
    diff = size - sum(norm)
    order = sorted((s for s, c in enumerate(counts) if c), key=lambda s: -counts[s])
    i = 0
    while diff > 0:
        norm[order[i % len(order)]] += 1
        diff -= 1
        i += 1
    while diff < 0:
        for s in order:
            if diff == 0:
                break
            if norm[s] > 1:
                norm[s] -= 1
                diff += 1
        else:
            if diff < 0:
                raise ValueError('cannot normalize')
    return norm


# --------------------------------------------------------------- Huffman ---

def huff_lengths(freq, maxbits=11):
    """Code length per symbol (dict), every length <= maxbits."""
    f = dict(freq)
    while True:
        heap = [(c, s, None) for s, c in f.items()]
        heapq.heapify(heap)
        parent = {}
        nid = 1 << 20
        while len(heap) > 1:
            a = heapq.heappop(heap)
            b = heapq.heappop(heap)
            parent[('n', a[1]) if a[2] else ('s', a[1])] = nid
            parent[('n', b[1]) if b[2] else ('s', b[1])] = nid
            heapq.heappush(heap, (a[0] + b[0], nid, True))
            nid += 1
        lens = {}
        for s in f:
            d = 0
            k = ('s', s)
            while k in parent:
                k = ('n', parent[k])
                d += 1
            lens[s] = d
        if max(lens.values()) <= maxbits:
            return lens
        f = {s: (c >> 1) | 1 for s, c in f.items()}


def huff_codes(lens):
    """Canonical codes the zstd way: longest codes first, symbols ascending,
    numerically smallest code to the longest length."""
    maxbits = max(lens.values())
    codes = {}
    code = 0
    for nb in range(maxbits, 0, -1):
        for s in sorted(s for s, l in lens.items() if l == nb):
            codes[s] = (code, nb)
            code += 1
        code >>= 1
    return codes


def huff_stream(data, codes):
    bw = BackwardBits()
    for b in reversed(data):
        c, nb = codes[b]
        bw.add(c, nb)
    return bw.close()


def fse_weights(weights):
    """FSE-compressed Huffman weights, or None if they will not fit."""
    n = len(weights)
    hist = [0] * (max(weights) + 1)
    for w in weights:
        hist[w] += 1
    present = [s for s, c in enumerate(hist) if c]
    if len(present) < 2:
        return None
    log = 6
    norm = normalize(hist, log)
    t = FseTable(norm, log)
    head = write_ncount(norm, log)
    # Decode order: x0 from state 1, x1 from state 2, alternating; the read
    # after x[n-2] must run past the end (>= 1 bit) for the decoder to stop.
    st = [None] * n
    st[n - 1] = t.any_state(weights[n - 1])
    st[n - 2] = t.any_state(weights[n - 2], need_bits=True)
    upd = [None] * n
    for k in range(n - 3, -1, -1):
        u, r, nb = t.enc[weights[k]][st[k + 2]]
        st[k] = u
        upd[k] = (r, nb)
    bw = BackwardBits()
    for k in range(n - 3, -1, -1):
        bw.add(*upd[k])
    bw.add(st[1], log)
    bw.add(st[0], log)
    out = head + bw.close()
    return out if len(out) < 128 else None


def huff_tree(lens):
    """Tree description: header byte + weights; None if not encodable."""
    maxbits = max(lens.values())
    last = max(lens)
    weights = [0] * last
    for s in range(last):
        if s in lens:
            weights[s] = maxbits + 1 - lens[s]
    if last <= 128:
        out = bytearray([127 + last])
        for i in range(0, last, 2):
            hi = weights[i]
            lo = weights[i + 1] if i + 1 < last else 0
            out.append((hi << 4) | lo)
        direct = bytes(out)
    else:
        direct = None
    f = fse_weights(weights) if last >= 2 else None
    fse = bytes([len(f)]) + f if f is not None else None
    cands = [c for c in (direct, fse) if c is not None]
    return min(cands, key=len) if cands else None


def raw_literals(lits):
    n = len(lits)
    if n < 32:
        return bytes([n << 3]) + lits
    if n < 4096:
        return struct.pack('<H', (1 << 2) | (n << 4)) + lits
    v = (3 << 2) | (n << 4)
    return struct.pack('<I', v)[:3] + lits


def literals_section(lits):
    raw = raw_literals(lits)
    n = len(lits)
    if n < 64:
        return raw
    freq = {}
    for b in lits:
        freq[b] = freq.get(b, 0) + 1
    if len(freq) < 2:
        return raw
    lens = huff_lengths(freq)
    tree = huff_tree(lens)
    if tree is None:
        return raw
    codes = huff_codes(lens)
    if n <= 1023:
        body = tree + huff_stream(lits, codes)
        csize = len(body)
        if csize > 1023:
            return raw
        hdr = 2 | (0 << 2) | (n << 4) | (csize << 14)
        out = struct.pack('<I', hdr)[:3] + body
    else:
        seg = (n + 3) // 4
        parts = [lits[0:seg], lits[seg:2 * seg], lits[2 * seg:3 * seg], lits[3 * seg:]]
        streams = [huff_stream(p, codes) for p in parts]
        if any(len(s) > 0xFFFF for s in streams[:3]):
            return raw
        body = tree + struct.pack('<3H', *(len(s) for s in streams[:3])) + b''.join(streams)
        csize = len(body)
        if n < (1 << 14) and csize < (1 << 14):
            hdr = 2 | (2 << 2) | (n << 4) | (csize << 18)
            out = struct.pack('<I', hdr) + body
        elif n < (1 << 18) and csize < (1 << 18):
            hdr = 2 | (3 << 2) | (n << 4) | (csize << 22)
            out = struct.pack('<Q', hdr)[:5] + body
        else:
            return raw
    return out if len(out) < len(raw) else raw


# ----------------------------------------------------------------- LZ77 ----

def parse(data, chain=96):
    """Greedy LZ77 with one step of lazy evaluation. Returns (literals,
    [(lit_len, offset, match_len)], trailing literal count)."""
    n = len(data)
    head = {}
    prev = [-1] * n
    seqs = []
    lits = bytearray()
    anchor = 0
    ins = [0]   # every position below this is in the hash chains

    def insert_upto(p):
        k = ins[0]
        while k < p:
            if k + MIN_MATCH <= n:
                key = data[k:k + MIN_MATCH]
                prev[k] = head.get(key, -1)
                head[key] = k
            k += 1
        ins[0] = max(ins[0], p)

    def best(i):
        """Longest earlier match for position i (positions < i only)."""
        if i + MIN_MATCH > n:
            return 0, 0
        insert_upto(i)
        j = head.get(data[i:i + MIN_MATCH], -1)
        bl, bo = 0, 0
        tries = chain
        while j >= 0 and tries:
            if i - j > MAX_FRAME:
                break
            l = 0
            while i + l < n and data[j + l] == data[i + l]:
                l += 1
            if l > bl:
                bl, bo = l, i - j
            j = prev[j]
            tries -= 1
        return bl, bo

    def rep_len(i, d):
        if d <= 0 or d > i:
            return 0
        l = 0
        while i + l < n and data[i - d + l] == data[i + l]:
            l += 1
        return l

    rep = [1, 4, 8]                 # the decoder's repeat offsets, kept in step
    i = 0
    while i < n:
        # a repeat offset costs a few bits where a new one costs ~log2(d) + 5:
        # take it when it is about as long
        rl, rd = 0, 0
        for d in rep:
            l = rep_len(i, d)
            if l > rl:
                rl, rd = l, d
        ml, off = best(i)
        if rl >= 3 and rl + 1 >= ml:
            ml, off = rl, rd
        elif ml < MIN_MATCH:
            i += 1
            continue
        else:
            ml2, off2 = best(i + 1)
            if ml2 > ml:
                i += 1
                ml, off = ml2, off2
        lits += data[anchor:i]
        seqs.append((i - anchor, off, ml))
        rep = rep_after(rep, i - anchor, off)[1]
        i += ml
        anchor = i
    lits += data[anchor:]
    return bytes(lits), seqs


def rep_after(rep, ll, off):
    """(Offset_Value to send for `off`, the repeat offsets after it), by the
    decoder's rules (RFC 8878 3.1.2.5); a repeat is used whenever one fits."""
    r0, r1, r2 = rep
    if ll > 0:
        if off == r0:
            return 1, [r0, r1, r2]
        if off == r1:
            return 2, [r1, r0, r2]
        if off == r2:
            return 3, [r2, r0, r1]
    else:
        if off == r1:
            return 1, [r1, r0, r2]
        if off == r2:
            return 2, [r2, r0, r1]
        if off == r0 - 1 and off > 0:
            return 3, [off, r0, r1]
    return off + 3, [off, r0, r1]


def sequences_section(seqs):
    nb = len(seqs)
    if nb < 128:
        out = bytearray([nb])
    elif nb < 0x7F00:
        out = bytearray([(nb >> 8) + 128, nb & 0xFF])
    else:
        out = bytearray([255]) + struct.pack('<H', nb - 0x7F00)
    if nb == 0:
        return bytes(out)
    codes = []
    rep = [1, 4, 8]
    for ll, off, ml in seqs:
        lc, mc = ll_code(ll), ml_code(ml)
        ov, rep = rep_after(rep, ll, off)
        oc = highbit(ov)
        codes.append((lc, ll - LL_BASE[lc], LL_BITS[lc],
                      mc, ml - ML_BASE[mc], ML_BITS[mc],
                      oc, ov - (1 << oc), oc))
    # per stream: the predefined table, or one built from these counts when
    # that is smaller with its description (FSE_Compressed_Mode)
    LL_TBL, LL_MODE, LL_HEAD = pick_table([c[0] for c in codes], LL_TABLE, 9)
    OF_TBL, OF_MODE, OF_HEAD = pick_table([c[6] for c in codes], OF_TABLE, 8)
    ML_TBL, ML_MODE, ML_HEAD = pick_table([c[3] for c in codes], ML_TABLE, 9)
    out.append((LL_MODE << 6) | (OF_MODE << 4) | (ML_MODE << 2))
    out += LL_HEAD + OF_HEAD + ML_HEAD
    ll_st = [0] * nb
    ml_st = [0] * nb
    of_st = [0] * nb
    ll_up = [None] * nb
    ml_up = [None] * nb
    of_up = [None] * nb
    last = codes[-1]
    ll_st[-1] = LL_TBL.any_state(last[0])
    ml_st[-1] = ML_TBL.any_state(last[3])
    of_st[-1] = OF_TBL.any_state(last[6])
    for i in range(nb - 2, -1, -1):
        c = codes[i]
        u, r, b = LL_TBL.enc[c[0]][ll_st[i + 1]]
        ll_st[i], ll_up[i] = u, (r, b)
        u, r, b = ML_TBL.enc[c[3]][ml_st[i + 1]]
        ml_st[i], ml_up[i] = u, (r, b)
        u, r, b = OF_TBL.enc[c[6]][of_st[i + 1]]
        of_st[i], of_up[i] = u, (r, b)
    bw = BackwardBits()
    for i in range(nb - 1, -1, -1):
        c = codes[i]
        if i < nb - 1:
            bw.add(*of_up[i])
            bw.add(*ml_up[i])
            bw.add(*ll_up[i])
        bw.add(c[1], c[2])     # LL extra
        bw.add(c[4], c[5])     # ML extra
        bw.add(c[7], c[8])     # OF extra
    bw.add(ml_st[0], ML_TBL.log)
    bw.add(of_st[0], OF_TBL.log)
    bw.add(ll_st[0], LL_TBL.log)
    return bytes(out) + bw.close()


def _cost_bits(table, counts):
    """Bits to code these symbol counts with a table: log2(size / cells)."""
    import math
    size = 1 << table.log
    cells = {}
    for s, _, _ in table.cells:
        cells[s] = cells.get(s, 0) + 1
    return sum(c * (table.log - math.log2(cells[s])) for s, c in counts.items())


def pick_table(syms, predefined, max_log):
    """(table, mode, description bytes) for one stream of codes."""
    counts = {}
    for s in syms:
        counts[s] = counts.get(s, 0) + 1
    best = (predefined, 0, b'')
    best_cost = _cost_bits(predefined, counts)
    if len(counts) < 2:
        return best
    hist = [0] * (max(counts) + 1)
    for s, c in counts.items():
        hist[s] = c
    for log in range(5, max_log + 1):
        if (1 << log) < len(counts):
            continue
        try:
            norm = normalize(hist, log)
            table = FseTable(norm, log)
            head = write_ncount(norm, log)
        except (ValueError, AssertionError):
            continue
        cost = _cost_bits(table, counts) + 8 * len(head)
        if cost < best_cost:
            best, best_cost = (table, 2, head), cost
    return best


# ---------------------------------------------------------------- frames ---

def compress_frame(data, checksum=True):
    """One zstd frame holding `data` (at most 64 KiB)."""
    n = len(data)
    assert 0 < n <= MAX_FRAME
    desc = 0x20 | (0x04 if checksum else 0)     # Single_Segment, checksum
    if n < 256:
        fcs = bytes([n])
    elif n < 65536 + 256:
        desc |= 0x40
        fcs = struct.pack('<H', n - 256)
    else:
        desc |= 0x80
        fcs = struct.pack('<I', n)
    out = bytearray(struct.pack('<I', MAGIC)) + bytes([desc]) + fcs
    lits, seqs = parse(data)
    body = literals_section(lits) + sequences_section(seqs)
    if len(body) < n:
        bh = 1 | (2 << 1) | (len(body) << 3)
        out += struct.pack('<I', bh)[:3] + body
    else:
        bh = 1 | (0 << 1) | (n << 3)
        out += struct.pack('<I', bh)[:3] + data
    if checksum:
        out += struct.pack('<I', xxh64(data) & 0xFFFFFFFF)
    return bytes(out)


def cz7(data):
    """CartoCore's container: "CZ7\\0", decoded size, one checksummed frame."""
    assert 32 <= len(data) <= MAX_FRAME
    return b'CZ7\0' + struct.pack('<I', len(data)) + compress_frame(data)


def decode(frame):
    """libzstd's answer, when this Python has it (3.14+); None otherwise."""
    try:
        from compression import zstd
    except ImportError:
        return None
    return zstd.decompress(frame)
