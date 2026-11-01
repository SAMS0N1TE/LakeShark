// The web screen: the board's screen over a WebSocket from the board itself.
// The stream is the USB live screen's (ls_rterm.c): 'term web ack z' starts
// it, deflated updates come in frames each ending in an empty one, and each
// is answered 'term ack'; input uses 'term ev <hex>' after 'term caps' confirms
// ev1 support, with 'term in <hex>' retained for older firmware.
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
    // LSF1: 512 glyphs a font. LSF2 adds a u16 glyph count: box lines and
    // shapes after braille (ls_box.h).
    const magic = String.fromCharCode(raw[0], raw[1], raw[2], raw[3]);
    if (magic !== 'LSF1' && magic !== 'LSF2') return;
    const list = [];
    let at = 5, n = 512;
    if (magic === 'LSF2') { n = raw[5] | raw[6] << 8; at = 7; }
    for (let i = 0; i < raw[4]; i++) {
      const w = raw[at], h = raw[at + 1];
      at += 2;
      list.push({ w, h, n, cov: raw.subarray(at, at + n * w * h) });
      at += n * w * h;
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

const paintAcks = [];
function schedulePaint(done) {
  if (done) paintAcks.push(done);
  if (paintPending) return;
  paintPending = true;
  requestAnimationFrame(() => {
    paintPending = false; paint(); inputPainted();
    for (const done of paintAcks.splice(0)) done();
  });
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
let boardPixels = localStorage.getItem('lakeshark-board-pixels') !== '0';
let smoothLines = localStorage.getItem('lakeshark-smooth-lines') === '1';
let visualCaps = { pixels: false, vectors: false };
function requestVisuals() {
  if (cm && canInflate && (visualCaps.pixels || visualCaps.vectors))
    say(`term visual ${boardPixels && visualCaps.pixels ? 1 : 0} ${smoothLines && visualCaps.vectors ? 1 : 0}`);
}
function visualToggle(which) {
  if (which === 'pixels') boardPixels = !boardPixels;
  else smoothLines = !smoothLines;
  try { localStorage.setItem(which === 'pixels' ? 'lakeshark-board-pixels' : 'lakeshark-smooth-lines',
    (which === 'pixels' ? boardPixels : smoothLines) ? '1' : '0'); } catch (e) {}
  if (cm) cm.dirty = [0, 0, cm.cols, cm.rows];
  if (cm) cm.visualFull = true;
  requestVisuals(); schedulePaint(); drawPanel();
}
function cellsStart(cols, rows) {
  cm = { pal: new Uint32Array(16).fill(0xff000000), pal565: new Uint16Array(16) };
  cellsGrid(cols, rows, 0, CELL_W, CELL_H);
}
function cellsGrid(cols, rows, font, cw, ch) {
  const n = cols * rows;
  Object.assign(cm, { cols, rows, font, cw, ch, g: new Uint16Array(n).fill(32), a: new Uint8Array(n),
                      k: new Uint8Array(n), c1: new Uint16Array(n), c2: new Uint16Array(n) });
  cm.face = fonts[font];
  if (!cm.face || cm.face.w !== cw || cm.face.h !== ch)
    cm.face = fonts.find(f => f.w === cw && f.h === ch) || fonts[0];
  cm.img = new ImageData(cols * cw, rows * ch);
  cm.px = new Uint32Array(cm.img.data.buffer);
  cm.px.fill(0xff000000);
  cm.dirty = [0, 0, cols, rows];
  cm.picture = null; cm.coverage = new Map(); cm.lines = []; cm.lineCells = new Map();
  cm.visualDirty = new Set(); cm.visualFull = true; cm.visualPending = false;
}
const c565 = (v) => (((v >> 11 & 31) * 255 / 31) | 0) | (((v >> 5 & 63) * 255 / 63) | 0) << 8 |
                    (((v & 31) * 255 / 31) | 0) << 16 | 0xff000000;     // RGB565 to ImageData's ABGR
const pictureColors = Uint32Array.from({ length: 65536 }, (_, i) => c565(i));
function pictureMapping(p) {
  p.xs ||= new Uint16Array(p.w); p.ys ||= new Uint16Array(p.h);
  for (let x = 0; x < p.w; x++) p.xs[x] = (Math.floor(x * p.sw / p.w) + p.x0) % p.sw;
  for (let y = 0; y < p.h; y++) p.ys[y] = (Math.floor(y * p.sh / p.h) + p.y0) % p.sh;
}
function pictureDirty(p, first = 0, count = p.sh) {
  const left = Math.max(0, p.x / cm.cw), right = Math.min(cm.cols, (p.x + p.w) / cm.cw);
  for (let y = 0; y < p.h; y++) {
    if (p.ys[y] < first || p.ys[y] >= first + count) continue;
    const row = Math.floor((p.y + y) / cm.ch); if (row < 0 || row >= cm.rows) continue;
    for (let col = left; col < right; col++) cm.visualDirty.add(row * cm.cols + col);
  }
}
function pictureColumnDirty(p, first, count) {
  for (let x = 0; x < p.w; x++) {
    if (p.xs[x] < first || p.xs[x] >= first + count) continue;
    const col = Math.floor((p.x + x) / cm.cw); if (col < 0 || col >= cm.cols) continue;
    for (let row = Math.max(0, p.y / cm.ch); row < Math.min(cm.rows, (p.y + p.h) / cm.ch); row++) cm.visualDirty.add(row * cm.cols + col);
  }
}
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
      for (let i = 0; i < 16; i++) { cm.pal565[i] = v.getUint16(at + i * 2, true); cm.pal[i] = c565(cm.pal565[i]); }
      at += 32;
      repaint = true;
      cm.visualFull = true;
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
        cm.visualDirty.add(i);
        if (!repaint) cellDraw(i);
      }
    } else if (t === 0x49 && at + 21 <= raw.length) { // I: reset, source row patch, or ring origin
      const op = raw[at], x = v.getInt16(at + 1, true), y = v.getInt16(at + 3, true);
      const w = v.getUint16(at + 5, true), h = v.getUint16(at + 7, true);
      const row = v.getUint16(at + 9, true), n = v.getUint16(at + 11, true);
      const sw = v.getUint16(at + 13, true), sh = v.getUint16(at + 15, true);
      const x0 = v.getUint16(at + 17, true), y0 = v.getUint16(at + 19, true); at += 21;
      if (w * h > 4194304 || sw * sh > 4194304 ||
          x % cm.cw || y % cm.ch || w % cm.cw || h % cm.ch ||
          (sw && x0 >= sw) || (sh && y0 >= sh)) break;
      if (op === 0) {
        cm.picture = w && h && sw && sh ? { x, y, w, h, sw, sh, x0, y0, px: new Uint16Array(sw * sh),
          valid: new Uint8Array(sh), known: new Uint8Array(sw * sh), rowKnown: new Uint32Array(sh) } : null;
        if (cm.picture) pictureMapping(cm.picture);
        cm.visualFull = true;
        cm.coverage.clear();
      } else if (op === 1) {
        if (row + n > sh || at + sw * n * 2 > raw.length) break;
        const p = cm.picture;
        if (p && p.x === x && p.y === y && p.w === w && p.h === h && p.sw === sw && p.sh === sh) {
          for (let i = 0; i < sw * n; i++) p.px[row * sw + i] = v.getUint16(at + i * 2, true);
          p.valid.fill(1, row, row + n);
          p.known.fill(1, row * sw, (row + n) * sw); p.rowKnown.fill(sw, row, row + n);
          pictureDirty(p, row, n);
        }
        at += sw * n * 2;
      } else if (op === 2) {
        const p = cm.picture;
        if (p && p.x === x && p.y === y && p.w === w && p.h === h && p.sw === sw && p.sh === sh) {
          let shift = (p.y0 - y0 + sh) % sh; if (shift > sh / 2) shift -= sh;
          for (let i = 0; i < Math.abs(shift); i++) {
            const y = ((shift > 0 ? y0 : p.y0) + i) % sh;
            p.valid[y] = 0; p.rowKnown[y] = 0; p.known.fill(0, y * sw, (y + 1) * sw);
          }
          let dx = (x0 - p.x0 + sw) % sw; if (dx > sw / 2) dx -= sw;
          for (let i = 0; i < Math.abs(dx); i++) {
            const x = ((dx > 0 ? p.x0 : x0) + i) % sw;
            for (let y = 0; y < sh; y++) if (p.known[y * sw + x]) {
              p.known[y * sw + x] = 0; p.rowKnown[y]--; p.valid[y] = 0;
            }
          }
          p.x0 = x0; p.y0 = y0;
          pictureMapping(p);
          pictureDirty(p);
        }
      } else if (op === 3) {
        const p = cm.picture;
        if (p && p.sw === sw && p.sh === sh) {
          pictureDirty(p);
          for (const i of cm.lineCells.keys()) cm.visualDirty.add(i);
          cm.lines = []; cm.lineCells.clear(); // ordinary dots until a fresh V
          const moved = new Map(), dx = (x - p.x) / cm.cw, dy = (y - p.y) / cm.ch;
          const clipped = x < 0 || y < 0 || x + w > cm.cols * cm.cw || y + h > cm.rows * cm.ch;
          if (w === p.w && h === p.h) for (const [i, o] of cm.coverage) {
            const cx = i % cm.cols + dx, cy = Math.floor(i / cm.cols) + dy, j = cy * cm.cols + cx;
            if (cx >= 0 && cx < cm.cols && cy >= 0 && cy < cm.rows && cm.g[j] === o.glyph && (clipped || cm.k[j] === 2)) moved.set(j, o);
          }
          cm.coverage = moved;
          Object.assign(p, { x, y, w, h, x0, y0, xs: null, ys: null }); pictureMapping(p); pictureDirty(p);
        }
      } else if (op === 5) {
        if (at + 2 > raw.length) break;
        const ground = v.getUint16(at, true); at += 2;
        const dx = row << 16 >> 16, dy = n << 16 >> 16, p = cm.picture;
        if (Math.abs(dx) >= sw || Math.abs(dy) >= sh || x0 || y0) break;
        if (p && p.x === x && p.y === y && p.w === w && p.h === h && p.sw === sw && p.sh === sh) {
          for (let j = 0; j < sh; j++) {
            const y = dy < 0 ? sh - 1 - j : j, sy = y + dy;
            for (let i = 0; i < sw; i++) {
              const x = dx < 0 ? sw - 1 - i : i, sx = x + dx, dst = y * sw + x;
              const inside = sx >= 0 && sx < sw && sy >= 0 && sy < sh;
              p.px[dst] = inside ? p.px[sy * sw + sx] : ground;
              p.known[dst] = inside ? p.known[sy * sw + sx] : 1;
            }
            let known = 0; for (let x = 0; x < sw; x++) known += p.known[y * sw + x];
            p.rowKnown[y] = known; p.valid[y] = known === sw ? 1 : 0;
          }
          pictureDirty(p);
        }
      } else if (op === 6) {
        if (n !== 1 || row >= sh || at + 5 > raw.length) break;
        const left = v.getUint16(at, true), width = v.getUint16(at + 2, true), codec = raw[at + 4]; at += 5;
        if (!width || left + width > sw) break;
        // Validate the complete span before touching retained pixels.
        const start = at; let end = at, runs = 0;
        if (codec === 0) end += width * 2;
        else if (codec === 1) {
          if (at + 2 > raw.length) break;
          runs = v.getUint16(at, true); end += 2 + runs * 4;
          if (!runs || end > raw.length) break;
          let count = 0;
          for (let j = 0; j < runs; j++) { const len = v.getUint16(at + 2 + j * 4, true); if (!len) { count = -1; break; } count += len; }
          if (count !== width) break;
        } else if (codec === 2 || codec === 3) end += codec === 2 ? width : Math.ceil(width / 2);
        else break;
        if (end > raw.length) break;
        const p = cm.picture;
        if (p && p.x === x && p.y === y && p.w === w && p.h === h && p.sw === sw && p.sh === sh) {
          if (codec >= 2) {
            let valid = !!p.palette;
            for (let j = 0; valid && j < width; j++) {
              const index = codec === 2 ? raw[start + j] : raw[start + (j >> 1)] >> ((j & 1) * 4) & 15;
              if (index >= p.palette.length) valid = false;
            }
            if (!valid) break;
          }
          let dst = row * sw + left;
          const put = c => { p.px[dst] = c; if (!p.known[dst]) { p.known[dst] = 1; p.rowKnown[row]++; } dst++; };
          if (!codec) for (let j = 0; j < width; j++) put(v.getUint16(start + j * 2, true));
          else if (codec >= 2) for (let j = 0; j < width; j++)
            put(p.palette[codec === 2 ? raw[start + j] : raw[start + (j >> 1)] >> ((j & 1) * 4) & 15]);
          else for (let j = 0; j < runs; j++) {
            const len = v.getUint16(start + 2 + j * 4, true), c = v.getUint16(start + 4 + j * 4, true);
            for (let i = 0; i < len; i++) put(c);
          }
          p.valid[row] = p.rowKnown[row] === sw ? 1 : 0; pictureDirty(p, row, 1);
        }
        at = end;
      } else if (op === 7) {
        if (at + 2 > raw.length) break;
        const count = v.getUint16(at, true); at += 2;
        if (!count || count > 256 || at + count * 2 > raw.length) break;
        const p = cm.picture;
        if (p && p.x === x && p.y === y && p.w === w && p.h === h && p.sw === sw && p.sh === sh)
          p.palette = Uint16Array.from({ length: count }, (_, i) => v.getUint16(at + i * 2, true));
        at += count * 2;
      } else if (op === 4) {
        if (row + n > sw || at + sh * n * 2 > raw.length) break;
        const p = cm.picture;
        if (p && p.x === x && p.y === y && p.w === w && p.h === h && p.sw === sw && p.sh === sh) {
          for (let j = 0; j < n; j++) for (let y = 0; y < sh; y++) {
            const i = y * sw + row + j; p.px[i] = v.getUint16(at + (j * sh + y) * 2, true);
            if (!p.known[i]) { p.known[i] = 1; p.rowKnown[y]++; p.valid[y] = p.rowKnown[y] === sw ? 1 : 0; }
          }
          pictureColumnDirty(p, row, n);
        }
        at += sh * n * 2;
      } else break;
      cm.visualPending = true;
    } else if (t === 0x4f && at + 3 <= raw.length) { // O: exact glass coverage, packed nibbles
      const op = raw[at++], n = v.getUint16(at, true), size = Math.ceil(cm.cw * cm.ch / 2); at += 2;
      if (at + n * (5 + size) > raw.length) break;
      if (op === 0) { for (const i of cm.coverage.keys()) cm.visualDirty.add(i); cm.coverage.clear(); }
      for (let j = 0; j < n; j++) {
        const i = v.getUint16(at, true), halo = raw[at + 2], glyph = v.getUint16(at + 3, true); at += 5;
        if (i < cm.g.length) { cm.coverage.set(i, { halo, glyph, cov: raw.slice(at, at + size) }); cm.visualDirty.add(i); }
        at += size;
      }
      cm.visualPending = true;
    } else if (t === 0x56 && at + 2 <= raw.length) { // V: this frame's paths and owned dot bits
      const n = v.getUint16(at, true); at += 2;
      if (n > 512 || at + n * 10 + 2 > raw.length) break;
      const lines = [];
      for (let j = 0; j < n; j++, at += 10) lines.push({ x0: v.getUint16(at, true) / 4,
        y0: v.getUint16(at + 2, true) / 4, x1: v.getUint16(at + 4, true) / 4,
        y1: v.getUint16(at + 6, true) / 4, color: v.getUint16(at + 8, true) });
      const count = v.getUint16(at, true); at += 2;
      if (count > 1024 || at + count * 6 > raw.length) break;
      cm.lines = lines;
      for (const i of cm.lineCells.keys()) cm.visualDirty.add(i);
      cm.lineCells.clear();
      for (let j = 0; j < count; j++, at += 6) {
        const i = v.getUint16(at, true);
        if (i < cm.g.length) cm.lineCells.set(i, { glyph: v.getUint16(at + 2, true), attr: raw[at + 4], bits: raw[at + 5],
          color: cm.pal[raw[at + 4] & 15] });
        if (i < cm.g.length) cm.visualDirty.add(i);
      }
      cm.visualPending = true;
    } else if (t === 0x4c && at + 4 <= raw.length) { // L: sequence in this completed UI frame
      paintedSeq = v.getUint32(at, true); at += 4;
    } else break;                                      // unknown record: preceding cells still apply
  }
  if (repaint) { for (let i = 0; i < cm.g.length; i++) cellDraw(i); cm.dirty = [0, 0, cm.cols, cm.rows]; }
}

