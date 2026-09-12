"""Cut a region out of a remote PMTiles archive, in the shape this board reads.

LS-1047  The board's reader is components/pmtiles/upstream/zeromesh_pmtiles.c,
written for a Flipper Zero, and it is strict for reasons that are about the
hardware rather than the format:

    internal_compression and tile_compression must both be NONE
        There is no zlib on that device. Protomaps ships everything gzipped.

    root directory at most 48 KB
        It is read whole and held for the life of the archive.

    leaf directories at most 4 KB and 256 entries
        Only one leaf is resident at a time, which is what stops the memory
        cost scaling with the tile count. A statewide archive opens in the
        same heap as a city one.

So this is not a download - it is a repack. Fetch the remote header and walk
its directories over HTTP range requests, pick the tiles inside a bounding
box, pull those byte ranges, gunzip each one, and write a fresh archive with
uncompressed directories sized to the reader's limits.

Usage:

    python pmtiles_extract.py --url https://build.protomaps.com/20260909.pmtiles \\
        --bbox -72.6 42.7 -70.6 44.4 --maxzoom 13 --out H:/maps/nh.pmtiles

The bbox is min_lon min_lat max_lon max_lat, in degrees.
"""

import argparse
import gzip
import io
import os
import struct
import sys
import time
import urllib.request

HEADER_LEN = 127

COMPRESSION_NONE = 1
COMPRESSION_GZIP = 2
TILETYPE_MVT = 1

# The board's limits, named here so the reason travels with the numbers.
MAX_ROOT_BYTES = 48 * 1024
MAX_LEAF_BYTES = 4 * 1024
MAX_LEAF_ENTRIES = 256


# ---------------------------------------------------------------- varints --

def read_varint(buf, pos):
    shift = 0
    result = 0
    while True:
        b = buf[pos]
        pos += 1
        result |= (b & 0x7F) << shift
        if not (b & 0x80):
            return result, pos
        shift += 7


def write_varint(out, value):
    while True:
        b = value & 0x7F
        value >>= 7
        if value:
            out.append(b | 0x80)
        else:
            out.append(b)
            return


# ------------------------------------------------------------- tile ids ----

def zxy_to_tileid(z, x, y):
    """The Hilbert curve index PMTiles orders tiles by.

    Straight from the specification. The base is the count of every tile at
    every zoom below this one, and the offset within the level is the Hilbert
    distance - which is what makes tiles that are near each other on the
    ground near each other in the file, and is the whole reason a range
    request for one screenful is a handful of seeks rather than hundreds.
    """
    acc = 0
    for t in range(z):
        acc += (1 << t) * (1 << t)
    n = 1 << z
    rx = ry = 0
    d = 0
    tx, ty = x, y
    s = n // 2
    while s > 0:
        rx = 1 if (tx & s) > 0 else 0
        ry = 1 if (ty & s) > 0 else 0
        d += s * s * ((3 * rx) ^ ry)
        # rotate
        if ry == 0:
            if rx == 1:
                tx = s - 1 - tx
                ty = s - 1 - ty
            tx, ty = ty, tx
        s //= 2
    return acc + d


def lonlat_to_tile(lon, lat, z):
    import math
    n = 1 << z
    x = int((lon + 180.0) / 360.0 * n)
    lat = max(min(lat, 85.05112878), -85.05112878)
    rad = math.radians(lat)
    y = int((1.0 - math.log(math.tan(rad) + 1.0 / math.cos(rad)) / math.pi) / 2.0 * n)
    return max(0, min(n - 1, x)), max(0, min(n - 1, y))


# ------------------------------------------------------------- the remote --

