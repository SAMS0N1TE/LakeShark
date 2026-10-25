// The web screen: the board's screen over a WebSocket from the board itself.
// The stream is the USB live screen's (ls_rterm.c): 'term web ack z' starts
// it, deflated updates come in frames each ending in an empty one, and each
// is answered 'term ack' once drawn; keys and taps go back as 'term in <hex>'.
// The frame parser and the canvas renderer are the website's
// (terminalbay theme/lakeshark-device.js), so both draw the board alike.

'use strict';

const enc = new TextEncoder();
const $ = (id) => document.getElementById(id);
const TOKEN = 'lakeshark-token';

// ------------------------------------------------------------------ frames

// Frames from `term serial`: 0x1E, 4 hex length, payload, 0x1F, 4 hex CRC-16.
// With 'z' the board deflates each update and its pieces come in frames that
// start 0x1C instead; the empty 0x1E frame that ends the update ends the stream.
class FrameParser {
  constructor(onFrame) {
    this.onFrame = onFrame;
    this.st = 0; this.hdr = ''; this.need = 0; this.body = null; this.at = 0; this.crc = '';
  }
  push(bytes) {
    for (let i = 0; i < bytes.length; i++) {
      const b = bytes[i];
      switch (this.st) {
        case 0:
          if (b === 0x1e || b === 0x1c) { this.st = 1; this.hdr = ''; this.z = b === 0x1c; }
          break;
        case 1:
          this.hdr += String.fromCharCode(b);
          if (this.hdr.length === 4) {
            this.need = parseInt(this.hdr, 16);
            if (!(this.need >= 0 && this.need <= 4000)) { this.st = 0; break; }
            this.body = new Uint8Array(this.need); this.at = 0; this.st = this.need ? 2 : 3;
          }
          break;
        case 2: {
          const n = Math.min(this.need - this.at, bytes.length - i);
          this.body.set(bytes.subarray(i, i + n), this.at); this.at += n; i += n - 1;
          if (this.at === this.need) this.st = 3;
          break;
        }
        case 3: this.st = b === 0x1f ? 4 : 0; this.crc = ''; break;
        case 4:
          this.crc += String.fromCharCode(b);
          if (this.crc.length === 4) {
            const ok = parseInt(this.crc, 16) === crc16(this.body);
            this.st = 0;
            this.onFrame(ok ? this.body : null, this.z);
          }
          break;
      }
    }
  }
}
function crc16(b) {
  let c = 0xffff;
  for (const x of b) { c ^= x << 8; for (let k = 0; k < 8; k++) c = c & 0x8000 ? ((c << 1) ^ 0x1021) & 0xffff : (c << 1) & 0xffff; }
  return c;
}
async function inflate(parts) {
  const ds = new DecompressionStream('deflate-raw');
  const out = new Response(new Blob(parts).stream().pipeThrough(ds));
  return new Uint8Array(await out.arrayBuffer());
}
const canInflate = (() => {
  try { new DecompressionStream('deflate-raw'); return true; } catch (e) { return false; }
})();

// The board's own glyphs (lsfont.bin.gz, made by `lssim atlas`): every glyph of
// every font as coverage, so with them the page asks for cells ('cells') and
// draws exactly the pixels the glass would. Without them, terminal text.
let fonts = null;
const fontsReady = (async () => {
  if (!canInflate) return;
  try {
    const r = await fetch('lsfont.bin.gz');
    if (!r.ok) return;
    const raw = new Uint8Array(await new Response(r.body.pipeThrough(new DecompressionStream('gzip'))).arrayBuffer());
    if (String.fromCharCode(raw[0], raw[1], raw[2], raw[3]) !== 'LSF1') return;
    const list = [];
    let at = 5;
    for (let i = 0; i < raw[4]; i++) {
      const w = raw[at], h = raw[at + 1];
      at += 2;
      list.push({ w, h, cov: raw.subarray(at, at + 512 * w * h) });
      at += 512 * w * h;
    }
    fonts = list;
  } catch (e) { /* terminal text it is */ }
})();

// ------------------------------------------------------------------ the screen

// xterm.js parses the board's stream into a cell buffer without ever being put
// on a page; the canvas draws that buffer the way the board's glass does,
// blocks, sextants and braille as exact pixels.
const CELL_W = 10, CELL_H = 17;                           // the board's default font cell
let term = null;
const canvas = $('screen');
let sigs = null, paintPending = false;