// Compose optional layers outside cellDraw: its glyph path remains shared with
// the text-font viewer. RGB565 blending below is the board's blend565 exactly.
function blend565(fg, bg, a) {
  if (!a) return bg; if (a >= 252) return fg;
  const ia = 255 - a;
  return (((((fg >> 11) * a + (bg >> 11) * ia) + 128) >> 8) << 11) |
    ((((((fg >> 5) & 63) * a + ((bg >> 5) & 63) * ia) + 128) >> 8) << 5) |
    ((((fg & 31) * a + (bg & 31) * ia) + 128) >> 8);
}
function lineMask(i) {
  const d = smoothLines && cm.lineCells.get(i);
  return d && d.glyph === cm.g[i] && d.attr === cm.a[i] && d.color === cm.pal[d.attr & 15] ? d.bits : 0;
}
function visualImage() {
  const p = boardPixels && cm.picture;
  if (!p && !smoothLines) return cm.img;
  if (!cm.composite || cm.composite.width !== cm.img.width || cm.composite.height !== cm.img.height) {
    cm.composite = new ImageData(cm.img.width, cm.img.height);
    cm.visualFull = true;
  }
  const px = new Uint32Array(cm.composite.data.buffer), W = cm.cols * cm.cw;
  const todo = cm.visualFull ? null : cm.visualDirty;
  if (todo && !todo.size) return cm.composite;
  if (!todo) cm.composite.data.set(cm.img.data);
  else for (const i of todo) {
    const x = i % cm.cols * cm.cw, y = Math.floor(i / cm.cols) * cm.ch;
    for (let row = 0; row < cm.ch; row++) {
      const at = (y + row) * W + x; px.set(cm.px.subarray(at, at + cm.cw), at);
    }
  }
  const finish = () => { cm.visualFull = false; cm.visualDirty.clear(); return cm.composite; };
  if (smoothLines) {
    const saved = cm.px, savedDirty = cm.dirty; cm.px = px;
    for (const [i] of cm.lineCells) {
      if (todo && !todo.has(i)) continue;
      const bits = lineMask(i); if (!bits) continue;
      const g = cm.g[i]; cm.g[i] = g & ~bits; cellDraw(i); cm.g[i] = g;
    }
    cm.px = saved; cm.dirty = savedDirty;
  }
  if (!p) return finish();
  // Cell-oriented iteration keeps one exact coverage/halo mask per glass cell.
  for (let cy = Math.max(0, p.y / cm.ch); cy < Math.min(cm.rows, (p.y + p.h) / cm.ch); cy++) for (let cx = Math.max(0, p.x / cm.cw); cx < Math.min(cm.cols, (p.x + p.w) / cm.cw); cx++) {
    const i = cy * cm.cols + cx, o = cm.coverage.get(i);
    if (todo && !todo.has(i)) continue;
    // A partly off-grid desk window retains the old C stream. Its image
    // marker and explicit glass coverage still identify the visible picture.
    const clipped = p.x < 0 || p.y < 0 || p.x + p.w > W || p.y + p.h > cm.rows * cm.ch;
    const kind = cm.k[i] || (clipped && cm.g[i] === 0xb0 ? 1 : clipped && o && o.glyph === cm.g[i] ? 2 : 0);
    if (kind !== 1 && kind !== 2) continue;
    const removed = lineMask(i), fg = cm.pal565[cm.a[i] & 15], bg = cm.pal565[cm.a[i] >> 4];
    if (kind === 2 && (!o || o.glyph !== cm.g[i])) {
      /* No mask for this glyph yet (the board sends them after the frames a
         pan or zoom answers): the picture shows through the cell's flat
         ground, so a ring or a mark never flashes on a black box. */
      const ground = c565(cm.c1[i]);
      for (let y = 0; y < cm.ch; y++) {
        const sy = p.ys[cy * cm.ch + y - p.y]; if (!p.rowKnown[sy]) continue;
        for (let x = 0; x < cm.cw; x++) {
          const at = (cy * cm.ch + y) * W + cx * cm.cw + x;
          if (px[at] !== ground) continue;
          const sx = p.xs[cx * cm.cw + x - p.x];
          if (p.known[sy * p.sw + sx]) px[at] = pictureColors[p.px[sy * p.sw + sx]];
        }
      }
      continue;
    }
    const coverage = (x, y) => {
      if (!o || x < 0 || y < 0 || x >= cm.cw || y >= cm.ch) return 0;
      if (removed) {
        const bit = cm.g[i] >= 0x2800 ? [[0, 3], [1, 4], [2, 5], [6, 7]][Math.floor(y * 4 / cm.ch)][Math.floor(x * 2 / cm.cw)]
          : Math.floor(y * 3 / cm.ch) * 2 + Math.floor(x * 2 / cm.cw);
        if (removed & 1 << bit) return 0;
      }
      const a = y * cm.cw + x; return o.cov[a >> 1] >> ((a & 1) * 4) & 15;
    };
    for (let y = 0; y < cm.ch; y++) {
      const sy = p.ys[cy * cm.ch + y - p.y]; if (!p.rowKnown[sy]) continue;
      for (let x = 0; x < cm.cw; x++) {
        const sx = p.xs[cx * cm.cw + x - p.x];
        if (!p.known[sy * p.sw + sx]) continue;
        let c = p.px[sy * p.sw + sx];
        if (kind === 2 && o) {
          const a = coverage(x, y); let near = 0;
          if (!a && o.halo) for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) near = Math.max(near, coverage(x + dx, y + dy));
          if (a) c = blend565(fg, c, a * 17); else if (near >= 6) c = blend565(bg, c, 200);
        }
        px[(cy * cm.ch + y) * W + cx * cm.cw + x] = pictureColors[c];
      }
    }
  }
  return finish();
}
function paintVectors(ctx) {
  if (!smoothLines || !cm.lines.length) return;
  for (const color of new Set(cm.lines.map(l => l.color))) {
    ctx.save(); ctx.beginPath();
    for (const [i, d] of cm.lineCells) if (lineMask(i) && (c565(color) >>> 0) === cm.pal[d.attr & 15]) {
      const rows = d.glyph >= 0x2800 ? 4 : 3;
      for (let y = 0; y < rows; y++) for (let x = 0; x < 2; x++) {
        const bit = rows === 4 ? [[0, 3], [1, 4], [2, 5], [6, 7]][y][x] : y * 2 + x;
        if (!(d.bits & 1 << bit)) continue;
        const x0 = Math.floor(x * cm.cw / 2), x1 = Math.floor((x + 1) * cm.cw / 2);
        const y0 = Math.floor(y * cm.ch / rows), y1 = Math.floor((y + 1) * cm.ch / rows);
        ctx.rect(i % cm.cols * cm.cw + x0, Math.floor(i / cm.cols) * cm.ch + y0, x1 - x0, y1 - y0);
      }
    }
    ctx.clip(); ctx.strokeStyle = hex((color >> 11) * 255 / 31 << 16 | (color >> 5 & 63) * 255 / 63 << 8 | (color & 31) * 255 / 31);
    ctx.lineWidth = 1; ctx.lineCap = 'round'; ctx.beginPath();
    for (const l of cm.lines) if (l.color === color) { ctx.moveTo(l.x0, l.y0); ctx.lineTo(l.x1, l.y1); }
    ctx.stroke();
    // Canvas may discard zero-length strokes; isolated board dots survive.
    ctx.fillStyle = ctx.strokeStyle;
    for (const l of cm.lines) if (l.color === color && l.x0 === l.x1 && l.y0 === l.y1) {
      ctx.beginPath(); ctx.arc(l.x0, l.y0, 0.5, 0, Math.PI * 2); ctx.fill();
    }
    ctx.restore();
  }
}
// Box lines U+2500-257F and the desk's shapes, in the atlas after braille
// (ls_box.h: 512 + cp - 0x2500, then the shapes in LS_BOX_SHAPES order).
const BOX_SHAPES = [0x25b2, 0x25bc, 0x25c0, 0x25b6, 0x2713, 0x2022, 0x2715, 0x25a1, 0x29c9,
  // edge-aligned frame lines: left, right, top, bottom; corner dots; two-edge turns
  0x258f, 0x2595, 0x2594, 0x2581, 0x2598, 0x259d, 0x2596, 0x2597, 0x259b, 0x259c, 0x2599, 0x259f];
