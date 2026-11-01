// "PC font for text" in the web screen's real code, in a DOM stand-in. No
// browser or board. Default off; the gear row saves the choice; text bytes map
// to the Unicode tui_cyr.h says; only text cells take the PC face; without one
// (fonts not loaded, or an unknown size) the board's atlas glyph is drawn.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const root = path.resolve(__dirname, '../..');
class Element {
  constructor(tagName = 'DIV') { this.tagName = tagName; this.listeners = {}; this.style = {}; this.hidden = true; this.children = []; this.classList = { add() {}, remove() {}, toggle() {} }; }
  addEventListener(n, f) { (this.listeners[n] ||= []).push(f); }
  getBoundingClientRect() { return { left: 0, top: 0, width: 100, height: 100 }; }
  appendChild(e) { this.children.push(e); }
  setAttribute() {}
  querySelectorAll() { return []; }
  append(...items) { this.children.push(...items); }
  replaceChildren() { this.children = []; }
  contains() { return false; }
  getContext() { return { putImageData() {} }; }
}
const elements = new Map(), store = new Map();
const document = new Element();
document.getElementById = id => { if (!elements.has(id)) elements.set(id, new Element(id === 'type' || id === 'pass' ? 'INPUT' : 'DIV')); return elements.get(id); };
document.createElement = tag => new Element(tag.toUpperCase());
class WebSocket { constructor() { this.readyState = 1; } send() {} }
const ctx = vm.createContext({ document, window: new Element(), WebSocket, TextEncoder, URLSearchParams,
  location: { host: 'test', search: '' },
  localStorage: { getItem: k => store.has(k) ? store.get(k) : null, setItem: (k, v) => store.set(k, String(v)) },
  performance: { now: () => 0 }, requestAnimationFrame() {}, setTimeout: () => 0, clearTimeout() {},
  setInterval: () => 0, clearInterval() {}, console, Uint8Array, DataView, Map, Blob, Response, DecompressionStream,
  ImageData: class { constructor(w, h) { this.width = w; this.height = h; this.data = new Uint8ClampedArray(w * h * 4); } },
  fetch: async () => new Response(fs.readFileSync(path.join(root, 'components/apps/tui/web/lsfont.bin.gz'))),
  matchMedia: () => ({ matches: false, addEventListener() {} }),
});
const run = s => vm.runInContext(s, ctx);
run(fs.readFileSync(path.join(root, 'components/apps/tui/web/screen.js'), 'utf8'));

// Every Cyrillic byte of tui_cyr.h, and nothing else past ASCII.
const hdr = fs.readFileSync(path.join(root, 'components/apps/tui/tuilib/tui_cyr.h'), 'utf8');
const cyr = new Map([...hdr.matchAll(/\{0x([0-9A-F]+),0x([0-9A-F]+)\}/g)].map(m => [parseInt(m[2], 16), parseInt(m[1], 16)]));
assert.equal(cyr.size, 55);
for (let b = 0; b < 256; b++) {
  const want = b > 0x20 && b < 0x7f ? b : cyr.get(b) || 0;
  assert.equal(run(`CELL_CP[${b}]`), want, `byte ${b.toString(16)}`);
}

(async () => {
  await run('fontsReady');
  assert.equal(run('pcFont'), false);                  // off by default
  run('begin(8, 2, true)');
  // A run of text and non-text cells: letters, a Cyrillic slot, a quadrant,
  // a sextant, a trace, braille, a box line, a frame shape, a picture cell.
  const codes = [0x41, 0x67, 0x97, 0x85, 0xc5, 0xa3, 0x28ff, 0x2502, 0x258f, 0x2022, 0x30, 0xb5, 0x7e, 0x20, 0x40, 0xb0];
  const raw = []; const u16 = v => raw.push(v & 255, v >> 8);
  raw.push(0x47); u16(8); u16(2); raw.push(4, 7, 13);
  raw.push(0x50); for (let i = 0; i < 16; i++) u16(i * 0x1084);
  raw.push(0x43); u16(0); u16(15); for (const c of codes.slice(0, 15)) { u16(c); raw.push(0x0f); }
  raw.push(0x58); u16(15); u16(1); u16(0xb0); raw.push(0x0f, 1); u16(0xf800); u16(0x07e0);
  run(`cellsApply(new Uint8Array([${raw}]))`);
  const pixels = () => Array.from(run('cm.px'));
  const atlas = pixels();
  // On, but no face yet: every cell is still the atlas glyph.
  run('pcFont = true; cellsRepaint()');
  assert.deepEqual(pixels(), atlas);
  // A face that inks every pixel of every text glyph: text cells turn solid
  // foreground, every other cell keeps its exact pixels.
  run('pcReady = true; pcFace = { cw: 7, ch: 13, cov: new Uint8Array(256 * 7 * 13).fill(255) }; cellsRepaint()');
  const on = pixels(), W = 8 * 7, fg = run('cm.pal[15]') >>> 0;
  codes.forEach((code, i) => {
    const x0 = (i % 8) * 7, y0 = ((i / 8) | 0) * 13, text = code < 256 && run(`CELL_CP[${code}]`) > 0;
    for (let y = 0; y < 13; y++) for (let x = 0; x < 7; x++) {
      const k = (y0 + y) * W + x0 + x;
      if (text) assert.equal(on[k] >>> 0, fg, `text cell ${i}`);
      else assert.equal(on[k], atlas[k], `cell ${i} code ${code.toString(16)}`);
    }
  });
  // Another size with no face for it: the atlas again.
  assert.equal(run('pcFaceFor(9, 15)'), null);
  // The gear row: label, state, and the choice kept for this viewer.
  run('pcFont = false; pcFace = null; pcReady = false; cellsRepaint()');
  assert.deepEqual(pixels(), atlas);
  run('pcLoad = Promise.resolve()');                    // no font fetch in this stand-in
  const row = run('pcFontRow()'), button = row.children[0];
  assert.equal(button.children[0], 'PC FONT FOR TEXT OFF');
  button.listeners.click[0]();
  assert.equal(run('pcFont'), true);
  assert.equal(store.get('lakeshark-pcfont'), '1');
  run('pcFontRow()').children[0].listeners.click[0]();
  assert.equal(store.get('lakeshark-pcfont'), '0');
  assert.deepEqual(pixels(), atlas);
  console.log('screen pcfont: default off, gear row + storage, byte map = tui_cyr.h, text-only PC face, atlas fallback PASS');
})().catch(e => { console.error(e); process.exit(1); });
