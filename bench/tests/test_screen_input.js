// Real page handlers in a DOM/socket/clock stand-in. No browser or board access.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const root = path.resolve(__dirname, '../..');
class Element {
  constructor(tagName = 'DIV') { this.tagName = tagName; this.listeners = {}; this.style = {}; this.hidden = true; this.value = ''; this.children = []; this.classList = { add() {}, remove() {}, toggle() {} }; }
  addEventListener(n, f) { (this.listeners[n] ||= []).push(f); }
  dispatch(n, e = {}) { e.target ||= this; e.preventDefault ||= () => { e.prevented = true; }; for (const f of this.listeners[n] || []) f(e); return e; }
  getBoundingClientRect() { return { left: 10, top: 20, width: 1000, height: 500 }; }
  appendChild(e) { this.children.push(e); }
  setAttribute() {}
  querySelectorAll() { return []; }
}
const elements = new Map();
const document = new Element();
document.getElementById = id => { if (!elements.has(id)) elements.set(id, new Element(id === 'type' || id === 'pass' ? 'INPUT' : 'DIV')); return elements.get(id); };
document.createElement = tag => new Element(tag.toUpperCase());
const window = new Element(), sent = [], frames = [], timers = new Map();
let now = 100, timerId = 0;
class WebSocket { constructor() { this.readyState = 1; } send(s) { sent.push(s); } }
const ctx = vm.createContext({ document, window, WebSocket, TextEncoder, URLSearchParams,
  location: { host: 'test', search: '?fit=0' }, localStorage: { getItem: () => null, setItem() {} },
  performance: { now: () => now }, requestAnimationFrame: f => frames.push(f),
  setTimeout: (f, delay) => { const id = ++timerId; timers.set(id, { f, delay }); return id; },
  clearTimeout: id => timers.delete(id), setInterval: () => ++timerId, clearInterval() {},
  console, Uint8Array, DataView, Map, Blob, Response,
  matchMedia: q => ({matches: !q.includes('coarse'), addEventListener() {}}),
});
const run = s => vm.runInContext(s, ctx);
run(fs.readFileSync(path.join(root, 'components/apps/tui/web/screen.js'), 'utf8'));
run('live = {}; term = {cols:100, rows:50};');
const canvas = document.getElementById('screen');
const records = () => sent.filter(s => s.startsWith('term ev ')).flatMap(s => {
  const b = Buffer.from(s.slice(8), 'hex'), out = [];
  for (let i = 0; i < b.length; i += 24) out.push({ kind: b[i+1], mods:b[i+3], button:b[i+4], buttons:b[i+5], clicks:b[i+6],
    col:b.readInt16LE(i+7), row:b.readInt16LE(i+9), px:b.readInt16LE(i+11), py:b.readInt16LE(i+13),
    wheel:b.readInt16LE(i+15), key:b.readUInt16LE(i+17), ch:b[i+19], seq:b.readUInt32LE(i+20) });
  return out;
});
// Old firmware keeps VT, including Escape's immediate no-op suffix.
document.dispatch('keydown', { key:'Escape', code:'Escape' });
assert.equal(sent.pop(), 'term in 1b1b5b306e');
assert.equal(run('desktopInput'), false);
run("ws.onmessage({ data:'console term: caps ev1' })");
assert.equal(run('desktopInput'), true);
sent.length = 0;
// IDs must stay aligned with the append-only C enum.
const header = fs.readFileSync(path.join(root, 'components/apps/tui/ls_tui_screen.h'), 'utf8').replace(/\/\*[\s\S]*?\*\//g, '');
const names = header.slice(header.indexOf('typedef enum'), header.indexOf('} ls_tk_t')).match(/LS_TK_[A-Z0-9_]+/g);
for (const [key, cname] of [['F12','F12'], ['Home','HOME'], ['End','END'], ['PageUp','PGUP'], ['PageDown','PGDN'], ['Insert','INSERT'], ['Delete','DELETE'], ['ContextMenu','MENU'], ['Shift','SHIFT']]) {
  assert.equal(run(`desktopKey({key:${JSON.stringify(key)},code:''}).key`), names.indexOf('LS_TK_' + cname));
}
for (const key of ['Tab','F1','F11','F12','ContextMenu','Home','End','PageUp','PageDown','Insert','Delete','ArrowUp']) {
  assert(document.dispatch('keydown', {key, code:key}).prevented);
  document.dispatch('keyup', {key, code:key});
}
for (const e of [{key:'`',code:'Backquote',ctrlKey:true}, {key:'F4',code:'F4',ctrlKey:true}, {key:'a',code:'KeyA',altKey:true}]) {
  assert(document.dispatch('keydown',e).prevented); document.dispatch('keyup', e);
}
sent.length = 0;
document.dispatch('keydown', {key:'A',code:'KeyA',shiftKey:true,ctrlKey:true});
document.dispatch('keydown', {key:'A',code:'KeyA',shiftKey:true,repeat:true});
document.dispatch('keyup', {key:'a',code:'KeyA'}); // Shift released first: same character identity
let a = records();
assert.deepEqual(a.map(e=>[e.kind,e.ch,e.clicks]), [[0,65,1],[0,65,0],[1,65,0]]);
assert.equal(a[0].mods,5);
sent.length=0;
const ptr = {clientX:55,clientY:65,button:0,buttons:1,pointerType:'mouse',pointerId:1};
canvas.dispatch('pointerdown', {...ptr});
canvas.dispatch('pointerdown', {...ptr,button:2,buttons:3}); // second held button edge
canvas.dispatch('pointerup', {...ptr,button:2,buttons:1});
canvas.dispatch('pointerup', {...ptr,buttons:0});
now+=200;
canvas.dispatch('pointerdown', {...ptr});
a=records();
assert.deepEqual(a.map(e=>[e.kind,e.button,e.buttons]), [[3,0,1],[3,2,5],[4,2,1],[4,0,0],[3,0,1]]);
assert.equal(a[4].clicks,2);
canvas.dispatch('pointerdown', {...ptr,button:1,buttons:4});
assert.equal(records().at(-1).button,1); assert.equal(records().at(-1).buttons,2);
assert.equal(a[0].col,4); assert.equal(a[0].row,4); assert.equal(a[0].px,45);
// Each button has its own double-click history (left/right chord does not reset it).
canvas.dispatch('pointerup', {...ptr,buttons:0}); now+=100;
canvas.dispatch('pointerdown', {...ptr}); assert.equal(records().at(-1).clicks,1);
assert(canvas.dispatch('contextmenu').prevented);
sent.length=0;
now=1000;
canvas.dispatch('pointermove', {...ptr,clientX:100,buttons:0});
canvas.dispatch('pointermove', {...ptr,clientX:101,buttons:0}); // same cell suppressed
now=1001; canvas.dispatch('pointermove', {...ptr,clientX:120,buttons:0});
now=1002; canvas.dispatch('pointermove', {...ptr,clientX:140,buttons:0});
assert.equal(records().length,1);
canvas.dispatch('wheel', {...ptr,deltaY:-300,deltaMode:0}); // flush latest move before wheel
a=records(); assert.deepEqual(a.map(e=>e.kind),[2,2,5]); assert.equal(a[2].wheel,-3);
assert.equal(sent.filter(s=>s.startsWith('term ev ')).at(-1).length,8+48*2);
canvas.dispatch('pointerleave'); assert.equal(records().at(-1).kind,6);
window.dispatch('blur'); assert.equal(records().at(-1).kind,6);
// Receipt is not a sample: only the canvas submission callback records it.
run('eventTimes.clear(); latencySamples.length=0; paintedSeq=0; paint = () => {};');
sent.length=0; now=2000; run('inputEvent({kind:0,key:6,clicks:1})');
const seq=records()[0].seq;
now=2005; run(`paintedSeq=(${seq}+100)>>>0; inputPainted()`);
assert.equal(run('latencySamples.length'),0); // unrecognized old session stamp
now=2010; run(`cm = {}; cellsApply(new Uint8Array([76,${seq&255},${seq>>>8&255},${seq>>>16&255},${seq>>>24}])); schedulePaint();`);
assert.equal(run('latencySamples.length'),0);
now=2035; while(frames.length) frames.shift()();
assert.equal(run('latencySamples[0].ms'),35);
run('reportLatency()'); assert.equal(sent.at(-1),'term lat 35 35');
assert.match(elements.get('bar').children.at(-1).textContent,/p50 35.*p95 35/);
for(let i=0;i<80;i++) { now++; run('inputEvent({kind:0,key:6}); paintedSeq=inputSeq; inputPainted();'); }
assert.equal(run('latencySamples.length'),64);
now+=10000; run('reportLatency()'); assert.equal(run('latencySamples.length'),0);
console.log('screen input: negotiation/VT, key edges/chords/repeat, buttons, hover/coalescing, wheel, release, rAF latency PASS');