const PALETTE = (() => {
  const base = ['#000000', '#cd3131', '#0dbc79', '#e5e510', '#2472c8', '#bc3fbc', '#11a8cd', '#e5e5e5',
    '#666666', '#f14c4c', '#23d18b', '#f5f543', '#3b8eea', '#d670d6', '#29b8db', '#ffffff'];
  const p = base.map(h => parseInt(h.slice(1), 16));
  const lv = [0, 95, 135, 175, 215, 255];
  for (let r = 0; r < 6; r++) for (let g = 0; g < 6; g++) for (let b = 0; b < 6; b++) p.push(lv[r] << 16 | lv[g] << 8 | lv[b]);
  for (let i = 0; i < 24; i++) { const v = 8 + i * 10; p.push(v << 16 | v << 8 | v); }
  return p;
})();
const DEF_FG = 0xe9eaee, DEF_BG = 0x000000;
const hex = (n) => '#' + n.toString(16).padStart(6, '0');
const QUAD = { 0x2596: 4, 0x2597: 8, 0x2598: 1, 0x2599: 13, 0x259a: 9, 0x259b: 7, 0x259c: 11, 0x259d: 2, 0x259e: 6, 0x259f: 14 };
const BOX = {};
(function () {
  const L = (cp, u, r, d, l) => { BOX[cp] = [u, r, d, l]; };
  L(0x2500, 0, 1, 0, 1); L(0x2501, 0, 2, 0, 2); L(0x2502, 1, 0, 1, 0); L(0x2503, 2, 0, 2, 0);
  L(0x250c, 0, 1, 1, 0); L(0x250f, 0, 2, 2, 0); L(0x2510, 0, 0, 1, 1); L(0x2513, 0, 0, 2, 2);
  L(0x2514, 1, 1, 0, 0); L(0x2517, 2, 2, 0, 0); L(0x2518, 1, 0, 0, 1); L(0x251b, 2, 0, 0, 2);
  L(0x251c, 1, 1, 1, 0); L(0x2523, 2, 2, 2, 0); L(0x2524, 1, 0, 1, 1); L(0x252b, 2, 0, 2, 2);
  L(0x252c, 0, 1, 1, 1); L(0x2533, 0, 2, 2, 2); L(0x2534, 1, 1, 0, 1); L(0x253b, 2, 2, 0, 2);
  L(0x253c, 1, 1, 1, 1); L(0x254b, 2, 2, 2, 2);
  L(0x2550, 0, 3, 0, 3); L(0x2551, 3, 0, 3, 0); L(0x2554, 0, 3, 3, 0); L(0x2557, 0, 0, 3, 3);
  L(0x255a, 3, 3, 0, 0); L(0x255d, 3, 0, 0, 3); L(0x2560, 3, 3, 3, 0); L(0x2563, 3, 0, 3, 3);
  L(0x2566, 0, 3, 3, 3); L(0x2569, 3, 3, 0, 3); L(0x256c, 3, 3, 3, 3);
  L(0x256d, 0, 1, 1, 0); L(0x256e, 0, 0, 1, 1); L(0x256f, 1, 0, 0, 1); L(0x2570, 1, 1, 0, 0);
  L(0x2574, 0, 0, 0, 1); L(0x2575, 1, 0, 0, 0); L(0x2576, 0, 1, 0, 0); L(0x2577, 0, 0, 1, 0);
})();
function mix(a, b, t) {
  const r = ((a >> 16) & 255) * t + ((b >> 16) & 255) * (1 - t);
  const g = ((a >> 8) & 255) * t + ((b >> 8) & 255) * (1 - t);
  const bl = (a & 255) * t + (b & 255) * (1 - t);
  return (r << 16) | (g << 8) | bl;
}

function schedulePaint() {
  if (paintPending) return;
  paintPending = true;
  requestAnimationFrame(() => { paintPending = false; paint(); });
}

// A row's look, to skip rows that have not changed since the canvas last drew them.
function rowSig(line, cell) {
  let h = 0;
  for (let x = 0; x < term.cols; x++) {
    line.getCell(x, cell);
    const str = cell.getChars();
    h = (Math.imul(h, 31) + (str ? str.codePointAt(0) : 32)) | 0;
    h = (Math.imul(h, 31) + (cell.isFgRGB() ? cell.getFgColor() : cell.getFgColor() ^ 0x5a5a5a)) | 0;
    h = (Math.imul(h, 31) + (cell.isBgRGB() ? cell.getBgColor() : cell.getBgColor() ^ 0xa5a5a5)) | 0;
    h = (Math.imul(h, 31) + (cell.isInverse() ? 7 : 1)) | 0;
  }
  return h;
}

// ------------------------------------------------------------------ cells

// The board's cells as it sent them, drawn into an image the size of its glass
// (cols x cell width by rows x cell height) with its own glyphs, then scaled.
let cm = null;
function cellsStart(cols, rows) {
  cm = { pal: new Uint32Array(16).fill(0xff000000) };
  cellsGrid(cols, rows, 0, CELL_W, CELL_H);
}
function cellsGrid(cols, rows, font, cw, ch) {
  const n = cols * rows;
  Object.assign(cm, { cols, rows, font, cw, ch, g: new Uint16Array(n).fill(32), a: new Uint8Array(n),
                      k: new Uint8Array(n), c1: new Uint16Array(n), c2: new Uint16Array(n) });
  cm.face = fonts.find(f => f.w === cw && f.h === ch) || fonts[font] || fonts[0];
  cm.img = new ImageData(cols * cw, rows * ch);
  cm.px = new Uint32Array(cm.img.data.buffer);
  cm.px.fill(0xff000000);
  cm.dirty = [0, 0, cols, rows];
}
const c565 = (v) => (((v >> 11 & 31) * 255 / 31) | 0) | (((v >> 5 & 63) * 255 / 63) | 0) << 8 |
                    (((v & 31) * 255 / 31) | 0) << 16 | 0xff000000;     // RGB565 to ImageData's ABGR
