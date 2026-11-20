// lakeshark-hub: bug reports, the file locker and anonymous chat for LakeShark (hub.terminalbay.com).
// One Worker, three Durable Objects (SQLite): ChatRoom (one per channel), Locker (one per paired
// locker), Registry (pairing codes, rate limits). Reports and locker files live in two private R2
// buckets that only this Worker can reach. No IP address is read, stored or logged anywhere.
import { DurableObject } from "cloudflare:workers";

const MB = 1024 * 1024;
const REPORT_MAX = 512 * 1024;
const FILE_MAX = 25 * MB;
const LOCKER_MAX = 200 * MB;
const FILE_TTL = 14 * 86400;
const CODE_TTL = 600;
const MSG_MAX = 280;
const KEEP_MSGS = 200;

const enc = new TextEncoder();
const hexOf = (u8) => [...u8].map((x) => x.toString(16).padStart(2, "0")).join("");
const rnd = (n) => hexOf(crypto.getRandomValues(new Uint8Array(n)));
const now = () => Math.floor(Date.now() / 1000);
async function sha(s) { return hexOf(new Uint8Array(await crypto.subtle.digest("SHA-256", enc.encode(s)))); }
function same(a, b) {
  if (typeof a !== "string" || typeof b !== "string" || a.length !== b.length) return false;
  let d = 0;
  for (let i = 0; i < a.length; i++) d |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return d === 0;
}

const CORS = {
  "access-control-allow-origin": "*",
  "access-control-allow-methods": "GET,POST,PUT,DELETE,OPTIONS",
  "access-control-allow-headers": "authorization,content-type,x-dev,x-secret,x-note,x-board,x-ver,x-lakeshark-token",
  "access-control-max-age": "86400",
};
const J = (o, s = 200) => new Response(JSON.stringify(o), { status: s, headers: { ...CORS, "content-type": "application/json", "cache-control": "no-store" } });
const T = (t, s = 200) => new Response(t, { status: s, headers: { ...CORS, "content-type": "text/plain; charset=utf-8", "cache-control": "no-store" } });
const ERR = (s, m) => J({ error: m }, s);

// ---- handles --------------------------------------------------------------------------------
const ADJ = ["amber", "brave", "calm", "dusty", "eager", "faint", "gentle", "hazy", "icy", "jolly", "keen", "lucky", "misty", "noble", "odd", "proud",
  "quick", "rusty", "silent", "tiny", "urban", "vivid", "warm", "young", "zesty", "bold", "clever", "dapper", "fuzzy", "grand", "humble", "swift"];
const ANI = ["otter", "heron", "lynx", "badger", "falcon", "gecko", "ibis", "jackal", "koala", "lemur", "marten", "newt", "osprey", "panda", "quail", "raven",
  "stoat", "tapir", "urial", "viper", "walrus", "yak", "zebra", "bison", "crane", "dingo", "egret", "ferret", "gopher", "hare", "iguana", "manta"];
async function handleFor(did) {
  const h = new Uint8Array(await crypto.subtle.digest("SHA-256", enc.encode("lakeshark-handle-v1:" + did)));
  return ADJ[h[0] & 31] + "-" + ANI[h[1] & 31] + "-" + hexOf(h.slice(2, 4));
}
const DID = /^[0-9a-f]{16,32}$/;
const SEC = /^[0-9a-f]{32,64}$/;

// ---- chat text filter -----------------------------------------------------------------------
const BAD = ["fuck", "shit", "bitch", "cunt", "asshole", "dickhead", "bastard", "slut", "whore", "nigger", "nigga", "faggot", "retard", "kike", "spic", "chink", "tranny", "rape", "cock", "pussy"];
function norm(s) {
  return s.toLowerCase().replace(/[@4]/g, "a").replace(/[3]/g, "e").replace(/[1!|]/g, "i").replace(/[0]/g, "o").replace(/[$5]/g, "s").replace(/[^a-z]/g, "");
}
function filterText(raw) {
  let t = String(raw ?? "").replace(/[\u0000-\u001f\u007f-\u009f​-‏‪-‮⁠-⁤﻿]/g, " ").replace(/\s+/g, " ").trim();
  if (!t) return { err: "empty" };
  if ([...t].length > MSG_MAX) return { err: "too long" };
  if (/(https?|ftp|ws)\s*:|:\s*\/\/|\bwww\s*\.|\b[a-z0-9-]{2,}\s*(\.|\s+dot\s+)\s*(com|net|org|io|co|me|ly|gg|tv|xyz|info|ru|cn|to|app|dev|ai|us|uk|de|fm|cc|ws|biz|link|club|top|site|online|store|live)\b/i.test(t)) return { err: "links are not allowed" };
  const n = norm(t);
  for (const w of BAD) if (n.includes(w)) return { err: "language" };
  return { text: t };
}

