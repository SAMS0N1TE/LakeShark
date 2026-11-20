// lakeshark-tiles: z/x/y vector tiles for LakeShark boards, read out of a
// Protomaps planet PMTiles archive with HTTP range requests.
//
//   GET /v1/meta.json            zooms, layers, attribution, build
//   GET /v1/{z}/{x}/{y}.mvt      slim tile (roads, water, boundaries, places), not compressed
//   GET /v1/{z}/{x}/{y}.mvt?full=1   every layer of the source tile
//
// A tile is looked for at the edge (Cache API), then in the R2 bucket, then
// in the upstream archive; what upstream gave is kept in R2, so each tile is
// read from the archive once. Map data (c) OpenStreetMap contributors, ODbL;
// tile schema and build by Protomaps (BSD / ODbL produced work).

const ATTRIBUTION = "(c) OpenStreetMap contributors, ODbL 1.0. Tiles: Protomaps basemap.";
const KEEP = ["roads", "water", "boundaries", "places"];
const MAX_Z = 15;
const UA = { "User-Agent": "tiles-edge/1.0" };

let g_src = null;          // { url, build, hdr, root, at }
const g_leaf = new Map();  // "build:offset" -> parsed directory (small LRU)

function hdrs(extra) {
  return Object.assign({
    "Access-Control-Allow-Origin": "*",
    "X-Attribution": ATTRIBUTION,
    "Link": '<https://www.openstreetmap.org/copyright>; rel="license"',
  }, extra || {});
}

async function gunzip(buf) {
  const s = new Response(buf).body.pipeThrough(new DecompressionStream("gzip"));
  return new Uint8Array(await new Response(s).arrayBuffer());
}

async function range(url, off, len) {
  const r = await fetch(url, { headers: Object.assign({ Range: `bytes=${off}-${off + len - 1}` }, UA) });
  if (r.status !== 206 && r.status !== 200) throw new Error("upstream " + r.status);
  const b = new Uint8Array(await r.arrayBuffer());
  if (b.length !== len) throw new Error("upstream short read");
  return b;
}

function u64(v, o) { return v.getUint32(o, true) + v.getUint32(o + 4, true) * 4294967296; }

function parseHeader(b) {
  if (b.length < 127 || String.fromCharCode(...b.subarray(0, 7)) !== "PMTiles" || b[7] !== 3)
    throw new Error("not PMTiles v3");
  const v = new DataView(b.buffer, b.byteOffset, b.byteLength);
  return {
    rootOff: u64(v, 8), rootLen: u64(v, 16), leafOff: u64(v, 40), leafLen: u64(v, 48),
    dataOff: u64(v, 56), dirComp: b[97], tileComp: b[98], type: b[99], minZ: b[100], maxZ: b[101],
  };
}

// A directory as four parallel arrays: tile id, run length, length, offset.
function parseDir(b) {
  let p = 0;
  const vi = () => {
    let r = 0, m = 1;
    for (;;) {
      const c = b[p++];
      r += (c & 0x7f) * m;
      if (c < 0x80) return r;
      m *= 128;
      if (p > b.length) throw new Error("bad directory");
    }
  };
  const n = vi();
  const id = new Float64Array(n), run = new Uint32Array(n), len = new Uint32Array(n), off = new Float64Array(n);
  let last = 0;
  for (let i = 0; i < n; i++) { last += vi(); id[i] = last; }
  for (let i = 0; i < n; i++) run[i] = vi();
  for (let i = 0; i < n; i++) len[i] = vi();
  for (let i = 0; i < n; i++) {
    const t = vi();
    off[i] = (t === 0 && i > 0) ? off[i - 1] + len[i - 1] : t - 1;
  }
  return { n, id, run, len, off };
}

function find(d, tid) {
  let lo = 0, hi = d.n - 1;
  while (lo <= hi) {
    const m = (lo + hi) >> 1;
    if (d.id[m] === tid) return m;
    if (d.id[m] < tid) lo = m + 1; else hi = m - 1;
  }
  return hi;   // the last entry at or below, or -1
}