function cellsApply(raw) {
  const v = new DataView(raw.buffer, raw.byteOffset, raw.byteLength);
  let at = 0, repaint = false;
  while (at < raw.length) {
    const t = raw[at++];
    if (t === 0x47 && at + 7 <= raw.length) {          // G: the grid
      const cols = v.getUint16(at, true), rows = v.getUint16(at + 2, true), font = raw[at + 4], cw = raw[at + 5], ch = raw[at + 6];
      at += 7;
      if (cols !== cm.cols || rows !== cm.rows || cw !== cm.cw || ch !== cm.ch || font !== cm.font) {
        cellsGrid(cols, rows, font, cw, ch);
        sigs = null;
      }
    } else if (t === 0x50 && at + 32 <= raw.length) {  // P: the palette
      for (let i = 0; i < 16; i++) cm.pal[i] = c565(v.getUint16(at + i * 2, true));
      at += 32;
      repaint = true;
    } else if ((t === 0x43 || t === 0x58) && at + 4 <= raw.length) {   // C, X: cells
      const first = v.getUint16(at, true), n = v.getUint16(at + 2, true), size = t === 0x43 ? 3 : 8;
      at += 4;
      if (at + n * size > raw.length) break;
      for (let j = 0; j < n; j++, at += size) {
        const i = first + j;
        if (i >= cm.g.length) continue;
        cm.g[i] = v.getUint16(at, true);
        cm.a[i] = raw[at + 2];
        if (t === 0x58) { cm.k[i] = raw[at + 3]; cm.c1[i] = v.getUint16(at + 4, true); cm.c2[i] = v.getUint16(at + 6, true); }
        else cm.k[i] = 0;
        if (!repaint) cellDraw(i);
      }
    } else break;                                      // a record this page does not know: stop here
  }
  if (repaint) { for (let i = 0; i < cm.g.length; i++) cellDraw(i); cm.dirty = [0, 0, cm.cols, cm.rows]; }
}
function cellDraw(i) {
  const cw = cm.cw, ch = cm.ch, W = cm.cols * cw, px = cm.px;
  const col = i % cm.cols, row = (i / cm.cols) | 0, x0 = col * cw, y0 = row * ch;
  const d = cm.dirty;
  if (d) { d[0] = Math.min(d[0], col); d[1] = Math.min(d[1], row); d[2] = Math.max(d[2], col + 1); d[3] = Math.max(d[3], row + 1); }
  else cm.dirty = [col, row, col + 1, row + 1];
  const kind = cm.k[i], at = cm.a[i];
  if (kind === 1) {                                    // a picture cell: its two halves
    const top = c565(cm.c1[i]), bot = c565(cm.c2[i]), half = ch >> 1;
    for (let y = 0; y < ch; y++) px.fill(y < half ? top : bot, (y0 + y) * W + x0, (y0 + y) * W + x0 + cw);
    return;
  }
  const fg = cm.pal[at & 15], bg = kind === 2 ? c565(cm.c1[i]) : cm.pal[at >> 4];
  const code = cm.g[i], face = cm.face;
  const gi = code < 256 ? code : code >= 0x2800 && code <= 0x28ff ? 256 + code - 0x2800 : 63;
  if (!face || face.w !== cw || face.h !== ch) {       // no glyphs at this size: the ground
    for (let y = 0; y < ch; y++) px.fill(bg, (y0 + y) * W + x0, (y0 + y) * W + x0 + cw);
    return;
  }
  const cov = face.cov, base = gi * cw * ch;
  const fr = fg & 255, fgr = fg >> 8 & 255, fb = fg >> 16 & 255;
  const br = bg & 255, bgr = bg >> 8 & 255, bb = bg >> 16 & 255;
  for (let y = 0; y < ch; y++) {
    let o = (y0 + y) * W + x0, c = base + y * cw;
    for (let x = 0; x < cw; x++, o++, c++) {
      const a = cov[c];
      px[o] = a === 0 ? bg : a === 255 ? fg : 0xff000000 | (br + ((fr - br) * a >> 8)) |
              (bgr + ((fgr - bgr) * a >> 8)) << 8 | (bb + ((fb - bb) * a >> 8)) << 16;
    }
  }
}
function paintCells() {
  const stage = $('stage');
  const w = cm.cols * cm.cw, h = cm.rows * cm.ch;
  if (canvas.width !== w || canvas.height !== h) { canvas.width = w; canvas.height = h; cm.dirty = [0, 0, cm.cols, cm.rows]; }
  const fit = Math.max(0.05, Math.min(stage.clientWidth / w, stage.clientHeight / h));
  canvas.style.width = Math.floor(w * fit) + 'px';
  canvas.style.height = Math.floor(h * fit) + 'px';
  canvas.style.imageRendering = fit >= 1 ? 'pixelated' : 'auto';
  const d = cm.dirty;
  if (!d) return;
  cm.dirty = null;
  canvas.getContext('2d', { alpha: false }).putImageData(cm.img, 0, 0, d[0] * cm.cw, d[1] * cm.ch,
                                                         (d[2] - d[0]) * cm.cw, (d[3] - d[1]) * cm.ch);
}
// The grid on show, whichever way it is drawn.
function grid() { return cm ? [cm.cols, cm.rows] : term ? [term.cols, term.rows] : [0, 0]; }

// Size the canvas to the stage (crisp at the device's pixel ratio), then draw.
function paint() {
  if (cm) { paintCells(); return; }
  if (!term) return;
  const stage = $('stage');
  const dpr = window.devicePixelRatio || 1;
  const w = term.cols * CELL_W, h = term.rows * CELL_H;
  const fit = Math.max(0.05, Math.min(stage.clientWidth / w, stage.clientHeight / h));
  const cssW = Math.floor(w * fit), cssH = Math.floor(h * fit);
  const scale = Math.max(0.5, fit * dpr);
  const pw = Math.round(w * scale), ph = Math.round(h * scale);
  const resized = canvas.width !== pw || canvas.height !== ph;
  if (resized) { canvas.width = pw; canvas.height = ph; }
  canvas.style.width = cssW + 'px'; canvas.style.height = cssH + 'px';
  if (resized || !sigs || sigs.length !== term.rows) sigs = new Int32Array(term.rows).fill(0x7fffffff);
  draw(canvas.getContext('2d', { alpha: false }), scale, sigs);
}