// ---- entry ----------------------------------------------------------------------------------
export default {
  async fetch(req, env, ctx) {
    if (req.method === "OPTIONS") return new Response(null, { status: 204, headers: CORS });
    const u = new URL(req.url);
    const p = u.pathname.replace(/\/+$/, "") || "/";
    try {
      if (p === "/" || p === "/v1") return J({ service: "lakeshark-hub", ok: true, t: now() });
      if (p === "/v1/whoami") {
        const d = req.headers.get("x-dev") || "";
        return DID.test(d) ? J({ handle: await handleFor(d) }) : ERR(400, "bad device id");
      }
      if (p === "/v1/report" && req.method === "POST") return report(req, env, ctx);
      if (p === "/v1/reports" || p.startsWith("/v1/reports/")) return reportsAdmin(req, env, p, u);
      if (p.startsWith("/v1/locker")) return locker(req, env, p, u);
      if (p.startsWith("/v1/chat")) return chat(req, env, p, u);
    } catch (e) {
      return ERR(500, "internal");
    }
    return ERR(404, "not found");
  },
};

const reg = (env) => env.REGISTRY.get(env.REGISTRY.idFromName("r"));
async function rate(env, key, limit, win) {
  const r = await reg(env).fetch("https://r/rl", { method: "POST", body: JSON.stringify({ key, limit, win }) });
  return (await r.json()).ok;
}
function isMod(req, env) {
  const m = env.MOD_SECRET;
  const a = (req.headers.get("authorization") || "").replace(/^Bearer\s+/i, "");
  return !!m && same(a, m);
}

// ---- reports --------------------------------------------------------------------------------
async function report(req, env, ctx) {
  const dev = req.headers.get("x-dev") || "";
  if (!DID.test(dev)) return ERR(400, "bad device id");
  const len = parseInt(req.headers.get("content-length") || "-1", 10);
  if (len < 0) return ERR(411, "length required");
  if (len > REPORT_MAX) return ERR(413, "over 512 KB");
  if (len < 2) return ERR(400, "empty");
  if (!(await rate(env, "rep:" + dev, 5, 3600)) || !(await rate(env, "rep:*", 400, 3600))) return ERR(429, "too many reports, try later");
  const body = await req.arrayBuffer();
  if (body.byteLength > REPORT_MAX) return ERR(413, "over 512 KB");
  let board = (req.headers.get("x-board") || "").slice(0, 32), ver = (req.headers.get("x-ver") || "").slice(0, 32);
  let note = (req.headers.get("x-note") || "").slice(0, 100);
  if (!board || !ver) {
    try {
      const j = JSON.parse(new TextDecoder().decode(body));
      board = board || String(j.board || "").slice(0, 32);
      ver = ver || String(j.version || j.ver || "").slice(0, 32);
      note = note || String(j.note || "").slice(0, 100);
    } catch {}
  }
  const clean = (s) => s.replace(/[^\x20-\x7e]/g, " ").trim();
  board = clean(board); ver = clean(ver); note = clean(note);
  const ts = Date.now();
  const id = ts.toString(36).padStart(9, "0") + "-" + rnd(3);
  const dh = (await sha("dev:" + dev)).slice(0, 8);
  await env.REPORTS.put("r/" + id, body, {
    httpMetadata: { contentType: req.headers.get("content-type") || "application/json" },
    customMetadata: { board, ver, note, dev: dh, ts: String(ts), size: String(body.byteLength) },
  });
  if (env.NTFY_TOPIC) {
    const line = `report ${id} ${board || "?"} ${ver || "?"} ${(body.byteLength / 1024).toFixed(0)}K${note ? " - " + note.slice(0, 60) : ""}`;
    ctx.waitUntil(fetch("https://ntfy.sh/" + encodeURIComponent(env.NTFY_TOPIC), {
      method: "POST", body: line, headers: { Title: "LakeShark report", Tags: "bug" },
    }).catch(() => {}));
  }
  return J({ ok: true, id });
}