function boxGlyph(code) {
  if (code >= 0x2500 && code <= 0x257f) return 512 + code - 0x2500;
  const s = BOX_SHAPES.indexOf(code);
  return s >= 0 ? 640 + s : 63;
}

// ------------------------------------------------------------------ PC font
// "PC font for text" (gear panel, per viewer, off by default). With it on,
// the cells that are text (letters, digits, punctuation, Cyrillic) are drawn
// by the browser in DejaVu Sans Mono, the face the board's bitmaps were made
// from (bench/tools/gen_small_font.py), at the board's pixel size and baseline
// in the same cell, and blended into fg/bg exactly as the atlas is. Blocks,
// sextants, traces, braille, box lines, desk frames and shapes, image and
// glass cells stay the board's own glyphs. Off draws what it always drew.
// Fonts: dejavu-mono.woff2 and -bold (DEJAVU-LICENSE.txt), subsets of DejaVu
// Sans Mono 2.37: pyftsubset DejaVuSansMono.ttf --flavor=woff2 --layout-features=''
// --name-IDs='*' --unicodes=U+0020-007E,U+00A0-00FF,U+0400-045F,U+0490-0491
const PCFONT_KEY = 'lakeshark-pcfont';
const PC_FAMILY = '"LS DejaVu Mono", "DejaVu Sans Mono", Menlo, "Bitstream Vera Sans Mono", Consolas, monospace';
// Cell size: [px, baseline up from the cell bottom, bold] (ls_font_mono_*.c).
const PC_SIZE = { '7x13': [11, 3], '8x14': [13, 3], '9x16': [14, 4], '10x17': [16, 4], '15x26': [24, 6, true] };
let pcFont = false, pcReady = false, pcLoad = null, pcFace = null, pcGamma = 1;
try { pcFont = localStorage.getItem(PCFONT_KEY) === '1'; } catch (e) { /* off */ }
// The Unicode a cell byte stands for, or 0 where the byte is not text. The
// board folds Latin-1 to ASCII before it reaches a cell (tui_core.c), and
// keeps Cyrillic in one byte a letter (tui_cyr.h): a letter drawn like a
// Latin one is that Latin byte, the rest are these 55 slots, in slot order.
const CYR_SLOT_CP = [0x411, 0x413, 0x490, 0x414, 0x404, 0x416, 0x417, 0x418, 0x407, 0x419,
  0x41b, 0x41f, 0x423, 0x424, 0x426, 0x427, 0x428, 0x429, 0x42c, 0x42e,
  0x42f, 0x431, 0x432, 0x433, 0x491, 0x434, 0x454, 0x436, 0x437, 0x438,
  0x456, 0x457, 0x439, 0x43a, 0x43b, 0x43c, 0x43d, 0x43f, 0x442, 0x444,
  0x446, 0x447, 0x448, 0x449, 0x44c, 0x44e, 0x44f, 0x401, 0x42a, 0x42b,
  0x42d, 0x451, 0x44a, 0x44b, 0x44d];