function draw(ctx, s, sigs) {
  const buf = term.buffer.active;
  const cell = buf.getNullCell();
  const cw = CELL_W * s, ch = CELL_H * s;
  ctx.textBaseline = 'middle'; ctx.textAlign = 'center';
  const font = `${Math.max(6, Math.round(CELL_H * s * 0.78))}px "IBM Plex Mono", Menlo, Consolas, monospace`;
  for (let y = 0; y < term.rows; y++) {
    const line = buf.getLine(buf.viewportY + y);
    if (!line) continue;
    const sig = rowSig(line, cell);
    if (sigs[y] === sig) continue;
    sigs[y] = sig;
    const py = Math.round(y * ch), ph = Math.round((y + 1) * ch) - py;
    for (let x = 0; x < term.cols; x++) {
      line.getCell(x, cell);
      const px = Math.round(x * cw), pw = Math.round((x + 1) * cw) - px;
      let fg = cell.isFgRGB() ? cell.getFgColor() : cell.isFgPalette() ? PALETTE[cell.getFgColor()] : DEF_FG;
      let bg = cell.isBgRGB() ? cell.getBgColor() : cell.isBgPalette() ? PALETTE[cell.getBgColor()] : DEF_BG;
      if (cell.isInverse()) [fg, bg] = [bg, fg];
      const str = cell.getChars();
      glyph(ctx, s, font, str ? str.codePointAt(0) : 32, str, px, py, pw, ph, fg, bg, cell.isBold());
    }
  }
}

function glyph(ctx, s, font, cp, str, x, y, w, h, fg, bg, bold) {
  const fill = (c, ax, ay, aw, ah) => { ctx.fillStyle = hex(c); ctx.fillRect(ax, ay, aw, ah); };
  if (cp === 32 || cp === 0) { fill(bg, x, y, w, h); return; }
  if (cp === 0x2580) { const m = Math.round(h / 2); fill(fg, x, y, w, m); fill(bg, x, y + m, w, h - m); return; }
  if (cp === 0x2584) { const m = Math.round(h / 2); fill(bg, x, y, w, m); fill(fg, x, y + m, w, h - m); return; }
  if (cp === 0x2588) { fill(fg, x, y, w, h); return; }
  if (cp === 0x258c) { const m = Math.round(w / 2); fill(fg, x, y, m, h); fill(bg, x + m, y, w - m, h); return; }
  if (cp === 0x2590) { const m = Math.round(w / 2); fill(bg, x, y, m, h); fill(fg, x + m, y, w - m, h); return; }
  if (cp >= 0x2591 && cp <= 0x2593) { fill(mix(fg, bg, (cp - 0x2590) / 4), x, y, w, h); return; }
  if (QUAD[cp] !== undefined) { blocks(ctx, QUAD[cp], 2, 2, x, y, w, h, fg, bg); return; }
  if (cp >= 0x1fb00 && cp <= 0x1fb3b) {
    let v = cp - 0x1fb00 + 1; if (v >= 21) v++; if (v >= 42) v++;
    blocks(ctx, v, 2, 3, x, y, w, h, fg, bg); return;
  }
  if (cp >= 0x2800 && cp <= 0x28ff) { braille(ctx, cp - 0x2800, x, y, w, h, fg, bg); return; }
  if (BOX[cp]) { box(ctx, s, BOX[cp], x, y, w, h, fg, bg); return; }
  fill(bg, x, y, w, h);
  if (h < 5) { fill(mix(fg, bg, 0.5), x, y, w, h); return; }         // too small to read: a tone
  ctx.fillStyle = hex(fg);
  ctx.font = (bold ? 'bold ' : '') + font;
  ctx.fillText(str, x + w / 2, y + h / 2 + 1);
}
function blocks(ctx, bits, cols, rows, x, y, w, h, fg, bg) {
  for (let r = 0; r < rows; r++) {
    const y0 = y + Math.round(r * h / rows), y1 = y + Math.round((r + 1) * h / rows);
    for (let c = 0; c < cols; c++) {
      const x0 = x + Math.round(c * w / cols), x1 = x + Math.round((c + 1) * w / cols);
      ctx.fillStyle = hex(bits >> (r * cols + c) & 1 ? fg : bg);
      ctx.fillRect(x0, y0, x1 - x0, y1 - y0);
    }
  }
}
const BRAILLE = [[0, 0], [0, 1], [0, 2], [1, 0], [1, 1], [1, 2], [0, 3], [1, 3]];
function braille(ctx, bits, x, y, w, h, fg, bg) {
  ctx.fillStyle = hex(bg); ctx.fillRect(x, y, w, h);
  ctx.fillStyle = hex(fg);
  const d = Math.max(1, Math.min(w / 4, h / 8) * 1.3);
  for (let i = 0; i < 8; i++) if (bits >> i & 1) {
    const [cx, cy] = BRAILLE[i];
    ctx.fillRect(x + w * (cx * 2 + 1) / 4 - d / 2, y + h * (cy * 2 + 1) / 8 - d / 2, d, d);
  }
}
function box(ctx, s, arms, x, y, w, h, fg, bg) {
  ctx.fillStyle = hex(bg); ctx.fillRect(x, y, w, h);
  ctx.fillStyle = hex(fg);
  const t = Math.max(1, Math.round(s)), cx = x + Math.round(w / 2 - t / 2), cy = y + Math.round(h / 2 - t / 2);
  const seg = (k, off) => {
    if (arms[0] === k) ctx.fillRect(cx + off, y, t, cy - y + t);
    if (arms[2] === k) ctx.fillRect(cx + off, cy, t, y + h - cy);
    if (arms[3] === k) ctx.fillRect(x, cy + off, cx - x + t, t);
    if (arms[1] === k) ctx.fillRect(cx, cy + off, x + w - cx, t);
  };
  seg(1, 0);
  if (arms.includes(2)) for (const off of [-t, 0, t]) seg(2, off);
  if (arms.includes(3)) { seg(3, -t * 1.5); seg(3, t * 1.5); }
}