class Remote:
    """Byte ranges over HTTP, with the directory pages remembered.

    Directories are read many times while a bbox is walked - the root on
    every lookup, a leaf once per tile under it - and each one is a request
    with a round trip in it. Caching them turns a walk of ten thousand tiles
    from ten thousand requests into a few hundred.
    """

    def __init__(self, url):
        self.url = url
        self.cache = {}
        self.bytes_fetched = 0
        self.requests = 0

    def get(self, offset, length, cache=False):
        if length <= 0:
            return b""
        key = (offset, length)
        if cache and key in self.cache:
            return self.cache[key]
        # A User-Agent, because the CDN in front of these archives answers
        # 403 to the default Python one. Naming the tool is also the polite
        # thing when pulling ranges out of somebody else's 137 GB object.
        req = urllib.request.Request(
            self.url,
            headers={
                "Range": "bytes=%d-%d" % (offset, offset + length - 1),
                "User-Agent": "lakeshark-pmtiles-extract/1.0",
            })
        for attempt in range(4):
            try:
                with urllib.request.urlopen(req, timeout=60) as r:
                    data = r.read()
                break
            except Exception as e:                       # noqa: BLE001
                if attempt == 3:
                    raise
                # A 137 GB object behind a CDN drops a connection now and
                # then; a retry is cheaper than losing the whole walk.
                print("  retry %d after %s" % (attempt + 1, e), file=sys.stderr)
                time.sleep(1.5 * (attempt + 1))
        self.requests += 1
        self.bytes_fetched += len(data)
        if cache:
            self.cache[key] = data
        return data


# ---------------------------------------------------------------- header ---

class Header:
    def __init__(self, raw):
        if raw[:7] != b"PMTiles":
            raise SystemExit("not a PMTiles archive")
        if raw[7] != 3:
            raise SystemExit("only PMTiles v3 is supported, this is v%d" % raw[7])
        (self.root_offset, self.root_length,
         self.metadata_offset, self.metadata_length,
         self.leaf_offset, self.leaf_length,
         self.data_offset, self.data_length,
         self.addressed, self.entries, self.contents) = struct.unpack_from(
            "<11Q", raw, 8)
        self.clustered = raw[96]
        self.internal_compression = raw[97]
        self.tile_compression = raw[98]
        self.tile_type = raw[99]
        self.min_zoom = raw[100]
        self.max_zoom = raw[101]


def decompress(data, compression):
    if compression == COMPRESSION_NONE:
        return data
    if compression == COMPRESSION_GZIP:
        return gzip.decompress(data)
    raise SystemExit("compression %d is not one this tool handles" % compression)


def parse_directory(buf):
    """-> list of (tile_id, run_length, offset, length), in file order."""
    pos = 0
    n, pos = read_varint(buf, pos)

    ids = [0] * n
    last = 0
    for i in range(n):
        delta, pos = read_varint(buf, pos)
        last += delta
        ids[i] = last

    runs = [0] * n
    for i in range(n):
        runs[i], pos = read_varint(buf, pos)

    lengths = [0] * n
    for i in range(n):
        lengths[i], pos = read_varint(buf, pos)

    offsets = [0] * n
    for i in range(n):
        v, pos = read_varint(buf, pos)
        if v == 0 and i > 0:
            offsets[i] = offsets[i - 1] + lengths[i - 1]
        else:
            offsets[i] = v - 1
    return list(zip(ids, runs, offsets, lengths))


def find_entry(entries, want):
    """The entry covering `want`, or None. Entries are sorted by tile id."""
    lo, hi = 0, len(entries) - 1
    hit = None
    while lo <= hi:
        mid = (lo + hi) // 2
        if entries[mid][0] <= want:
            hit = entries[mid]
            lo = mid + 1
        else:
            hi = mid - 1
    if hit is None:
        return None
    tid, run, off, length = hit
    if run == 0:
        return hit                      # a leaf pointer; the caller recurses
    if want >= tid + run:
        return None
    return hit


# ------------------------------------------------------------- the writer --

def serialise_directory(entries):
    """entries: list of (tile_id, run_length, offset, length) sorted by id."""
    out = bytearray()
    write_varint(out, len(entries))

    last = 0
    for e in entries:
        write_varint(out, e[0] - last)
        last = e[0]
    for e in entries:
        write_varint(out, e[1])
    for e in entries:
        write_varint(out, e[3])
    for i, e in enumerate(entries):
        if i > 0 and e[2] == entries[i - 1][2] + entries[i - 1][3]:
            write_varint(out, 0)        # contiguous with the one before
        else:
            write_varint(out, e[2] + 1)
    return bytes(out)


def build_leaves(entries):
    """Split entries into leaf directories inside the reader's two limits.

    Both limits bite: 256 entries is the scratch array it allocates, and 4 KB
    is the buffer it reads a leaf into. A leaf that breaks either is refused
    at open with a message naming this tool.
    """
    leaves = []
    i = 0
    while i < len(entries):
        take = min(MAX_LEAF_ENTRIES, len(entries) - i)
        # Shrink until it fits the byte budget too.
        while take > 0:
            blob = serialise_directory(entries[i:i + take])
            if len(blob) <= MAX_LEAF_BYTES:
                break
            take -= 8
        if take <= 0:
            raise SystemExit("a single entry will not fit a 4 KB leaf")
        leaves.append((entries[i][0], blob))
        i += take
    return leaves