function tileId(z, x, y) {
  let acc = 0;
  for (let i = 0; i < z; i++) acc += Math.pow(4, i);
  const n = 1 << z;
  let d = 0;
  for (let s = n >> 1; s > 0; s >>= 1) {
    const rx = (x & s) ? 1 : 0, ry = (y & s) ? 1 : 0;
    d += s * s * ((3 * rx) ^ ry);
    if (ry === 0) {
      if (rx === 1) { x = n - 1 - x; y = n - 1 - y; }
      const t = x; x = y; y = t;
    }
  }
  return acc + d;
}

async function source(env, ctx) {
  const now = Date.now();
  if (g_src && now - g_src.at < 6 * 3600 * 1000) return g_src;
  const cache = caches.default;
  const tries = [];
  if (env.ARCHIVE_URL) tries.push({ url: env.ARCHIVE_URL, build: "pinned" });
  else {
    try {
      const key = new Request("https://tiles.internal/_builds");
      let r = await cache.match(key);
      if (!r) {
        r = await fetch(env.BUILDS_URL, { headers: UA });
        if (r.ok) {
          r = new Response(await r.text(), { headers: { "Cache-Control": "max-age=21600" } });
          ctx.waitUntil(cache.put(key, r.clone()));
        }
      }
      const list = await r.json();
      list.sort((a, b) => (a.key < b.key ? 1 : -1));
      for (const b of list.slice(0, 2))
        tries.push({ url: env.BUILD_BASE + b.key, build: b.key.replace(".pmtiles", "") });
    } catch (e) { /* fall through to the mirror */ }
  }
  if (env.FALLBACK_URL) tries.push({ url: env.FALLBACK_URL, build: "mirror" });
  let err = null;
  for (const t of tries) {
    try {
      const head = await range(t.url, 0, 16384);
      const hdr = parseHeader(head);
      if (hdr.type !== 1) throw new Error("not vector tiles");
      let root = hdr.rootOff + hdr.rootLen <= 16384
        ? head.subarray(hdr.rootOff, hdr.rootOff + hdr.rootLen)
        : await range(t.url, hdr.rootOff, hdr.rootLen);
      if (hdr.dirComp === 2) root = await gunzip(root);
      g_src = { url: t.url, build: t.build, hdr, root: parseDir(root), at: now };
      g_leaf.clear();
      return g_src;
    } catch (e) { err = e; }
  }
  if (g_src) { g_src.at = now - 5 * 3600 * 1000; return g_src; }   // keep the old one, look again in an hour
  throw err || new Error("no upstream");
}

async function leaf(src, off, len, ctx) {
  const k = src.build + ":" + off;
  let d = g_leaf.get(k);
  if (d) return d;
  const cache = caches.default;
  const key = new Request(`https://tiles.internal/_dir/${src.build}/${off}-${len}`);
  let raw = null;
  const hit = await cache.match(key);
  if (hit) raw = new Uint8Array(await hit.arrayBuffer());
  else {
    raw = await range(src.url, src.hdr.leafOff + off, len);
    ctx.waitUntil(cache.put(key, new Response(raw, { headers: { "Cache-Control": "max-age=604800" } })));
  }
  d = parseDir(src.hdr.dirComp === 2 ? await gunzip(raw) : raw);
  if (g_leaf.size >= 12) g_leaf.delete(g_leaf.keys().next().value);
  g_leaf.set(k, d);
  return d;
}

// The tile's bytes as stored, or null when the archive has no tile there.
async function archiveTile(src, z, x, y, ctx) {
  const tid = tileId(z, x, y);
  let d = src.root;
  for (let depth = 0; depth < 4; depth++) {
    const i = find(d, tid);
    if (i < 0) return null;
    if (d.run[i] === 0) { d = await leaf(src, d.off[i], d.len[i], ctx); continue; }
    if (tid >= d.id[i] + d.run[i]) return null;
    if (d.len[i] === 0 || d.len[i] > 8 * 1024 * 1024) return null;
    return await range(src.url, src.hdr.dataOff + d.off[i], d.len[i]);
  }
  return null;
}