const CYR_SLOT_BYTES = [[0x01, 0x08], [0x0b, 0x0c], [0x0e, 0x1a], [0x1c, 0x1f], [0x94, 0x9f], [0xa8, 0xaf], [0xb1, 0xb8]];
const CELL_CP = (() => {
  const t = new Uint16Array(256);
  for (let c = 0x21; c < 0x7f; c++) t[c] = c;
  let s = 0;
  for (const [a, b] of CYR_SLOT_BYTES) for (let c = a; c <= b; c++) t[c] = CYR_SLOT_CP[s++];
  return t;
})();
function pcFontStart() {
  if (pcLoad) return pcLoad;
  pcLoad = (async () => {
    for (const [file, weight] of [['dejavu-mono.woff2', 'normal'], ['dejavu-mono-bold.woff2', 'bold']]) {
      try {
        const r = await fetch(file);
        if (!r.ok) continue;
        const f = new FontFace('LS DejaVu Mono', await r.arrayBuffer(), { weight });
        document.fonts.add(await f.load());
      } catch (e) { /* the local fonts after it in PC_FAMILY */ }
    }
    pcReady = true; pcFace = null;
    cellsRepaint();
  })();
  return pcLoad;
}
// The text glyphs at one cell size, rasterized once as coverage (0-255).
function pcFaceFor(cw, ch) {
  if (pcFace && pcFace.cw === cw && pcFace.ch === ch) return pcFace;
  const spec = PC_SIZE[cw + 'x' + ch];
  if (!pcReady || !spec) return null;
  const [size, base, bold] = spec, W = cw * 16;
  const g = Object.assign(document.createElement('canvas'), { width: W, height: ch * 16 })
    .getContext('2d', { willReadFrequently: true });       // transparent: greyscale coverage, no LCD fringes
  g.font = `${bold ? 'bold ' : ''}${size}px ${PC_FAMILY}`;
  g.textBaseline = 'alphabetic'; g.textAlign = 'left'; g.fillStyle = '#fff';
  const dx = Math.round((cw - g.measureText('M').width) / 2);   // centred, stems on whole pixels
  for (let c = 0; c < 256; c++) {
    if (!CELL_CP[c]) continue;
    const x0 = (c & 15) * cw, y0 = (c >> 4) * ch;
    g.save(); g.beginPath(); g.rect(x0, y0, cw, ch); g.clip();
    g.fillText(String.fromCodePoint(CELL_CP[c]), x0 + dx, y0 + ch - base);
    g.restore();
  }
  const data = g.getImageData(0, 0, W, ch * 16).data, cov = new Uint8Array(256 * cw * ch);
  for (let c = 0; c < 256; c++) {
    if (!CELL_CP[c]) continue;
    const x0 = (c & 15) * cw, y0 = (c >> 4) * ch;
    for (let y = 0; y < ch; y++)
      for (let x = 0; x < cw; x++) cov[(c * ch + y) * cw + x] = data[((y0 + y) * W + x0 + x) * 4 + 3];
  }
  // Same weight as the glass: a browser inks small light-on-dark text heavier
  // than the board's FreeType light hinting did, so a gamma on the coverage
  // brings the text glyphs' total ink to the atlas's own at this size.
  const atlas = cm && cm.face && cm.face.w === cw && cm.face.h === ch ? cm.face.cov : null;
  if (atlas) {
    let want = 0;
    const hist = new Float64Array(256);
    for (let c = 0; c < 256; c++) {
      if (!CELL_CP[c]) continue;
      for (let k = c * cw * ch, e = k + cw * ch; k < e; k++) { want += atlas[k]; hist[cov[k]]++; }
    }
    const ink = (gam) => { let s = 0; for (let v = 1; v < 256; v++) s += hist[v] * 255 * Math.pow(v / 255, gam); return s; };
    let lo = 0.4, hi = 4;
    for (let n = 0; n < 30; n++) { const m = (lo + hi) / 2; if (ink(m) > want) lo = m; else hi = m; }
    const gam = (lo + hi) / 2, lut = new Uint8Array(256);
    for (let v = 0; v < 256; v++) lut[v] = Math.round(255 * Math.pow(v / 255, gam));
    for (let k = 0; k < cov.length; k++) cov[k] = lut[cov[k]];
    pcGamma = gam;
  }
  return (pcFace = { cw, ch, cov });
}
function cellsRepaint() {
  if (!cm || !cm.face) return;
  for (let i = 0; i < cm.g.length; i++) cellDraw(i);
  cm.dirty = [0, 0, cm.cols, cm.rows];
  schedulePaint();
}
function pcFontSet(on) {
  pcFont = on;
  try { localStorage.setItem(PCFONT_KEY, on ? '1' : '0'); } catch (e) { /* this visit only */ }
  if (on && !pcReady) pcFontStart(); else cellsRepaint();
}
function pcFontRow() {
  return el('div', { cls: 'row' }, btn('PC FONT FOR TEXT ' + (pcFont ? 'ON' : 'OFF'), () => { pcFontSet(!pcFont); drawPanel(); }, pcFont));
}
if (pcFont) pcFontStart();

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
  let gi = code < 256 ? code : code >= 0x2800 && code <= 0x28ff ? 256 + code - 0x2800 : boxGlyph(code);
  if (face && gi >= face.n) gi = 63;
  if (!face || face.w !== cw || face.h !== ch) {       // no glyphs at this size: the ground
    for (let y = 0; y < ch; y++) px.fill(bg, (y0 + y) * W + x0, (y0 + y) * W + x0 + cw);
    return;
  }
  let cov = face.cov, base = gi * cw * ch;
  if (pcFont && code < 256 && CELL_CP[code]) {
    const pc = pcFaceFor(cw, ch);
    if (pc) { cov = pc.cov; base = code * cw * ch; }
  }
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
  const fit = denseDesktop() ? 1 / (window.devicePixelRatio || 1)
    : Math.max(0.05, Math.min(stage.clientWidth / w, stage.clientHeight / h));
  canvas.style.width = w * fit + 'px';
  canvas.style.height = h * fit + 'px';
  canvas.style.imageRendering = denseDesktop() || fit >= 1 ? 'pixelated' : 'auto';
  // Flex centring may land on half a device pixel. Align the image's origin
  // as well as its size so a DPR of 1.5 still maps every bitmap pixel exactly.
  canvas.style.transform = '';
  if (denseDesktop()) {
    const dpr = window.devicePixelRatio || 1, r = canvas.getBoundingClientRect();
    canvas.style.transform = `translate(${(Math.round(r.left * dpr) - r.left * dpr) / dpr}px, ${(Math.round(r.top * dpr) - r.top * dpr) / dpr}px)`;
  }
  const d = cm.dirty;
  if (!d && !cm.visualPending) return;
  // Full glyph redraws (including another viewer's text-font toggle) refresh
  // this cache without any changes to cellDraw's glyph implementation.
  if (d && d[0] === 0 && d[1] === 0 && d[2] === cm.cols && d[3] === cm.rows) cm.visualFull = true;
  cm.dirty = null; cm.visualPending = false;
  const ctx = canvas.getContext('2d', { alpha: false });
  if ((boardPixels && cm.picture) || smoothLines) {
    ctx.putImageData(visualImage(), 0, 0); paintVectors(ctx);
  } else if (d) ctx.putImageData(cm.img, 0, 0, d[0] * cm.cw, d[1] * cm.ch,
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
  // LakeShark Studio's run panel serves this page with its own socket (LS_WS_PATH).
  const sock = ws = new WebSocket(`ws://${location.host}${window.LS_WS_PATH || '/ws'}`);
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
    if (sizeReply(m)) return;
    if (/^(console )?term: caps /.test(m)) {
      const caps = m.split(/\s+/);
      if (live) desktopInput = caps.includes('ev1');
      visualCaps = { pixels: caps.includes('pix1'), vectors: caps.includes('vec1') };
      requestVisuals();
      // With a mouse and a keyboard the on-screen keys are in the way; a phone keeps them.
      if (desktopInput && matchMedia('(pointer: fine)').matches) {
        for (const b of $('bar').querySelectorAll('button[data-key], #kbd')) b.hidden = true;
        fitSoon();
      }
      schedulePaint();
      return;
    }
    if (m.startsWith('console ')) { consoleLine(m.slice(8)); return; }
    if (m.startsWith('ok ')) {
      if (m === 'ok local') local = true;
      $('gear').hidden = false;
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
    note((window.LS_WS_PATH ? 'The simulator' : 'The board') + ' is not answering. Trying again...');
    retry = Math.min(retry + 1, 5);
    retryT = setTimeout(connect, retry * 1000);
  };
}

