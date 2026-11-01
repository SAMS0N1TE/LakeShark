// Real page handlers in a DOM/socket/clock stand-in. No browser or board access.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const zlib = require('node:zlib');
const root = path.resolve(__dirname, '../..');
class Element {
  constructor(tagName = 'DIV') { this.tagName = tagName; this.listeners = {}; this.style = {}; this.hidden = true; this.value = ''; this.children = []; this.classList = { add() {}, remove() {}, toggle() {} }; }
  addEventListener(n, f) { (this.listeners[n] ||= []).push(f); }
  dispatch(n, e = {}) { e.target ||= this; e.preventDefault ||= () => { e.prevented = true; }; for (const f of this.listeners[n] || []) f(e); return e; }
  getBoundingClientRect() { return { left: 10, top: 20, width: 1000, height: 500 }; }
  appendChild(e) { this.children.push(e); }
  setAttribute() {}
  querySelectorAll() { return []; }
  append(...items) { this.children.push(...items); }
  replaceChildren() { this.children = []; }
  contains() { return false; }
  getContext() { return { putImageData: (...args) => { this.lastImage = args[0]; } }; }
}
const elements = new Map();
const document = new Element();
document.getElementById = id => { if (!elements.has(id)) elements.set(id, new Element(id === 'type' || id === 'pass' ? 'INPUT' : 'DIV')); return elements.get(id); };
document.createElement = tag => new Element(tag.toUpperCase());
let coarse = false;
const window = new Element(), sent = [], frames = [], timers = new Map();
let now = 100, timerId = 0;
class WebSocket { constructor() { this.readyState = 1; } send(s) { sent.push(s); } }
const ctx = vm.createContext({ document, window, WebSocket, TextEncoder, URLSearchParams,
  location: { host: 'test', search: '' }, localStorage: { getItem: () => null, setItem() {} },
  performance: { now: () => now }, requestAnimationFrame: f => frames.push(f),
  setTimeout: (f, delay) => { const id = ++timerId; timers.set(id, { f, delay }); return id; },
  clearTimeout: id => timers.delete(id), setInterval: () => ++timerId, clearInterval() {},
  console, Uint8Array, DataView, Map, Blob, Response, DecompressionStream,
  ImageData: class { constructor(w,h) { this.width=w; this.height=h; this.data=new Uint8ClampedArray(w*h*4); } },
  fetch: async () => new Response(fs.readFileSync(path.join(root,'components/apps/tui/web/lsfont.bin.gz'))),
  matchMedia: q => ({ matches: q.includes('coarse') ? coarse : !coarse, addEventListener() {} }),
});
const run = s => vm.runInContext(s, ctx);
run(fs.readFileSync(path.join(root, 'components/apps/tui/web/screen.js'), 'utf8'));