// Keep whole layers by name. A tile is a run of length-delimited layer
// messages (field 3); a layer's name is its field 1.
function slim(b) {
  const parts = [];
  let p = 0, total = 0;
  const vi = (lim) => {
    let r = 0, m = 1;
    for (;;) {
      if (p >= lim) throw new Error("bad tile");
      const c = b[p++];
      r += (c & 0x7f) * m;
      if (c < 0x80) return r;
      m *= 128;
    }
  };
  while (p < b.length) {
    const start = p, key = vi(b.length), wt = key & 7;
    if (wt !== 2) throw new Error("bad tile");
    const len = vi(b.length), body = p, end = p + len;
    if (end > b.length) throw new Error("bad tile");
    let name = "";
    if ((key >> 3) === 3) {
      p = body;
      while (p < end) {
        const k = vi(end), w = k & 7;
        if (w === 2) {
          const l = vi(end);
          if ((k >> 3) === 1) { name = new TextDecoder().decode(b.subarray(p, p + l)); break; }
          p += l;
        } else if (w === 0) vi(end);
        else if (w === 5) p += 4;
        else if (w === 1) p += 8;
        else break;
      }
    }
    p = end;
    if (KEEP.includes(name)) { parts.push(b.subarray(start, end)); total += end - start; }
  }
  const out = new Uint8Array(total);
  let o = 0;
  for (const s of parts) { out.set(s, o); o += s.length; }
  return out;
}

function tileResponse(body, src, where) {
  const empty = !body || body.length === 0;
  return new Response(empty ? null : body, {
    status: empty ? 204 : 200,
    headers: hdrs({
      "Content-Type": "application/vnd.mapbox-vector-tile",
      "Cache-Control": "public, max-age=604800",
      "X-Tile-Source": where,
    }),
  });
}

async function tile(req, env, ctx, z, x, y, full) {
  const cache = caches.default;
  const ckey = new Request(new URL(req.url).origin + `/v1/${z}/${x}/${y}.mvt` + (full ? "?full=1" : ""));
  const hit = await cache.match(ckey);
  if (hit) return hit;

  const rkey = `v1/${z}/${x}/${y}.mvt`;
  if (!full && env.CACHE) {
    const o = await env.CACHE.get(rkey);
    if (o) {
      const res = tileResponse(new Uint8Array(await o.arrayBuffer()), null, "store");
      ctx.waitUntil(cache.put(ckey, res.clone()));
      return res;
    }
  }

  const src = await source(env, ctx);
  let raw = await archiveTile(src, z, x, y, ctx);
  let body = new Uint8Array(0);
  if (raw) {
    if (src.hdr.tileComp === 2) raw = await gunzip(raw);
    body = full ? raw : slim(raw);
  }
  if (!full && env.CACHE)
    ctx.waitUntil(env.CACHE.put(rkey, body, { customMetadata: { build: src.build } }));
  const res = tileResponse(body, src, "archive");
  ctx.waitUntil(cache.put(ckey, res.clone()));
  return res;
}

export default {
  async fetch(req, env, ctx) {
    if (req.method === "OPTIONS")
      return new Response(null, { status: 204, headers: hdrs({ "Access-Control-Allow-Methods": "GET" }) });
    if (req.method !== "GET" && req.method !== "HEAD")
      return new Response("GET only\n", { status: 405, headers: hdrs() });
    const url = new URL(req.url);
    try {
      if (url.pathname === "/" || url.pathname === "/v1/meta.json") {
        let build = "unknown";
        try { build = (await source(env, ctx)).build; } catch (e) { /* still answer */ }
        return new Response(JSON.stringify({
          v: 1, format: "mvt", tiles: "/v1/{z}/{x}/{y}.mvt", minzoom: 0, maxzoom: MAX_Z,
          layers: KEEP, compressed: false, empty: 204, build, attribution: ATTRIBUTION,
          license: "https://www.openstreetmap.org/copyright",
        }) + "\n", { headers: hdrs({ "Content-Type": "application/json", "Cache-Control": "public, max-age=3600" }) });
      }
      const m = /^\/v1\/(\d{1,2})\/(\d{1,7})\/(\d{1,7})\.mvt$/.exec(url.pathname);
      if (!m) return new Response("not found\n", { status: 404, headers: hdrs() });
      const z = +m[1], x = +m[2], y = +m[3], n = Math.pow(2, z);
      if (z > MAX_Z || x >= n || y >= n) return new Response("no such tile\n", { status: 404, headers: hdrs() });
      return await tile(req, env, ctx, z, x, y, url.searchParams.get("full") === "1");
    } catch (e) {
      return new Response("tile source unavailable\n", {
        status: 502, headers: hdrs({ "Cache-Control": "no-store", "X-Tile-Error": String(e && e.message).slice(0, 80) }),
      });
    }
  },
};