function begin(cols, rows, cells = false) {
  end();
  admittedCells = Infinity; requestedSize = null;
  if (cells && fonts) cellsStart(cols, rows);
  else { cm = null; makeTerm(cols, rows); }
  fitSoon(300);
  const l = live = { zparts: [], chain: Promise.resolve() };
  const ack = () => { if (live === l) say('term ack'); };
  l.parser = new FrameParser((body, z) => {
    if (!body) { l.zparts = []; l.bad = true; return; }                // bad CRC: drop; the next update repaints
    if (cm) {                                            // cells: an update's records, whole, then drawn
      if (body.length) { l.zparts.push(body); l.z = z; return; }
      if (l.bad) { l.bad = false; l.zparts = []; say('term redraw'); schedulePaint(ack); return; }
      const parts = l.zparts, wasZ = l.z; l.zparts = [];
      l.chain = l.chain.then(() => !parts.length ? null : wasZ ? inflate(parts)
        : new Blob(parts).arrayBuffer().then(b => new Uint8Array(b)))
        .then((raw) => { if (live === l && raw && cm) cellsApply(raw); }).catch(() => { if (live === l) say('term redraw'); }).then(() => { if (live === l) schedulePaint(ack); });
      return;
    }
    if (z) { l.zparts.push(body); return; }
    if (body.length === 0) {
      const parts = l.zparts; l.zparts = [];
      if (!parts.length) { schedulePaint(ack); return; }
      // Inflate in order, draw, then ask for the next.
      l.chain = l.chain.then(() => inflate(parts)).then((raw) => new Promise(r => term.write(raw, r)))
        .catch(() => {}).then(() => { if (live === l) schedulePaint(ack); });
      return;
    }
    term.write(body);
  });
  say('term caps');
  l.ping = setInterval(() => {
    say('term ping');
    reportLatency();
  }, 2000);
  note('');
  schedulePaint();
}