async function reportsAdmin(req, env, p, u) {
  if (!isMod(req, env)) return ERR(401, "moderator key required");
  if (p === "/v1/reports") {
    const lim = Math.min(1000, parseInt(u.searchParams.get("limit") || "200", 10) || 200);
    const o = await env.REPORTS.list({ prefix: "r/", limit: lim, cursor: u.searchParams.get("cursor") || undefined, include: ["customMetadata"] });
    return J({
      truncated: o.truncated, cursor: o.truncated ? o.cursor : null,
      reports: o.objects.map((x) => ({ id: x.key.slice(2), size: x.size, ...(x.customMetadata || {}) })),
    });
  }
  const id = p.slice("/v1/reports/".length);
  if (!/^[0-9a-z]{9}-[0-9a-f]{6}$/.test(id)) return ERR(400, "bad id");
  if (req.method === "DELETE") { await env.REPORTS.delete("r/" + id); return J({ ok: true }); }
  const o = await env.REPORTS.get("r/" + id);
  if (!o) return ERR(404, "no such report");
  return new Response(o.body, { headers: { ...CORS, "content-type": o.httpMetadata?.contentType || "application/json", "cache-control": "no-store" } });
}

// ---- locker ---------------------------------------------------------------------------------
const NAME = /^[A-Za-z0-9_][A-Za-z0-9._ -]{0,62}$/;
function tokenOf(req) {
  const a = (req.headers.get("authorization") || "").replace(/^Bearer\s+/i, "") || req.headers.get("x-lakeshark-token") || "";
  const i = a.indexOf(".");
  if (i < 0) return null;
  const lid = a.slice(0, i), tok = a.slice(i + 1);
  return DID.test(lid) && SEC.test(tok) ? { lid, tok } : null;
}
const lk = (env, lid) => env.LOCKER_DO.get(env.LOCKER_DO.idFromName(lid));
async function lcall(env, lid, op, body) {
  const r = await lk(env, lid).fetch("https://l/" + op, { method: "POST", body: JSON.stringify(body || {}) });
  return { s: r.status, j: await r.json() };
}

async function locker(req, env, p, u) {
  const sub = p.slice("/v1/locker".length);
  // device: ask for a pairing code (creates the locker on first use)
  if (sub === "/pair" && req.method === "POST") {
    const a = tokenOf(req);
    if (!a) return ERR(401, "device id and secret required");
    const r = await lcall(env, a.lid, "init", { sh: await sha("s:" + a.tok) });
    if (r.s !== 200) return J(r.j, r.s);
    const c = await (await reg(env).fetch("https://r/code", { method: "POST", body: JSON.stringify({ lid: a.lid, ttl: CODE_TTL }) })).json();
    return J({ code: c.code, expires_in: CODE_TTL, lid: a.lid });
  }
  // website or desktop: trade a code for a locker session
  if (sub === "/claim" && req.method === "POST") {
    let code = "";
    try { code = String((await req.json()).code || "").replace(/\D/g, ""); } catch {}
    if (code.length !== 6) return ERR(400, "six digits");
    const c = await (await reg(env).fetch("https://r/claim", { method: "POST", body: JSON.stringify({ code }) })).json();
    if (!c.lid) return ERR(c.locked ? 429 : 404, c.locked ? "too many wrong codes, wait a minute" : "wrong or expired code");
    const r = await lcall(env, c.lid, "newweb", {});
    return J({ lid: c.lid, token: r.j.token, secret: c.lid + "." + r.j.token });
  }
  const a = tokenOf(req);
  if (!a) return ERR(401, "locker token required");
  const au = await lcall(env, a.lid, "auth", { h: await sha("s:" + a.tok), w: await sha("w:" + a.tok) });
  if (au.s !== 200) return ERR(401, "bad locker token");
  const role = au.j.role; // "dev" | "web"
  if (sub === "/unpair" && req.method === "POST") {
    if (role !== "dev" && !u.searchParams.has("self")) return ERR(403, "device only");
    await lcall(env, a.lid, "revoke", { all: role === "dev", h: await sha("w:" + a.tok) });
    return J({ ok: true });
  }
  if (sub === "/info") return J(await (await lk(env, a.lid).fetch("https://l/info", { method: "POST", body: "{}" })).json());
  if (sub === "/files" && req.method === "GET") {
    const r = await lcall(env, a.lid, "list", {});
    if (u.searchParams.get("fmt") === "t")
      return T(r.j.files.map((f) => `${f.name}\t${f.size}\t${now() - f.ts}\t${f.src}`).join("\n") + (r.j.files.length ? "\n" : ""));
    return J(r.j);
  }
  if (sub.startsWith("/files/")) {
    const name = decodeURIComponent(sub.slice(7));
    if (!NAME.test(name) || name.endsWith(".") || name.endsWith(" ")) return ERR(400, "bad file name");
    const key = a.lid + "/" + name;
    if (req.method === "PUT") {
      const len = parseInt(req.headers.get("content-length") || "-1", 10);
      if (len < 0) return ERR(411, "length required");
      if (len > FILE_MAX) return ERR(413, "over 25 MB");
      const r = await lcall(env, a.lid, "reserve", { name, size: len, src: role, ttl: FILE_TTL });
      if (r.s !== 200) return J(r.j, r.s);
      try {
        await env.LOCKER.put(key, req.body, { httpMetadata: { contentType: req.headers.get("content-type") || "application/octet-stream" } });
      } catch {
        await lcall(env, a.lid, "drop", { name });
        return ERR(500, "upload failed");
      }
      return J({ ok: true, name, size: len, expires_in: FILE_TTL });
    }
    if (req.method === "GET") {
      const m = await lcall(env, a.lid, "meta", { name });
      if (m.s !== 200) return ERR(404, "no such file");
      const o = await env.LOCKER.get(key);
      if (!o) return ERR(404, "no such file");
      return new Response(o.body, { headers: { ...CORS, "content-type": "application/octet-stream", "content-length": String(o.size), "content-disposition": `attachment; filename="${name.replace(/"/g, "")}"`, "cache-control": "no-store" } });
    }
    if (req.method === "DELETE") {
      const m = await lcall(env, a.lid, "drop", { name });
      await env.LOCKER.delete(key);
      return J({ ok: true, existed: m.j.existed });
    }
  }
  return ERR(404, "not found");
}