function makeTerm(cols, rows) {
  if (!term) {
    term = new exports.Terminal({ cols, rows, scrollback: 0, allowProposedApi: true,
                                  windowOptions: { setWinSizeChars: true } });
    // The board says its grid's size first in every stream (CSI 8 ; rows ; cols t),
    // and again when it turns.
    term.parser.registerCsiHandler({ final: 't' }, (p) => {
      if (p[0] === 8 && p[1] > 0 && p[2] > 0 && (p[1] !== term.rows || p[2] !== term.cols)) { term.resize(p[2], p[1]); sigs = null; }
      return true;
    });
  }
  term.resize(cols, rows);
  term.reset();
  sigs = null;
}

// ------------------------------------------------------------------ the link

let ws = null, live = null, retry = 0, retryT = 0, lastSent = '';
let stopped = false;               // signed out or taken elsewhere: wait for the visitor
let pendingLogin = null;           // typed while the socket was down

function note(text, opts = {}) {
  $('note').hidden = !text;
  $('say').textContent = text || '';
  $('say').className = opts.bad ? 'bad' : '';
  $('login').hidden = !opts.login;
  $('again').hidden = !opts.again;
}

function say(text) {
  lastSent = text;
  if (ws && ws.readyState === 1) ws.send(text);
}

function connect() {
  clearTimeout(retryT);
  if (ws && ws.readyState <= 1) return;
  stopped = false;
  const sock = ws = new WebSocket(`ws://${location.host}/ws`);
  sock.binaryType = 'arraybuffer';
  sock.onopen = () => {
    retry = 0;
    const t = localStorage.getItem(TOKEN);
    if (pendingLogin) { say(pendingLogin); pendingLogin = null; }
    else if (t) say('token ' + t);
    else note('Sign in with the board\'s terminal password (LINK, REMOTE tab). Asked once.', { login: true });
  };
  sock.onmessage = (e) => {
    if (sock !== ws) return;
    if (typeof e.data !== 'string') { live?.parser.push(new Uint8Array(e.data)); return; }
    const m = e.data;
    if (m.startsWith('console ')) { consoleLine(m.slice(8)); return; }
    if (m.startsWith('ok ')) {
      if (m === 'ok local') { local = true; $('gear').hidden = false; }
      localStorage.setItem(TOKEN, m.slice(3));
      note('Starting the screen...');
      fontsReady.then(() => say('term web ack' + (canInflate ? ' z' : '') + (fonts ? ' cells' : '')));
    } else if (m === 'no') {
      const tried = lastSent.startsWith('login ');
      localStorage.removeItem(TOKEN);
      note(tried ? 'Wrong password.' : 'The board\'s password changed: sign in again.', { login: true, bad: tried });
      $('pass').value = '';
    } else if (m === 'wait') {
      setTimeout(() => say(lastSent), 1100);
    } else if (m.startsWith('term: web begin')) {
      const g = m.match(/cols (\d+) rows (\d+)/);
      begin(+g[1], +g[2], / cells\b/.test(m));
    } else if (m === 'term: web taken') {
      stopped = true;
      note('Another browser is watching the board now.', { again: true });
    }
  };
  sock.onclose = () => {
    if (sock !== ws) return;
    end();
    if (stopped || !$('login').hidden) return;
    note('The board is not answering. Trying again...');
    retry = Math.min(retry + 1, 5);
    retryT = setTimeout(connect, retry * 1000);
  };
}

function begin(cols, rows, cells = false) {
  end();
  if (cells && fonts) cellsStart(cols, rows);
  else { cm = null; makeTerm(cols, rows); }
  fitSoon(300);
  const l = live = { zparts: [], chain: Promise.resolve() };
  const ack = () => { if (live === l) say('term ack'); };
  l.parser = new FrameParser((body, z) => {
    if (!body) { l.zparts = []; return; }                // bad CRC: drop; the next update repaints
    if (cm) {                                            // cells: an update's records, whole, then drawn
      if (body.length) { l.zparts.push(body); l.z = z; return; }
      const parts = l.zparts, wasZ = l.z; l.zparts = [];
      l.chain = l.chain.then(() => !parts.length ? null : wasZ ? inflate(parts)
        : new Blob(parts).arrayBuffer().then(b => new Uint8Array(b)))
        .then((raw) => { if (raw && cm) cellsApply(raw); }).catch(() => {}).then(() => { schedulePaint(); ack(); });
      return;
    }
    if (z) { l.zparts.push(body); return; }
    if (body.length === 0) {
      const parts = l.zparts; l.zparts = [];
      if (!parts.length) { schedulePaint(); ack(); return; }
      // Inflate in order, draw, then ask for the next.
      l.chain = l.chain.then(() => inflate(parts)).then((raw) => new Promise(r => term.write(raw, r)))
        .catch(() => {}).then(() => { schedulePaint(); ack(); });
      return;
    }
    term.write(body);
  });
  l.ping = setInterval(() => say('term ping'), 2000);
  note('');
  schedulePaint();
}

function end() {
  if (!live) return;
  clearInterval(live.ping);
  live = null;
}

// ------------------------------------------------------------------ input

function send(bytes) {
  if (!live) return;
  for (let i = 0; i < bytes.length; i += 48) {
    const hx = [...bytes.subarray(i, i + 48)].map(x => x.toString(16).padStart(2, '0')).join('');
    say('term in ' + hx);
  }
}
const KEYS = {
  ArrowUp: '\x1b[A', ArrowDown: '\x1b[B', ArrowRight: '\x1b[C', ArrowLeft: '\x1b[D',
  // A lone Esc waits on the board for a sequence that may follow; a no-op
  // report request after it makes it the key now, on any firmware.
  Escape: '\x1b\x1b[0n', Enter: '\r', Tab: '\t', Backspace: '\x7f', Delete: '\x1b[3~',
  F1: '\x1bOP', F2: '\x1bOQ', F3: '\x1bOR', F4: '\x1bOS', PageUp: '\x1b[5~', PageDown: '\x1b[6~',
};
function key(name) {
  const s = KEYS[name] ?? (name.length === 1 ? name : null);
  if (s != null) send(enc.encode(s));
  return s != null;
}