function end() {
  if (!live) return;
  releaseInput();
  desktopInput = false;
  clearTimeout(moveTimer); pendingMove = null; lastMoveAt = -Infinity;
  eventTimes.clear(); latencySamples.length = 0; paintedSeq = 0; sessionSeq0 = 0;
  if (latencyStatus) latencyStatus.textContent = '';
  clearInterval(live.ping);
  live = null;
}

// ------------------------------------------------------------------ input

// Version 1 is byte-for-byte ls_input_ev.h (24 bytes, little-endian).
let desktopInput = false, inputSeq = Date.now() >>> 0;
let paintedSeq = 0, sessionSeq0 = 0, pendingMove = null, moveTimer = 0, lastMoveAt = -Infinity;
const MOD_KEYS = new Set([21, 22, 24, 37]);   // Alt, Ctrl, Meta, Shift
let lastPointerCell = null, latencyStatus = null;
const lastClicks = new Map();
const eventTimes = new Map(), latencySamples = [], pressedKeys = new Map();
const inputMods = e => (e.shiftKey ? 1 : 0) | (e.altKey ? 2 : 0) | (e.ctrlKey ? 4 : 0) | (e.metaKey ? 8 : 0);
// DOM buttons: left=1 right=2 middle=4; contract: left=1 middle=2 right=4.
const inputButtons = b => (b & 1) | ((b & 4) >> 1) | ((b & 2) << 1);
function encodeInput(e) {
  const b = new Uint8Array(24), v = new DataView(b.buffer);
  b.set([1, e.kind, 0, e.mods || 0, e.button || 0, e.buttons || 0, e.clicks || 0]);
  for (const [off, field] of [[7, 'col'], [9, 'row'], [11, 'px'], [13, 'py'], [15, 'wheel']]) v.setInt16(off, e[field] || 0, true);
  v.setUint16(17, e.key || 0, true); b[19] = e.ch || 0; v.setUint32(20, e.seq, true);
  return [...b].map(x => x.toString(16).padStart(2, '0')).join('');
}
function emitInputs(events) {
  if (!live || !desktopInput) return;
  // Ten records fit the board's 512-byte console buffer, including command/CR.
  for (let i = 0; i < events.length; i += 10) say('term ev ' + events.slice(i, i + 10).map(encodeInput).join(''));
}
function flushMove() {
  clearTimeout(moveTimer); moveTimer = 0;
  if (pendingMove) { emitInputs([pendingMove]); pendingMove = null; }
}
function inputEvent(e) {
  if (!live || !desktopInput) return;
  const now = performance.now();
  e.seq = inputSeq = (inputSeq + 1) >>> 0 || 1;
  if (!sessionSeq0) sessionSeq0 = e.seq;
  // Time only what normally changes the screen: a key press (not a modifier),
  // a button going down or up, a wheel step. Hover and key-up do not.
  if ((e.kind === 0 && !MOD_KEYS.has(e.key)) || e.kind === 3 || e.kind === 4 || e.kind === 5) eventTimes.set(e.seq, now);
  // Bound unacknowledged samples even when transport/display stops responding.
  for (const [seq, at] of eventTimes) {
    if (eventTimes.size <= 512 && now - at <= 10000) break;
    eventTimes.delete(seq);
  }
  if (e.kind === 2 && now - lastMoveAt < 4) {
    if (pendingMove) eventTimes.delete(pendingMove.seq); // superseded, never sent
    pendingMove = e;
    if (!moveTimer) moveTimer = setTimeout(() => { flushMove(); lastMoveAt = performance.now(); }, 4 - (now - lastMoveAt));
    return;
  }
  const batch = pendingMove ? [pendingMove, e] : [e];
  clearTimeout(moveTimer); moveTimer = 0; pendingMove = null;
  emitInputs(batch);
  lastMoveAt = e.kind === 2 ? now : -Infinity;
}
function releaseInput() {
  if (desktopInput && live) inputEvent({ kind: 6 });
  pressedKeys.clear(); lastPointerCell = null; lastClicks.clear();
}
function inputPainted() {
  if (!paintedSeq) return;
  // A stamp already in flight at reconnect cannot acknowledge this session's
  // events. Only a sequence we actually sent can complete a sample window.
  if (!sessionSeq0 || ((paintedSeq - sessionSeq0) | 0) < 0 || ((inputSeq - paintedSeq) | 0) < 0) { paintedSeq = 0; return; }
  const now = performance.now();
  for (const [seq, at] of eventTimes) {
    if (((paintedSeq - seq) | 0) >= 0) {
      // Over a second means that input changed nothing and a later, unrelated
      // update carried its number: no sample.
      if (now - at < 1000) latencySamples.push({ at: now, ms: now - at });
      eventTimes.delete(seq);
    }
  }
  if (latencySamples.length > 64) latencySamples.splice(0, latencySamples.length - 64);
  paintedSeq = 0;
  showLatency();
}
function latencyPercentiles() {
  const now = performance.now();
  while (latencySamples.length && now - latencySamples[0].at >= 10000) latencySamples.shift();
  const a = latencySamples.map(x => x.ms).sort((a, b) => a - b);
  return a.length ? [Math.round(a[Math.ceil(a.length * .5) - 1]), Math.round(a[Math.ceil(a.length * .95) - 1])] : null;
}
function showLatency() {
  const p = latencyPercentiles();
  if (!latencyStatus && p) {
    latencyStatus = document.createElement('output');
    latencyStatus.setAttribute('aria-label', 'Event to canvas latency');
    $('bar').appendChild(latencyStatus); // telemetry text; no drawing or style changes
  }
  if (latencyStatus) latencyStatus.textContent = p ? `p50 ${p[0]} / p95 ${p[1]} ms` : '';
  return p;
}
function reportLatency() {
  const p = showLatency();
  if (desktopInput && p) say(`term lat ${p[0]} ${p[1]}`);
}
const DESK_KEYS = { ArrowUp: 2, ArrowDown: 3, ArrowLeft: 4, ArrowRight: 5,
  Enter: 6, Escape: 7, Tab: 8, Backspace: 9, Alt: 21, AltGraph: 21, Control: 22, Meta: 24,
  F12: 29, Home: 30, End: 31, PageUp: 32, PageDown: 33, Insert: 34, Delete: 35,
  ContextMenu: 36, Shift: 37 };