// ---- chat -----------------------------------------------------------------------------------
const CHAN = /^(global|[a-z]{2}|z6-([0-9]|[1-5][0-9]|6[0-3])-([0-9]|[1-5][0-9]|6[0-3]))$/;
async function chat(req, env, p, u) {
  const parts = p.split("/").filter(Boolean); // v1 chat <ch> [op]
  if (parts.length < 3) return J({ channels: ["global", "<2-letter country code>", "z6-<x>-<y>"], max: MSG_MAX });
  const ch = parts[2];
  if (!CHAN.test(ch)) return ERR(400, "bad channel");
  const op = parts[3] || "";
  const stub = env.CHAT.get(env.CHAT.idFromName(ch));
  const fwd = (path, init) => stub.fetch("https://c/" + path + u.search, init);
  if (op === "" && req.method === "GET") return fwd("poll", { headers: { "x-fmt": u.searchParams.get("fmt") || "" } });
  if (op === "ws") {
    if (req.headers.get("upgrade") !== "websocket") return ERR(426, "websocket required");
    const d = u.searchParams.get("did") || "";
    if (!DID.test(d)) return ERR(400, "bad device id");
    return stub.fetch(new Request("https://c/ws?h=" + encodeURIComponent(await handleFor(d)), req));
  }
  if (op === "mod" && req.method === "POST") {
    if (!isMod(req, env)) return ERR(401, "moderator key required");
    return fwd("mod", { method: "POST", body: await req.text() });
  }
  if (op === "" && req.method === "POST") {
    const d = req.headers.get("x-dev") || "";
    if (!DID.test(d)) return ERR(400, "bad device id");
    const ct = req.headers.get("content-type") || "";
    let raw = (await req.text()).slice(0, 2000);
    if (ct.includes("json")) { try { raw = JSON.parse(raw).text; } catch { return ERR(400, "bad json"); } }
    const f = filterText(raw);
    if (f.err) return ERR(422, f.err);
    return stub.fetch("https://c/send", { method: "POST", body: JSON.stringify({ h: await handleFor(d), t: f.text }), headers: { "x-fmt": u.searchParams.get("fmt") || "" } });
  }
  return ERR(404, "not found");
}