// The cell under a point of the canvas, 1-based as the mouse reports it.
function cellAt(clientX, clientY) {
  const r = canvas.getBoundingClientRect();
  const [cols, rows] = grid();
  const col = Math.min(cols, Math.max(1, Math.floor((clientX - r.left) / r.width * cols) + 1));
  const row = Math.min(rows, Math.max(1, Math.floor((clientY - r.top) / r.height * rows) + 1));
  return [col, row];
}

// A touch that stays put is a tap on its cell, as an SGR mouse press and
// release; one that slides up or down is the wheel, a step every two cells.
// A mouse is a finger: pressed, held moving and let go, so a slider or a
// game's stick follows it; the board makes the tap of a press that stayed put.
let touch = null;
canvas.addEventListener('pointerdown', (e) => {
  if (!live || (!term && !cm)) return;
  e.preventDefault();
  canvas.setPointerCapture?.(e.pointerId);
  touch = { id: e.pointerId, x: e.clientX, y: e.clientY, at: e.clientY, moved: false, mouse: e.pointerType === 'mouse' };
  if (touch.mouse) {
    const [col, row] = touch.cell = cellAt(e.clientX, e.clientY);
    send(enc.encode(`\x1b[<0;${col};${row}M`));
  }
});
canvas.addEventListener('pointermove', (e) => {
  if (!touch || e.pointerId !== touch.id) return;
  if (touch.mouse) {
    const [col, row] = cellAt(e.clientX, e.clientY);
    if (col !== touch.cell[0] || row !== touch.cell[1]) {
      touch.cell = [col, row];
      send(enc.encode(`\x1b[<32;${col};${row}M`));
    }
    return;
  }
  const stepPx = canvas.getBoundingClientRect().height / grid()[1] * 2;
  if (Math.hypot(e.clientX - touch.x, e.clientY - touch.y) > stepPx * 0.6) touch.moved = true;
  while (Math.abs(e.clientY - touch.at) >= stepPx) {
    const up = e.clientY > touch.at;                  // the finger down: what is above comes into view
    const [col, row] = cellAt(touch.x, touch.y);
    send(enc.encode(`\x1b[<${up ? 64 : 65};${col};${row}M`));
    touch.at += up ? stepPx : -stepPx;
  }
});
canvas.addEventListener('pointerup', (e) => {
  if (!touch || e.pointerId !== touch.id) return;
  const t = touch; touch = null;
  if (t.mouse) {
    const [col, row] = cellAt(e.clientX, e.clientY);
    if (live) send(enc.encode(`\x1b[<0;${col};${row}m`));
    return;
  }
  if (t.moved || !live) return;
  const [col, row] = cellAt(t.x, t.y);
  send(enc.encode(`\x1b[<0;${col};${row}M\x1b[<0;${col};${row}m`));
});
canvas.addEventListener('pointercancel', () => {
  if (touch?.mouse && live) send(enc.encode(`\x1b[<0;${touch.cell[0]};${touch.cell[1]}m`));
  touch = null;
});
canvas.addEventListener('wheel', (e) => {
  if (!live) return;
  e.preventDefault();
  const [col, row] = cellAt(e.clientX, e.clientY);
  send(enc.encode(`\x1b[<${e.deltaY < 0 ? 64 : 65};${col};${row}M`));
}, { passive: false });

// The bar's keys never take the focus, so the phone's keyboard stays down;
// the keyboard button is the one way to raise it.
const typer = $('type');
$('bar').addEventListener('pointerdown', (e) => e.preventDefault());
$('bar').addEventListener('click', (e) => {
  const b = e.target.closest('button');
  if (!b) return;
  if (b.id === 'kbd') {
    if (document.activeElement === typer) typer.blur(); else typer.focus();
    return;
  }
  if (b.dataset.key) key(b.dataset.key);         // the gear has its own handler
});
typer.addEventListener('focus', () => $('kbd').classList.add('on'));
typer.addEventListener('blur', () => $('kbd').classList.remove('on'));
typer.addEventListener('keydown', (e) => {
  if (e.key.length > 1 && key(e.key)) e.preventDefault();
});
typer.addEventListener('input', () => {
  if (typer.value) send(enc.encode(typer.value));
  typer.value = '';
});

// A computer's keyboard, whenever nothing else on the page wants it.
// A held arrow or letter repeats steadily from the start: the system's own
// repeat waits half a second after the first press, and a game felt that as
// move, stall, move. Letters wait longer first, so a tap while typing is one.
const held = new Map();
const stopHeld = (k) => { const t = held.get(k); if (t) { clearTimeout(t.t); held.delete(k); } };
document.addEventListener('keydown', (e) => {
  if (!live || e.ctrlKey || e.metaKey || e.altKey) return;
  if (e.target === typer || e.target === $('pass') || e.target.tagName === 'INPUT') return;
  if (e.repeat) {                                   // ours repeat instead; others as before
    if (held.has(e.key)) e.preventDefault();
    else if (key(e.key)) e.preventDefault();
    return;
  }
  if (!key(e.key)) return;
  e.preventDefault();
  const arrow = e.key.startsWith('Arrow'), letter = /^[a-z]$/i.test(e.key);
  if (!arrow && !letter) return;
  stopHeld(e.key);
  const wait = arrow ? 110 : 200, every = arrow ? 40 : 45, t = {};
  t.t = setTimeout(function tick() { key(e.key); t.t = setTimeout(tick, every); }, wait);
  held.set(e.key, t);
});
document.addEventListener('keyup', (e) => {
  stopHeld(e.key);
  if (e.key.length === 1) { stopHeld(e.key.toLowerCase()); stopHeld(e.key.toUpperCase()); }   // shift let go first
});
window.addEventListener('blur', () => { for (const k of [...held.keys()]) stopHeld(k); });