function desktopKey(e) {
  if (DESK_KEYS[e.key]) return { key: DESK_KEYS[e.key] };
  if (/^F([1-9]|10|11)$/.test(e.key)) return { key: 9 + Number(e.key.slice(1)) };
  if (e.key.length === 1 && e.key.charCodeAt(0) >= 32 && e.key.charCodeAt(0) <= 255) return { key: 1, ch: e.key.charCodeAt(0) };
  // Some Ctrl/Alt combinations supply an unidentified/control key, but a code.
  if (/^Key[A-Z]$/.test(e.code)) return { key: 1, ch: e.code.charCodeAt(3) + (e.shiftKey ? 0 : 32) };
  if (/^Digit[0-9]$/.test(e.code)) return { key: 1, ch: e.code.charCodeAt(5) };
  const punct = { Backquote:'`~', Minus:'-_', Equal:'=+', BracketLeft:'[{', BracketRight:']}',
    Backslash:'\\|', Semicolon:';:', Quote:"'\"", Comma:',<', Period:'.>', Slash:'/?', Space:'  ' }[e.code];
  if (punct) return { key: 1, ch: punct.charCodeAt(e.shiftKey ? 1 : 0) };
  return null;
}
function desktopKeyboard(e, up) {
  if (!live || e.isComposing) return;
  const id = e.code || e.key;
  if (e.target !== typer && (e.target.tagName === 'INPUT' || e.target.tagName === 'TEXTAREA') && !pressedKeys.has(id)) return;
  const k = up ? pressedKeys.get(id) : desktopKey(e);
  if (!k) return;
  e.preventDefault(); // includes Tab, F1-F12, Ctrl+`, Ctrl+F4, Alt letters, Menu
  if (up) pressedKeys.delete(id); else pressedKeys.set(id, k);
  inputEvent({ ...k, kind: up ? 1 : 0, mods: inputMods(e), clicks: up || e.repeat ? 0 : 1 });
}
function pointerPosition(e) {
  const [c, r] = cellAt(e.clientX, e.clientY), [cols, rows] = grid();
  const box = canvas.getBoundingClientRect(), cw = cm ? cm.cw : CELL_W, ch = cm ? cm.ch : CELL_H;
  return { col: c - 1, row: r - 1,
    px: Math.max(0, Math.min(cols * cw - 1, Math.floor((e.clientX - box.left) / box.width * cols * cw))),
    py: Math.max(0, Math.min(rows * ch - 1, Math.floor((e.clientY - box.top) / box.height * rows * ch))) };
}
function desktopPointer(e, kind) {
  if (!live) return;
  const p = pointerPosition(e);
  if (kind === 2 && lastPointerCell && p.col === lastPointerCell.col && p.row === lastPointerCell.row) return;
  lastPointerCell = p;
  const button = e.button === 1 ? 1 : e.button === 2 ? 2 : 0;
  let clicks = 1;
  if (kind === 3) {
    const now = performance.now();
    const lastClick = lastClicks.get(button);
    if (lastClick && lastClick.button === button && lastClick.col === p.col && lastClick.row === p.row && now - lastClick.at <= 400) {
      clicks = 2; lastClicks.delete(button);
    } else lastClicks.set(button, { ...p, button, at: now });
  }
  inputEvent({ ...p, kind, button, buttons: inputButtons(e.buttons), mods: inputMods(e), clicks });
}

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
  if (desktopInput) {
    const k = desktopKey({ key: name, code: '' });
    if (!k) return false;
    inputEvent({ kind: 0, key: k.key, ch: k.ch, clicks: 1 });
    inputEvent({ kind: 1, key: k.key, ch: k.ch });
    return true;
  }
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
  // preventDefault on pointerdown also suppresses the compatibility mousedown/
  // mouseup events, so the desktop takes its button edges from pointer events.
  if (desktopInput) { canvas.setPointerCapture?.(e.pointerId); desktopPointer(e, 3); return; }
  if (e.button === 2) { key('Escape'); return; }
  canvas.setPointerCapture?.(e.pointerId);
  touch = { id: e.pointerId, x: e.clientX, y: e.clientY, at: e.clientY, moved: false, mouse: e.pointerType === 'mouse' };
  if (touch.mouse) {
    const [col, row] = touch.cell = cellAt(e.clientX, e.clientY);
    send(enc.encode(`\x1b[<0;${col};${row}M`));
  }
});
canvas.addEventListener('pointermove', (e) => {
  if (desktopInput) { desktopPointer(e, 2); return; }
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
  if (desktopInput) { desktopPointer(e, 4); return; }
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
canvas.addEventListener('contextmenu', e => e.preventDefault());
canvas.addEventListener('pointerleave', () => { if (desktopInput) releaseInput(); });
canvas.addEventListener('pointercancel', () => {
  if (desktopInput) { releaseInput(); return; }
  if (touch?.mouse && live) send(enc.encode(`\x1b[<0;${touch.cell[0]};${touch.cell[1]}m`));
  touch = null;
});
canvas.addEventListener('wheel', (e) => {
  if (!live) return;
  e.preventDefault();
  if (desktopInput) {
    const p = pointerPosition(e);
    const unit = e.deltaMode ? 1 : 100; // line/page units, or 100 px per wheel step
    inputEvent({ ...p, kind: 5, mods: inputMods(e), buttons: inputButtons(e.buttons),
                 wheel: Math.sign(e.deltaY) * Math.min(32767, Math.ceil(Math.abs(e.deltaY) / Math.max(1, unit))) });
    return;
  }
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
  if (desktopInput) return; // document handler sends both edges
  if (e.key.length > 1 && key(e.key)) e.preventDefault();
});
typer.addEventListener('input', () => {
  if (typer.value) {
    if (desktopInput) { for (const ch of typer.value) key(ch); }
    else send(enc.encode(typer.value));
  }
  typer.value = '';
});

// A computer's keyboard, whenever nothing else on the page wants it.
// A held arrow or letter repeats steadily from the start: the system's own
// repeat waits half a second after the first press, and a game felt that as
// move, stall, move. Letters wait longer first, so a tap while typing is one.
const held = new Map();
const stopHeld = (k) => { const t = held.get(k); if (t) { clearTimeout(t.t); held.delete(k); } };
document.addEventListener('keydown', (e) => {
  if (desktopInput) { desktopKeyboard(e, false); return; }
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
  if (desktopInput) desktopKeyboard(e, true);
  stopHeld(e.key);
  if (e.key.length === 1) { stopHeld(e.key.toLowerCase()); stopHeld(e.key.toUpperCase()); }   // shift let go first
});
window.addEventListener('blur', () => { releaseInput(); for (const k of [...held.keys()]) stopHeld(k); });

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
  if (document.visibilityState === 'hidden') releaseInput();
  if (document.visibilityState === 'visible' && !stopped && (!ws || ws.readyState > 1) && $('login').hidden) connect();
});
window.addEventListener('resize', () => { sigs = null; schedulePaint(); fitSoon(); });
function watchPixelRatio() {
  const media = matchMedia(`(resolution: ${window.devicePixelRatio || 1}dppx)`);
  media.addEventListener?.('change', () => { watchPixelRatio(); schedulePaint(); fitSoon(); }, { once: true });
}
watchPixelRatio();