export class ChatRoom extends DurableObject {
  constructor(state, env) {
    super(state, env);
    this.sql = state.storage.sql;
    this.waiters = new Set();
    this.bucket = new Map(); // handle -> {n, t}
    this.recent = [];        // room-wide send times
    this.sql.exec("CREATE TABLE IF NOT EXISTS msgs(id INTEGER PRIMARY KEY AUTOINCREMENT, ts INTEGER, h TEXT, x TEXT)");
    this.sql.exec("CREATE TABLE IF NOT EXISTS bans(h TEXT PRIMARY KEY)");
  }
  rows(since, n) {
    return since < 0
      ? this.sql.exec("SELECT id,ts,h,x FROM (SELECT * FROM msgs ORDER BY id DESC LIMIT ?) ORDER BY id", n).toArray()
      : this.sql.exec("SELECT id,ts,h,x FROM msgs WHERE id>? ORDER BY id LIMIT ?", since, n).toArray();
  }
  last() { return this.sql.exec("SELECT COALESCE(MAX(id),0) AS m FROM msgs").one().m; }
  pack(rows, fmt) {
    const last = rows.length ? rows[rows.length - 1].id : null;
    return { last, rows, fmt };
  }
  out(rows, fmt, fallbackLast) {
    const last = rows.length ? rows[rows.length - 1].id : fallbackLast;
    if (fmt === "t")
      return T(`last\t${last}\n` + rows.map((r) => `${r.id}\t${r.ts}\t${r.h}\t${r.x}\n`).join(""));
    return J({ last, m: rows.map((r) => ({ i: r.id, t: r.ts, h: r.h, x: r.x })) });
  }
  async fetch(req) {
    const u = new URL(req.url);
    const op = u.pathname.slice(1);
    const fmt = req.headers.get("x-fmt") || u.searchParams.get("fmt") || "";
    if (op === "poll") {
      const sp = u.searchParams.get("since");
      const since = sp === null ? -1 : parseInt(sp, 10);
      const n = Math.min(KEEP_MSGS, Math.max(1, parseInt(u.searchParams.get("n") || "30", 10) || 30));
      const wait = Math.min(50, Math.max(0, parseInt(u.searchParams.get("wait") || "0", 10) || 0));
      let rows = this.rows(since, n);
      if (!rows.length && wait && since >= 0 && this.waiters.size < 300) {
        await new Promise((res) => {
          const w = { res };
          this.waiters.add(w);
          setTimeout(() => { this.waiters.delete(w); res(); }, wait * 1000);
        });
        rows = this.rows(since, n);
      }
      return this.out(rows, fmt, since >= 0 ? since : this.last());
    }
    if (op === "send") {
      const { h, t } = await req.json();
      const r = this.add(h, t);
      if (r.err) return ERR(r.code, r.err);
      return fmt === "t" ? T(`ok\t${r.id}\t${h}\n`) : J({ ok: true, id: r.id, handle: h });
    }
    if (op === "mod") {
      const j = await req.json().catch(() => ({}));
      if (j.delete) {
        this.sql.exec("DELETE FROM msgs WHERE id=?", j.delete);
        this.bcast({ t: "del", i: j.delete });
      }
      if (j.ban) this.sql.exec("INSERT OR IGNORE INTO bans(h) VALUES(?)", String(j.ban));
      if (j.unban) this.sql.exec("DELETE FROM bans WHERE h=?", String(j.unban));
      if (j.delete_by) { // wipe every message of a handle
        const ids = this.sql.exec("SELECT id FROM msgs WHERE h=?", String(j.delete_by)).toArray();
        this.sql.exec("DELETE FROM msgs WHERE h=?", String(j.delete_by));
        for (const r of ids) this.bcast({ t: "del", i: r.id });
      }
      return J({ ok: true, bans: this.sql.exec("SELECT h FROM bans").toArray().map((x) => x.h) });
    }
    if (op === "ws") {
      const pair = new WebSocketPair();
      this.ctx.acceptWebSocket(pair[1]);
      pair[1].serializeAttachment({ h: u.searchParams.get("h") });
      const rows = this.rows(-1, 50);
      pair[1].send(JSON.stringify({ t: "hello", handle: u.searchParams.get("h"), m: rows.map((r) => ({ i: r.id, t: r.ts, h: r.h, x: r.x })) }));
      return new Response(null, { status: 101, webSocket: pair[0] });
    }
    return ERR(404, "not found");
  }
  add(h, t) {
    if (this.sql.exec("SELECT 1 FROM bans WHERE h=?", h).toArray().length) return { err: "banned", code: 403 };
    const nowMs = Date.now();
    const b = this.bucket.get(h) || { n: 5, t: nowMs };
    b.n = Math.min(5, b.n + (nowMs - b.t) / 3000); b.t = nowMs;
    if (b.n < 1) return { err: "slow down", code: 429 };
    this.recent = this.recent.filter((x) => nowMs - x < 10000);
    if (this.recent.length >= 25) return { err: "room is busy", code: 429 };
    b.n -= 1; this.bucket.set(h, b); this.recent.push(nowMs);
    if (this.bucket.size > 2000) this.bucket.clear();
    const c = this.sql.exec("INSERT INTO msgs(ts,h,x) VALUES(?,?,?) RETURNING id", now(), h, t).one();
    this.sql.exec("DELETE FROM msgs WHERE id<=?", c.id - KEEP_MSGS);
    const m = { t: "msg", i: c.id, ts: now(), h, x: t };
    this.bcast(m);
    for (const w of this.waiters) w.res();
    this.waiters.clear();
    return { id: c.id };
  }
  bcast(m) {
    const s = JSON.stringify(m);
    for (const ws of this.ctx.getWebSockets()) { try { ws.send(s); } catch {} }
  }
  async webSocketMessage(ws, msg) {
    const at = ws.deserializeAttachment() || {};
    let t = "";
    try { const j = JSON.parse(String(msg)); t = j.text; } catch { return; }
    const f = filterText(t);
    if (f.err) { ws.send(JSON.stringify({ t: "err", e: f.err })); return; }
    const r = this.add(at.h, f.text);
    if (r.err) ws.send(JSON.stringify({ t: "err", e: r.err }));
  }
  webSocketClose(ws) { try { ws.close(); } catch {} }
}