$('login').addEventListener('submit', (e) => {
  e.preventDefault();
  const p = $('pass').value;
  if (!p) return;
  $('pass').blur();
  note('Signing in...');
  if (ws && ws.readyState === 1) say('login ' + p);
  else { pendingLogin = 'login ' + p; connect(); }
});
$('again').addEventListener('click', () => { stopped = false; note('Connecting to the board...'); connect(); });

// A phone puts the page away without a word; coming back, look again at once.
document.addEventListener('visibilitychange', () => {
  if (document.visibilityState === 'visible' && !stopped && (!ws || ws.readyState > 1) && $('login').hidden) connect();
});
window.addEventListener('resize', () => { sigs = null; schedulePaint(); fitSoon(); });

// Ask for a grid that fills the window at the board's own cell size. A board
// whose screen is only a terminal (a Nano) takes it and regrids; one with
// glass keeps its own. ?fit=0 leaves the grid alone.
let FIT = new URLSearchParams(location.search).get('fit') !== '0' && localStorage.getItem('lakeshark-fit') !== '0';
let fitT = 0;
function fitSoon(ms = 600) {
  if (!FIT) return;
  clearTimeout(fitT);
  fitT = setTimeout(() => {
    const stage = $('stage');
    if (!live || (!term && !cm) || !stage.clientWidth) return;
    const cols = Math.max(40, Math.min(200, Math.floor(stage.clientWidth / CELL_W)));
    const rows = Math.max(12, Math.min(60, Math.floor(stage.clientHeight / CELL_H)));
    const [now_c, now_r] = grid();
    if (cols !== now_c || rows !== now_r) say(`term size ${cols} ${rows}`);
  }, ms);
}

connect();

// ------------------------------------------------------------------ settings

// Over USB (lsweb.py answers 'ok local') the board's own settings are a panel
// away: sound and voice, the mesh radio, the screen and the board. Each control
// is a console line; the board's replies come back as 'console ...' and set
// what the panel shows. Mute is always set on or off, never toggled.
let local = false;
const dev = { vol: null, mute: null, voice: null, svol: null, run: null, tx: null, listen: null, autotx: null,
              chans: {}, sendOn: 0, version: '', board: '', daylight: null, meshNote: '' };