def write_archive(path, tiles, min_zoom, max_zoom, bbox):
    """tiles: list of (tile_id, blob), sorted by tile_id, already uncompressed."""
    os.makedirs(os.path.dirname(os.path.abspath(path)) or ".", exist_ok=True)

    # Lay the tile data out in id order and record where each landed. Runs of
    # identical tiles - ocean, empty land - are very common in a basemap, and
    # storing one copy is most of why an extract is small.
    data = bytearray()
    seen = {}
    entries = []
    for tid, blob in tiles:
        key = blob
        if key in seen:
            off, length = seen[key]
        else:
            off, length = len(data), len(blob)
            data.extend(blob)
            seen[key] = (off, length)
        entries.append((tid, 1, off, length))

    # Merge adjacent entries that point at the same bytes into one run, which
    # is what the run_length field is for and what keeps the directories small.
    merged = []
    for e in entries:
        if merged:
            ptid, prun, poff, plen = merged[-1]
            if e[2] == poff and e[3] == plen and e[0] == ptid + prun:
                merged[-1] = (ptid, prun + 1, poff, plen)
                continue
        merged.append(e)

    leaves = build_leaves(merged)

    # The root points at the leaves: run_length 0, offset and length relative
    # to the leaf section.
    leaf_blob = bytearray()
    root_entries = []
    for first_id, blob in leaves:
        root_entries.append((first_id, 0, len(leaf_blob), len(blob)))
        leaf_blob.extend(blob)

    root = serialise_directory(root_entries)
    if len(root) > MAX_ROOT_BYTES:
        raise SystemExit(
            "root directory is %d bytes, over the reader's %d - "
            "extract a smaller area or a lower max zoom"
            % (len(root), MAX_ROOT_BYTES))

    metadata = b"{}"

    root_offset = HEADER_LEN
    metadata_offset = root_offset + len(root)
    leaf_offset = metadata_offset + len(metadata)
    data_offset = leaf_offset + len(leaf_blob)

    h = bytearray(HEADER_LEN)
    h[0:7] = b"PMTiles"
    h[7] = 3
    struct.pack_into("<11Q", h, 8,
                     root_offset, len(root),
                     metadata_offset, len(metadata),
                     leaf_offset, len(leaf_blob),
                     data_offset, len(data),
                     len(entries), len(merged), len(seen))
    h[96] = 1                      # clustered
    h[97] = COMPRESSION_NONE       # internal - the board has no zlib
    h[98] = COMPRESSION_NONE       # tile
    h[99] = TILETYPE_MVT
    h[100] = min_zoom
    h[101] = max_zoom
    struct.pack_into("<4i", h, 102,
                     int(bbox[0] * 1e7), int(bbox[1] * 1e7),
                     int(bbox[2] * 1e7), int(bbox[3] * 1e7))
    h[118] = min(max_zoom, max(min_zoom, (min_zoom + max_zoom) // 2))
    struct.pack_into("<2i", h, 119,
                     int((bbox[0] + bbox[2]) / 2 * 1e7),
                     int((bbox[1] + bbox[3]) / 2 * 1e7))

    with open(path, "wb") as f:
        f.write(h)
        f.write(root)
        f.write(metadata)
        f.write(leaf_blob)
        f.write(data)

    return {
        "root_bytes": len(root),
        "leaves": len(leaves),
        "leaf_bytes": len(leaf_blob),
        "tiles": len(entries),
        "unique": len(seen),
        "entries": len(merged),
        "data_bytes": len(data),
        "total": HEADER_LEN + len(root) + len(metadata) + len(leaf_blob) + len(data),
    }


# ------------------------------------------------------------------ main ---

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--url", required=True)
    ap.add_argument("--bbox", nargs=4, type=float, required=True,
                    metavar=("MIN_LON", "MIN_LAT", "MAX_LON", "MAX_LAT"))
    ap.add_argument("--minzoom", type=int, default=0)
    ap.add_argument("--maxzoom", type=int, default=13)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    remote = Remote(args.url)
    hdr = Header(remote.get(0, HEADER_LEN))
    print("remote: z%d..z%d, %d tiles addressed, tile compression %d"
          % (hdr.min_zoom, hdr.max_zoom, hdr.addressed, hdr.tile_compression))

    root = parse_directory(
        decompress(remote.get(hdr.root_offset, hdr.root_length, cache=True),
                   hdr.internal_compression))

    maxz = min(args.maxzoom, hdr.max_zoom)
    minz = max(args.minzoom, hdr.min_zoom)

    # Which tiles the box covers, per zoom.
    wanted = []
    for z in range(minz, maxz + 1):
        x0, y0 = lonlat_to_tile(args.bbox[0], args.bbox[3], z)
        x1, y1 = lonlat_to_tile(args.bbox[2], args.bbox[1], z)
        for x in range(x0, x1 + 1):
            for y in range(y0, y1 + 1):
                wanted.append((z, x, y, zxy_to_tileid(z, x, y)))
    print("%d tiles in the box for z%d..z%d" % (len(wanted), minz, maxz))

    # Look each one up, following leaves as needed.
    found = []
    missing = 0
    for i, (z, x, y, tid) in enumerate(sorted(wanted, key=lambda t: t[3])):
        entries = root
        base = None
        for _ in range(4):                      # the spec allows nesting
            e = find_entry(entries, tid)
            if e is None:
                break
            if e[1] == 0:
                blob = remote.get(hdr.leaf_offset + e[2], e[3], cache=True)
                entries = parse_directory(decompress(blob,
                                                     hdr.internal_compression))
                continue
            base = e
            break
        if base is None:
            missing += 1
            continue
        found.append((tid, base[2], base[3]))
        if (i + 1) % 2000 == 0:
            print("  looked up %d/%d (%d requests, %.1f MB)"
                  % (i + 1, len(wanted), remote.requests,
                     remote.bytes_fetched / 1e6))

    print("%d present, %d not in the archive" % (len(found), missing))
    if not found:
        raise SystemExit("nothing to write")

    # Fetch the tile bytes. Unique ranges only - a basemap repeats an empty
    # ocean tile thousands of times and they all point at the same bytes.
    ranges = sorted({(off, length) for _, off, length in found})
    print("%d unique byte ranges to fetch" % len(ranges))

    blobs = {}
    fetched = 0
    i = 0
    while i < len(ranges):
        # Coalesce ranges that are close together into one request: a seek is
        # a round trip and the gaps in clustered data are small.
        start_off, start_len = ranges[i]
        end = start_off + start_len
        j = i + 1
        while (j < len(ranges) and ranges[j][0] - end < 65536
               and (ranges[j][0] + ranges[j][1]) - start_off < 4 * 1024 * 1024):
            end = max(end, ranges[j][0] + ranges[j][1])
            j += 1
        chunk = remote.get(hdr.data_offset + start_off, end - start_off)
        for off, length in ranges[i:j]:
            blobs[(off, length)] = chunk[off - start_off:off - start_off + length]
        fetched += j - i
        if fetched % 2000 < (j - i):
            print("  fetched %d/%d ranges (%.1f MB)"
                  % (fetched, len(ranges), remote.bytes_fetched / 1e6))
        i = j

    # Gunzip once per unique blob, not once per tile.
    plain = {}
    for key, blob in blobs.items():
        plain[key] = decompress(blob, hdr.tile_compression)

    tiles = [(tid, plain[(off, length)]) for tid, off, length in found]
    tiles.sort(key=lambda t: t[0])

    stats = write_archive(args.out, tiles, minz, maxz, args.bbox)
    print("wrote %s" % args.out)
    print("  %d tiles, %d unique, %d directory entries"
          % (stats["tiles"], stats["unique"], stats["entries"]))
    print("  root %d B (limit %d), %d leaves totalling %d B"
          % (stats["root_bytes"], MAX_ROOT_BYTES, stats["leaves"],
             stats["leaf_bytes"]))
    print("  tile data %.1f MB, file %.1f MB"
          % (stats["data_bytes"] / 1e6, stats["total"] / 1e6))
    print("  downloaded %.1f MB in %d requests"
          % (remote.bytes_fetched / 1e6, remote.requests))


if __name__ == "__main__":
    main()