// ---- locker metadata ------------------------------------------------------------------------
export class Locker extends DurableObject {
  constructor(state, env) {
    super(state, env);
    this.sql = state.storage.sql;
    this.sql.exec("CREATE TABLE IF NOT EXISTS meta(k TEXT PRIMARY KEY, v TEXT)");
    this.sql.exec("CREATE TABLE IF NOT EXISTS files(name TEXT PRIMARY KEY, size INTEGER, ts INTEGER, exp INTEGER, src TEXT)");
    this.sql.exec("CREATE TABLE IF NOT EXISTS webs(h TEXT PRIMARY KEY, ts INTEGER)");
  }
  get(k) { const r = this.sql.exec("SELECT v FROM meta WHERE k=?", k).toArray(); return r.length ? r[0].v : null; }
  used() { return this.sql.exec("SELECT COALESCE(SUM(size),0) AS s FROM files WHERE exp>?", now()).one().s; }
  async fetch(req) {
    const op = new URL(req.url).pathname.slice(1);
    const b = await req.json().catch(() => ({}));
    const t = now();
    if (op === "init") {
      const have = this.get("dev");
      if (have && !same(have, b.sh)) return J({ error: "this locker belongs to another device secret" }, 403);
      if (!have) this.sql.exec("INSERT INTO meta VALUES('dev',?)", b.sh);
      if (!(await this.ctx.storage.getAlarm())) await this.ctx.storage.setAlarm(Date.now() + 86400000);
      return J({ ok: true });
    }
    if (op === "auth") {
      const dev = this.get("dev");
      if (dev && same(dev, b.h)) return J({ role: "dev" });
      if (this.sql.exec("SELECT 1 FROM webs WHERE h=?", b.w).toArray().length) return J({ role: "web" });
      return J({ error: "no" }, 401);
    }
    if (op === "newweb") {
      const tok = rnd(24);
      this.sql.exec("INSERT INTO webs VALUES(?,?)", await sha("w:" + tok), t);
      this.sql.exec("DELETE FROM webs WHERE h NOT IN (SELECT h FROM webs ORDER BY ts DESC LIMIT 8)");
      return J({ token: tok });
    }
    if (op === "revoke") {
      if (b.all) this.sql.exec("DELETE FROM webs"); else this.sql.exec("DELETE FROM webs WHERE h=?", b.h);
      return J({ ok: true });
    }
    if (op === "info") return J({ used: this.used(), quota: LOCKER_MAX, file_max: FILE_MAX, ttl_s: FILE_TTL, files: this.sql.exec("SELECT COUNT(*) AS n FROM files WHERE exp>?", t).one().n });
    if (op === "list") {
      const files = this.sql.exec("SELECT name,size,ts,exp,src FROM files WHERE exp>? ORDER BY ts DESC", t).toArray();
      return J({ files, used: this.used(), quota: LOCKER_MAX });
    }
    if (op === "reserve") {
      const old = this.sql.exec("SELECT size FROM files WHERE name=? AND exp>?", b.name, t).toArray();
      if (this.used() - (old.length ? old[0].size : 0) + b.size > LOCKER_MAX) return J({ error: "locker full (200 MB)" }, 413);
      this.sql.exec("INSERT OR REPLACE INTO files VALUES(?,?,?,?,?)", b.name, b.size, t, t + b.ttl, b.src);
      return J({ ok: true });
    }
    if (op === "meta") {
      const r = this.sql.exec("SELECT size,ts,exp,src FROM files WHERE name=? AND exp>?", b.name, t).toArray();
      return r.length ? J(r[0]) : J({ error: "no" }, 404);
    }
    if (op === "drop") {
      const had = this.sql.exec("SELECT 1 FROM files WHERE name=?", b.name).toArray().length > 0;
      this.sql.exec("DELETE FROM files WHERE name=?", b.name);
      return J({ ok: true, existed: had });
    }
    return J({ error: "?" }, 404);
  }
  async alarm() {
    const lid = this.ctx.id.name;
    const old = this.sql.exec("SELECT name FROM files WHERE exp<=?", now()).toArray();
    for (const f of old) { try { await this.env.LOCKER.delete(lid + "/" + f.name); } catch {} }
    this.sql.exec("DELETE FROM files WHERE exp<=?", now());
    if (this.sql.exec("SELECT COUNT(*) AS n FROM files").one().n) await this.ctx.storage.setAlarm(Date.now() + 86400000);
  }
}