// Ask for a grid that fills the window at the board's own cell size. A board
// whose screen is only a terminal (a Nano) takes it and regrids; one with
// glass keeps its own. ?fit=0 leaves the grid alone.
let FIT = new URLSearchParams(location.search).get('fit') !== '0' && localStorage.getItem('lakeshark-fit') !== '0';
const DESKTOP_FONTS = ['7x13', '8x14', '10x17'];
// 10x17, the board's own cell, for a new viewer; 8x14 and 7x13 from the gear.
// The choice is kept in this browser.
let desktopFont = '10x17';
try { desktopFont = localStorage.getItem('lakeshark-font') || '10x17'; } catch (e) { /* this visit only */ }
if (!DESKTOP_FONTS.includes(desktopFont)) desktopFont = '10x17';
function denseDesktop() { return desktopInput && !matchMedia('(pointer: coarse)').matches && !!cm; }
function fitGrid(width, height, dpr, dense, font) {
  const [cw, ch] = dense ? font.split('x').map(Number) : [CELL_W, CELL_H];
  let cols = Math.max(40, Math.min(dense ? 384 : 200, Math.floor(width * (dense ? dpr : 1) / cw)));
  let rows = Math.max(12, Math.min(dense ? 112 : 60, Math.floor(height * (dense ? dpr : 1) / ch)));
  const cap = dense ? Math.min(43008, Math.floor(4194304 / (cw * ch))) : 8192;
  if (cols * rows > cap) {
    const k = Math.sqrt(cap / (cols * rows));
    cols = Math.floor(cols * k); rows = Math.floor(rows * k);
  }
  return [cols, rows];
}
// Remember admission failures so a later window resize does not immediately
// ask for the same allocation again. Retry at 3/4 of each axis, down to 40x12.
let admittedCells = Infinity;
let requestedSize = null;
function requestSize(cols, rows, font = '') {
  while (cols * rows > admittedCells && (cols > 40 || rows > 12)) {
    cols = Math.max(40, Math.floor(cols * .75));
    rows = Math.max(12, Math.floor(rows * .75));
  }
  requestedSize = [cols, rows, font];
  say(`term size ${cols} ${rows}${font ? ' ' + font : ''}`);
}
function sizeReply(message) {
  const m = message.match(/^(?:console )?term: size (\d+)x(\d+) (nomem|kept|regrid)$/);
  if (!m) return false;
  if (m[3] === 'nomem' && requestedSize &&
      +m[1] === requestedSize[0] && +m[2] === requestedSize[1]) {
    const [c, r, font] = requestedSize;
    if (c > 40 || r > 12) {
      const cols = Math.max(40, Math.floor(c * .75));
      const rows = Math.max(12, Math.floor(r * .75));
      admittedCells = Math.min(admittedCells, cols * rows);
      requestSize(cols, rows, font);
    }
  }
  return true;
}
let fitT = 0;
function fitSoon(ms = 600) {
  if (!FIT) return;
  clearTimeout(fitT);
  fitT = setTimeout(() => {
    const stage = $('stage');
    if (!live || (!term && !cm) || !stage.clientWidth) return;
    const dense = denseDesktop();
    const [cols, rows] = fitGrid(stage.clientWidth, stage.clientHeight, window.devicePixelRatio || 1, dense, desktopFont);
    const [now_c, now_r] = grid();
    const fontChanged = dense && `${cm.cw}x${cm.ch}` !== desktopFont;
    if (cols !== now_c || rows !== now_r || fontChanged)
      requestSize(cols, rows, dense ? desktopFont : '');
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
// The desk's LakeShark Studio window asks for Studio on this PC: the USB
// bridge serves it at /studio/. A key press on the page just went to the
// board, so the browser still counts this as the visitor's own action.
function openStudio() {
  // Over USB the bridge opens Studio in its own window (a page cannot open one
  // for a board message: there is no click behind it). Elsewhere, say how.
  if (local) return;
  note('Studio runs on the PC: lsdk studio, then http://127.0.0.1:8790/');
  setTimeout(() => note(''), 8000);
}
function consoleLine(l) {
  let m;
  if (l === 'studio: open') { openStudio(); return; }
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
  if (!local) {
    p.append(el('h2', {}, 'SCREEN'), el('div', { cls: 'row' },
      btn('Board pixels for pictures', () => visualToggle('pixels'), boardPixels),
      btn('Smooth lines', () => visualToggle('lines'), smoothLines)));
    return;
  }
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
  p.append(el('div', { cls: 'row' },
    btn('Board pixels for pictures', () => visualToggle('pixels'), boardPixels),
    btn('Smooth lines', () => visualToggle('lines'), smoothLines)));
  p.append(el('div', { cls: 'row' }, btn('FIT ' + (FIT ? 'ON' : 'OFF'), () => {
      FIT = !FIT; try { localStorage.setItem('lakeshark-fit', FIT ? '1' : '0'); } catch (e) {} if (FIT) fitSoon(100); drawPanel();
    }, FIT),
    btn('THEME', () => send(enc.encode('\x1b[20~'))),
    btn('DAYLIGHT', () => { cmd('tui daylight ' + (dev.daylight ? 'off' : 'on')); dev.daylight = !dev.daylight; drawPanel(); }, dev.daylight === true)));
  if (denseDesktop()) p.append(el('div', { cls: 'row' }, ...DESKTOP_FONTS.map(font => btn(font, () => {
    desktopFont = font;
    try { localStorage.setItem('lakeshark-font', font); } catch (e) {}
    // A font choice applies even when automatic window fitting is off.
    if (FIT) fitSoon(0);
    else {
      const [c, r] = grid(), [cw, ch] = font.split('x').map(Number);
      const [cols, rows] = fitGrid(c * cw, r * ch, 1, true, font);
      requestSize(cols, rows, font);
    }
    drawPanel();
  }, desktopFont === font))));
  if (cm) p.append(pcFontRow());
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