let panelT = 0;
function cmd(line) { say('cmd ' + line); }
function refresh() {
  for (const c of ['vol', 'say voice', 'say vol', 'mesh', 'version']) cmd(c);
}
function consoleLine(l) {
  let m;
  if ((m = l.match(/^vol=(\d+)(?: mute=(\d))?/))) { dev.vol = +m[1]; if (m[2] !== undefined) dev.mute = m[2] === '1'; }
  else if ((m = l.match(/^mute=(\d)/))) dev.mute = m[1] === '1';
  else if ((m = l.match(/^voice=(\w+)/))) dev.voice = m[1];
  else if ((m = l.match(/speech volume=(\d+)%/))) dev.svol = +m[1];
  else if ((m = l.match(/^speech \S+ voice=(\w+) level=(\d+)%/))) { dev.voice = m[1]; dev.svol = +m[2]; }
  else if ((m = l.match(/^mesh: node \S+\s+transmit (\w+)/))) { dev.run = true; dev.tx = /^armed$/i.test(m[1]); }
  else if ((m = l.match(/^mesh: transmit (\w+)/))) { dev.tx = /^armed$/i.test(m[1]); }
  else if ((m = l.match(/^mesh: auto listen (on|off), auto transmit (on|off)/))) { dev.listen = m[1] === 'on'; dev.autotx = m[2] === 'on'; }
  else if ((m = l.match(/^mesh: channel (\d) (.*?)(\s+<- sending here)?$/))) {
    if (!/refused/.test(m[2])) dev.chans[m[1]] = m[2] === 'cleared' ? '' : m[2];
    if (m[3]) dev.sendOn = +m[1];
    if (/refused/.test(m[2])) dev.meshNote = 'Channel ' + m[1] + ': type #name, or a base64 key.';
  }
  else if ((m = l.match(/^mesh: sending on channel (\d)(.*)$/))) { dev.sendOn = +m[1]; dev.meshNote = m[2].trim(); }
  else if (/^mesh: (not running|stopped)/.test(l)) { dev.run = false; dev.tx = false; }
  else if (/^mesh: board has no LoRa radio/.test(l)) dev.noRadio = true;
  else if ((m = l.match(/^mesh: start (\S+)/))) { dev.run = m[1] === 'ESP_OK'; if (!dev.run) dev.meshNote = 'No mesh radio: ' + m[1]; }
  else if ((m = l.match(/^LakeShark (\S+).*board=(\S+)/))) { dev.version = m[1]; dev.board = m[2]; }
  else if ((m = l.match(/daylight (on|off)/))) dev.daylight = m[1] === 'on';
  else return;
  if (!$('panel').hidden) drawPanel();
}
function el(tag, attrs = {}, ...kids) {
  const e = document.createElement(tag);
  for (const [k, v] of Object.entries(attrs)) {
    if (k === 'on') e.addEventListener('click', v);
    else if (k === 'cls') e.className = v;
    else e.setAttribute(k, v);
  }
  for (const c of kids) e.append(c);
  return e;
}
function btn(label, act, on = false, cls = '') { return el('button', { on: act, cls: (on ? 'on ' : '') + cls }, label); }
const known = (v, t, f) => v === null ? '?' : v ? t : f;
function drawPanel() {
  const p = $('panel');
  const keep = document.activeElement && p.contains(document.activeElement) && document.activeElement.tagName === 'INPUT';
  if (keep) return;                                    // typing a channel: leave the fields alone
  p.replaceChildren();
  p.append(el('h2', {}, 'SOUND'));
  p.append(el('div', { cls: 'row' }, btn(dev.mute ? 'UNMUTE' : 'MUTE', () => { cmd(dev.mute ? 'mute off' : 'mute on'); setTimeout(() => cmd('vol'), 300); }, dev.mute === true),
    btn('VOL -', () => { cmd('vol -5'); setTimeout(() => cmd('vol'), 300); }),
    el('span', { cls: 'val' }, dev.vol === null ? '?' : dev.vol + '%'),
    btn('VOL +', () => { cmd('vol +5'); setTimeout(() => cmd('vol'), 300); })));
  p.append(el('h2', {}, 'VOICE'));
  p.append(el('div', { cls: 'row' }, ...['glitch', 'dark', 'female'].map(v => btn(v.toUpperCase(), () => cmd('say voice ' + v), dev.voice === v))));
  p.append(el('div', { cls: 'row' },
    btn('LEVEL -', () => cmd('say vol ' + Math.max(0, (dev.svol ?? 100) - 10))),
    el('span', { cls: 'val' }, dev.svol === null ? '?' : dev.svol + '%'),
    btn('LEVEL +', () => cmd('say vol ' + Math.min(100, (dev.svol ?? 100) + 10))),
    btn('TEST', () => cmd('say Voice check, one two.'))));
  p.append(el('h2', {}, 'MESH'));
  if (dev.noRadio) p.append(el('div', { cls: 'st' }, 'No LoRa radio on this board.'));
  else meshRows(p);
  p.append(el('h2', {}, 'SCREEN'));
  p.append(el('div', { cls: 'row' }, btn('FIT ' + (FIT ? 'ON' : 'OFF'), () => {
      FIT = !FIT; try { localStorage.setItem('lakeshark-fit', FIT ? '1' : '0'); } catch (e) {} if (FIT) fitSoon(100); drawPanel();
    }, FIT),
    btn('THEME', () => send(enc.encode('\x1b[20~'))),
    btn('DAYLIGHT', () => { cmd('tui daylight ' + (dev.daylight ? 'off' : 'on')); dev.daylight = !dev.daylight; drawPanel(); }, dev.daylight === true)));
  p.append(el('div', { cls: 'row' }, btn('SNAPSHOT', () => canvas.toBlob(b => {
    const a = el('a', { href: URL.createObjectURL(b), download: 'lakeshark-' + Date.now() + '.png' });
    a.click();
  }))));
  p.append(el('h2', {}, 'BOARD'));
  p.append(el('div', { cls: 'st' }, (dev.board || 'board') + '   ' + (dev.version || '')));
  p.append(el('div', { cls: 'row' }, btn('REBOOT', () => { if (confirm('Restart the board?')) cmd('reboot'); }, false, 'warn')));
}
function meshRows(p) {
  p.append(el('div', { cls: 'st' }, 'radio ' + known(dev.run, 'running', 'stopped') + '   transmit ' + known(dev.tx, 'ARMED', 'off') +
    '   listen at start ' + known(dev.listen, 'on', 'off')));
  p.append(el('div', { cls: 'row' }, btn(dev.run ? 'STOP' : 'START', () => { cmd(dev.run ? 'mesh stop' : 'mesh start'); setTimeout(() => cmd('mesh'), 600); }, dev.run === true),
    btn(dev.tx ? 'DISARM' : 'ARM TX', () => { cmd('mesh tx ' + (dev.tx ? 'off' : 'on')); setTimeout(() => cmd('mesh'), 400); }, dev.tx === true, dev.tx ? '' : 'warn'),
    btn('ADVERT', () => cmd('mesh advert'))));
  p.append(el('div', { cls: 'row' }, btn('AUTO LISTEN ' + known(dev.listen, 'ON', 'OFF'), () => { cmd('mesh auto listen ' + (dev.listen ? 'off' : 'on')); }, dev.listen === true)));
  for (const n of ['1', '2', '3']) {
    const input = el('input', { value: dev.chans[n] || '', placeholder: '#name or key', 'aria-label': 'Channel ' + n });
    p.append(el('div', { cls: 'row' }, el('span', { cls: 'lbl' }, 'CH' + n), input,
      btn('SET', () => { cmd('mesh chan ' + n + ' ' + (input.value.trim() || 'clear')); input.blur(); setTimeout(() => cmd('mesh'), 400); }),
      btn('SEND', () => { cmd('mesh send ' + n); setTimeout(() => cmd('mesh'), 400); }, dev.sendOn === +n)));
  }
  p.append(el('div', { cls: 'row' }, btn('SEND ON PUBLIC', () => { cmd('mesh send 0'); setTimeout(() => cmd('mesh'), 400); }, dev.sendOn === 0)));
  if (dev.meshNote) p.append(el('div', { cls: 'st' }, dev.meshNote));
}
$('gear').addEventListener('pointerdown', (e) => e.preventDefault());
$('gear').addEventListener('click', () => {
  const p = $('panel');
  p.hidden = !p.hidden;
  $('gear').classList.toggle('on', !p.hidden);
  clearInterval(panelT);
  if (!p.hidden) { refresh(); drawPanel(); panelT = setInterval(refresh, 4000); }
});