// ---- pairing codes and rate limits ----------------------------------------------------------
export class Registry extends DurableObject {
  constructor(state, env) {
    super(state, env);
    this.sql = state.storage.sql;
    this.sql.exec("CREATE TABLE IF NOT EXISTS codes(code TEXT PRIMARY KEY, lid TEXT, exp INTEGER)");
    this.sql.exec("CREATE TABLE IF NOT EXISTS rl(k TEXT PRIMARY KEY, n INTEGER, reset INTEGER)");
    this.fails = [];
  }
  async fetch(req) {
    const op = new URL(req.url).pathname.slice(1);
    const b = await req.json().catch(() => ({}));
    const t = now();
    if (!(await this.ctx.storage.getAlarm())) await this.ctx.storage.setAlarm(Date.now() + 3600000);
    if (op === "rl") {
      const r = this.sql.exec("SELECT n,reset FROM rl WHERE k=?", b.key).toArray()[0];
      if (!r || r.reset <= t) { this.sql.exec("INSERT OR REPLACE INTO rl VALUES(?,1,?)", b.key, t + b.win); return J({ ok: true }); }
      if (r.n >= b.limit) return J({ ok: false });
      this.sql.exec("UPDATE rl SET n=n+1 WHERE k=?", b.key);
      return J({ ok: true });
    }
    if (op === "code") {
      this.sql.exec("DELETE FROM codes WHERE lid=?", b.lid);
      for (let i = 0; i < 20; i++) {
        const code = String(crypto.getRandomValues(new Uint32Array(1))[0] % 1000000).padStart(6, "0");
        if (this.sql.exec("SELECT 1 FROM codes WHERE code=? AND exp>?", code, t).toArray().length) continue;
        this.sql.exec("INSERT OR REPLACE INTO codes VALUES(?,?,?)", code, b.lid, t + b.ttl);
        return J({ code });
      }
      return J({ error: "busy" }, 503);
    }
    if (op === "claim") {
      this.fails = this.fails.filter((x) => t - x < 60);
      if (this.fails.length >= 20) return J({ locked: true });
      const r = this.sql.exec("SELECT lid FROM codes WHERE code=? AND exp>?", b.code, t).toArray()[0];
      if (!r) { this.fails.push(t); return J({}); }
      this.sql.exec("DELETE FROM codes WHERE code=?", b.code);
      return J({ lid: r.lid });
    }
    return J({ error: "?" }, 404);
  }
  async alarm() {
    this.sql.exec("DELETE FROM codes WHERE exp<=?", now());
    this.sql.exec("DELETE FROM rl WHERE reset<=?", now());
    await this.ctx.storage.setAlarm(Date.now() + 3600000);
  }
}