(async () => {
  await run('fontsReady');
  assert.equal(run('fonts.length'),5);
  assert.equal(run('fonts[3].w + "x" + fonts[3].h'),'8x14');
  assert.equal(run('fonts[4].w + "x" + fonts[4].h'),'7x13');
  const dims = (w,h,dpr,dense,font) => Array.from(run(`fitGrid(${w},${h},${dpr},${dense},'${font}')`));
  assert.deepEqual(dims(2520,1365,1,true,'7x13'),[360,105]);
  assert.deepEqual(dims(1680,910,1.5,true,'7x13'),[360,105]);
  assert.deepEqual(dims(1260,682.5,2,true,'7x13'),[360,105]);
  assert.deepEqual(dims(2880,1470,1,true,'8x14'),[358,104]);
  assert.deepEqual(dims(2560,1440,1,true,'10x17'),[256,84]);
  assert.deepEqual(dims(2560,1440,1,true,'8x14'),[320,102]);
  for (const font of ['7x13','8x14','10x17']) {
    const [c,r]=dims(8000,8000,2,true,font),[cw,ch]=font.split('x').map(Number);
    assert(c*r*cw*ch<=4194304);
  }
  assert.deepEqual(dims(8000,8000,2,true,'7x13'),[384,112]);
  assert.deepEqual(dims(1480,765,2,false,'7x13'),[148,45]);
  const legacy = dims(2560,1440,1,false,'7x13');
  assert(legacy[0]<=200 && legacy[1]<=60 && legacy[0]*legacy[1]<=8192);
  const stage=document.getElementById('stage'), canvas=document.getElementById('screen');
  stage.clientWidth=1680; stage.clientHeight=910; window.devicePixelRatio=1.5;
  run('begin(100,40,true)');
  run("ws.onmessage({data:'term: caps ev1'})");
  assert(run('denseDesktop()'));
  run('fitSoon(0)'); timers.get(run('fitT')).f();
  assert.equal(sent.at(-1),'term size 360 105 7x13');
  run('desktopFont="8x14"; fitSoon(0)'); timers.get(run('fitT')).f();
  assert.equal(sent.at(-1),'term size 315 97 8x14');
  coarse=true; assert.equal(run('denseDesktop()'),false);
  run('fitSoon(0)'); timers.get(run('fitT')).f();
  assert.equal(sent.at(-1),'term size 161 50');
  coarse=false; run('desktopFont="7x13"');
  run("requestSize(384,112,'7x13')");
  run("ws.onmessage({data:'console term: size 384x112 nomem'})");
  assert.equal(sent.at(-1),'term size 288 84 7x13');
  run("ws.onmessage({data:'term: size 288x84 nomem'})");
  assert.equal(sent.at(-1),'term size 216 63 7x13');
  const sentBefore=sent.length;
  run("ws.onmessage({data:'term: size 384x112 nomem'})");
  assert.equal(sent.length,sentBefore); // stale reply
  run("fitSoon(0)"); timers.get(run('fitT')).f();
  assert(run('requestedSize[0]*requestedSize[1] <= admittedCells'));
  run("requestSize(40,12,'7x13')");
  const atMinimum=sent.length;
  run("ws.onmessage({data:'term: size 40x12 nomem'})");
  assert.equal(sent.length,atMinimum); // no retry loop at minimum
  run('begin(100,40,true)');
  assert.equal(run('admittedCells'),Infinity);
  run("ws.onmessage({data:'term: caps ev1'})");
  // All records at full capacity, chunked as the firmware does. Include Cyrillic
  // byte 0x97 and the last cell to catch u8 columns and 8192-cell truncation.
  const cols=384, rows=112, n=cols*rows;
  const raw=Buffer.alloc(8+33+5+n*3+5); let at=0;
  raw[at++]=71;raw.writeUInt16LE(cols,at);at+=2;raw.writeUInt16LE(rows,at);at+=2;
  raw[at++]=4;raw[at++]=7;raw[at++]=13;
  raw[at++]=80;for(let i=0;i<16;i++){raw.writeUInt16LE(i?65535:0,at);at+=2;}
  raw[at++]=67;raw.writeUInt16LE(0,at);at+=2;raw.writeUInt16LE(n,at);at+=2;
  for(let i=0;i<n;i++){raw.writeUInt16LE(i===n-1?0x97:65,at);at+=2;raw[at++]=15;}
  raw[at++]=76;raw.writeUInt32LE(123,at);at+=4;assert.equal(at,raw.length);
  const compressed=zlib.deflateRawSync(raw);
  const frame=(body,z=false) => {
    const crc=run(`crc16(new Uint8Array([${[...body]}]))`);
    return Buffer.concat([Buffer.from((z?'\x1c':'\x1e')+body.length.toString(16).padStart(4,'0')),body,Buffer.from('\x1f'+crc.toString(16).padStart(4,'0'))]);
  };
  const push=b=>{ctx.packet=b;run('live.parser.push(packet)');};
  // Raw records cross many frames; compressed records share the same parser.

  for(const z of [false,true]) {
    const body=z?compressed:raw;
    for(let i=0;i<body.length;i+=4000) {
      const f=frame(body.subarray(i,i+4000),z);
      push(f.subarray(0,3));push(f.subarray(3));
    }
    const before=sent.filter(x=>x==='term ack').length;
    push(frame(Buffer.alloc(0)));
    await run('live.chain');
    assert.equal(sent.filter(x=>x==='term ack').length,before); // actual paint owns ack
    while(frames.length) frames.shift()();
    assert.equal(sent.filter(x=>x==='term ack').length,before+1);
    assert.equal(run('cm.g.length'),n); assert.equal(run('cm.g[cm.g.length-1]'),0x97);
    assert.equal(run('cm.face === fonts[4]'),true);
    assert.equal(canvas.width,384*7); assert.equal(canvas.height,112*13);
    assert.equal(canvas.style.width,384*7/1.5+'px');
    assert.equal(canvas.style.height,112*13/1.5+'px');
  }
  // A noisy full X update must inflate across dozens of 4000-byte frames.
  const xraw=Buffer.alloc(5+n*8); xraw[0]=88;
  xraw.writeUInt16LE(n,3);
  let rng=1;
  for(let i=0;i<n;i++) {
    const p=5+i*8; xraw.writeUInt16LE(0xb0,p); xraw[p+3]=1;
    rng=(Math.imul(rng,1664525)+1013904223)>>>0;xraw.writeUInt16LE(rng&65535,p+4);
    rng=(Math.imul(rng,1664525)+1013904223)>>>0;xraw.writeUInt16LE(rng>>>16,p+6);
  }
  const xz=zlib.deflateRawSync(xraw);assert(xz.length>4000*40);
  for(let i=0;i<xz.length;i+=4000) push(frame(xz.subarray(i,i+4000),true));
  push(frame(Buffer.alloc(0)));await run('live.chain');
  while(frames.length) frames.shift()();
  assert.equal(run('cm.k[cm.k.length-1]'),1);
  assert.equal(run('cm.c1[cm.c1.length-1]'),xraw.readUInt16LE(xraw.length-4));
  assert.equal(run('cm.c2[cm.c2.length-1]'),xraw.readUInt16LE(xraw.length-2));
  // A damaged update is discarded, acknowledged, and explicitly redrawn.
  const bad=frame(Buffer.from([67,0,0,0,0]));bad[5]^=1;
  push(bad);push(frame(Buffer.alloc(0)));
  assert(sent.includes('term redraw'));
  while(frames.length) frames.shift()();
  run('drawPanel()');
  const labels=document.getElementById('panel').children.flatMap(x=>x.children||[]).flatMap(x=>x.children||[]);
  assert(labels.includes('7x13') && labels.includes('8x14') && labels.includes('10x17'));
  console.log('screen resolution: DPR 1/1.5/2, fonts/settings, legacy/coarse, 43008-cell C feed, raw/deflate framing, paint ACK PASS');
})().catch(e=>{console.error(e);process.exitCode=1;});

