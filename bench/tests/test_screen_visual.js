// Focused page-path checks. Reuse only the DOM/socket fixture from the existing
// resolution test; none of that test's cases are executed here.
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const zlib = require('node:zlib');
const fixture = fs.readFileSync(path.join(__dirname, 'test_screen_resolution.js'), 'utf8').split('(async () => {')[0];
const { run, ctx, sent, frames, elements } = new Function('require', '__dirname', fixture + '\nreturn {run,ctx,sent,frames,elements};')(require, __dirname);
const root = path.resolve(__dirname, '../..');
const out = process.argv[2] || path.join(root, 'bench/build-pixels');
const oldSource = fs.readFileSync(path.join(out, 'legacy-screen.js'), 'utf8');
const oldRequire = name => name !== 'node:fs' ? require(name) : { ...fs, readFileSync(file, ...args) {
  return String(file).endsWith('screen.js') ? oldSource : fs.readFileSync(file, ...args);
} };
const old = new Function('require', '__dirname', fixture + '\nreturn {run,ctx,sent};')(oldRequire, __dirname);
function png(file, w, h, data) {
  const crc = b => { let c = 0xffffffff; for (const x of b) { c ^= x; for (let i = 0; i < 8; i++) c = c & 1 ? 0xedb88320 ^ c >>> 1 : c >>> 1; } return (c ^ 0xffffffff) >>> 0; };
  const chunk = (type, b) => { const n = Buffer.alloc(4), body = Buffer.concat([Buffer.from(type), b]), c = Buffer.alloc(4); n.writeUInt32BE(b.length); c.writeUInt32BE(crc(body)); return Buffer.concat([n, body, c]); };
  const head = Buffer.alloc(13); head.writeUInt32BE(w); head.writeUInt32BE(h, 4); head[8] = 8; head[9] = 6;
  const rows = Buffer.alloc(h * (w * 4 + 1));
  for (let y = 0; y < h; y++) data.copy(rows, y * (w * 4 + 1) + 1, y * w * 4, (y + 1) * w * 4);
  fs.writeFileSync(file, Buffer.concat([Buffer.from('89504e470d0a1a0a', 'hex'), chunk('IHDR', head), chunk('IDAT', zlib.deflateSync(rows)), chunk('IEND', Buffer.alloc(0))]));
}
async function push(file) {
  const wire = fs.readFileSync(file);
  // Paint each update, as a real acknowledged viewer does. This also exercises
  // the incremental picture/composition cache, not just one final full paint.
  for (let at = 0; at < wire.length;) {
    const n = parseInt(wire.subarray(at + 1, at + 5).toString(), 16), frame = wire.subarray(at, at + n + 10);
    ctx.packet = frame.subarray(0, 3); run('live.parser.push(packet)');
    for (let i = 3; i < frame.length; i += 379) { ctx.packet = frame.subarray(i, i + 379); run('live.parser.push(packet)'); }
    if (!n) { await run('live.chain'); for (const f of frames.splice(0)) f(); }
    at += n + 10;
  }
}
function snapshot(file) {
  const b = fs.readFileSync(file); assert.equal(b.subarray(0, 4).toString(), 'LSP2');
  const m = Array.from({ length: 15 }, (_, i) => b.readUInt32LE(4 + i * 4));
  const [cols, rows, , cw, ch] = m, n = cols * rows;
  const at = 64 + 32 + n * 4 + Math.ceil(n / 8) + m[9] * m[10] * 2 + m[14] * (5 + cw * ch);
  return { b, m, cols, rows, cw, ch, expected: b.subarray(at + 16) };
}
(async () => {
  await run('fontsReady');
  await old.run('fontsReady');
  assert.equal(run('boardPixels'), true); assert.equal(run('smoothLines'), false);
  const saved = new Map(); ctx.localStorage.setItem = (k, v) => saved.set(k, v);
  for (const name of ['map', 'waterfall', 'desktop-map','carto-portrait','carto-landscape']) {
    const dir = path.join(out, name), { b, cols, rows, cw, ch, m, expected } = snapshot(path.join(dir, 'snapshot.bin'));
    run(`boardPixels=true;smoothLines=false;begin(${cols},${rows},true)`);
    run("ws.onmessage({data:'term: caps ev1'})");
    run("ws.onmessage({data:'term: caps ev1 pix1 vec1'})");
    assert.equal(sent.at(-1), 'term visual 1 0');
    await push(path.join(dir, 'all.zframes'));
    const legacy = Buffer.from(run('cm.img.data'));
    const composeStart = process.hrtime.bigint();
    const pixels = Buffer.from(run('visualImage().data'));
    const composeMs = Number(process.hrtime.bigint() - composeStart) / 1e6;
    const kinds = Array.from(run('cm.k'));
    let checked = 0, mismatch = 0, first = '';
    for (let i = 0; i < kinds.length; i++) if (kinds[i] === 1 || kinds[i] === 2) {
      for (let y = 0; y < ch; y++) for (let x = 0; x < cw; x++) {
        const at = ((Math.floor(i / cols) * ch + y) * cols * cw + i % cols * cw + x);
        const c = expected.readUInt16LE(at * 2), got = pixels.readUInt32LE(at * 4);
        const want = ((c >> 11) * 255 / 31 | 0) | ((c >> 5 & 63) * 255 / 63 | 0) << 8 | ((c & 31) * 255 / 31 | 0) << 16 | 0xff000000;
        if (got !== (want >>> 0)) { mismatch++; first ||= `cell ${i} (${x},${y}) got ${got.toString(16)} expected ${(want >>> 0).toString(16)}`; }
        checked++;
      }
    }
    assert(checked > 1000); assert.equal(mismatch, 0, `${name}: ${first}; ${mismatch}/${checked} pixels differ from board`);
    assert.deepEqual(Buffer.from(run('cm.img.data')), legacy, 'composition must preserve original cell image');
    png(path.join(dir, 'page.png'), cols * cw, rows * ch, pixels);
    run('visualToggle("pixels")'); assert.equal(saved.get('lakeshark-board-pixels'), '0');
    assert.equal(sent.at(-1), 'term visual 0 0'); assert(run('visualImage()===cm.img'));
    assert.deepEqual(Buffer.from(run('visualImage().data')), legacy);
    const oldRecords = fs.readFileSync(path.join(dir, 'legacy.records'));
    ctx.oldRecords = oldRecords; run('cellsApply(oldRecords)');
    old.run(`begin(${cols},${rows},true)`); old.ctx.oldRecords = oldRecords; old.run('cellsApply(oldRecords)');
    assert.deepEqual(Buffer.from(run('visualImage().data')), Buffer.from(old.run('cm.img.data')), 'pixels off is exactly the old page');
    old.run("ws.onmessage({data:'term: caps ev1'}); ws.onmessage({data:'term: caps ev1 pix1 vec1'})");
    assert.equal(old.run('desktopInput'), true, 'retained ev1 line keeps old page input working');
    run('visualToggle("pixels")');
    run('visualImage()');
    ctx.edit = Buffer.from([67,0,0,1,0,65,0,15]); run('cellsApply(edit)');
    const patched = Buffer.from(run('visualImage().data')), text = Buffer.from(run('cm.img.data'));
    for (let y = 0; y < ch; y++) {
      const at = y * cols * cw * 4;
      assert.deepEqual(patched.subarray(at, at + cw * 4), text.subarray(at, at + cw * 4));
    }
    const picAt = (m[6] * ch * cols * cw + m[5] * cw) * 4;
    assert.deepEqual(patched.subarray(picAt, picAt + 128), pixels.subarray(picAt, picAt + 128));
    // The parallel text-font viewer can redraw the glyph layer directly.
    // A full redraw must invalidate our cache while cellDraw stays untouched.
    run('cm.g[0]=66; for(let i=0;i<cm.g.length;i++) cellDraw(i); paintCells()');
    const redrawn = Buffer.from(elements.get('screen').lastImage.data), current = Buffer.from(run('cm.img.data'));
    for (let y = 0; y < ch; y++) {
      const at = y * cols * cw * 4;
      assert.deepEqual(redrawn.subarray(at, at + cw * 4), current.subarray(at, at + cw * 4));
    }
    await push(path.join(dir, 'scroll.zframes'));
    assert.equal(run('cm.picture.px[cm.picture.y0*cm.picture.sw]'), 0);
    assert.equal(run('cm.picture.px[cm.picture.y0*cm.picture.sw+10]'), (10 * 319) & 65535);
    assert.equal(run('cm.picture.valid[cm.picture.y0]'), 1);
    const storedPixels = run('cm.picture.px');
    await push(path.join(dir, 'move.zframes'));
    assert.strictEqual(run('cm.picture.px'), storedPixels, 'moving reuses pixels, including during input');
    assert.equal(run('cm.picture.x'), (m[5] + 1) * cw);
    await push(path.join(dir, 'vectors.zframes'));
    assert.equal(run('cm.lines.length'), 512);
    assert.equal(run('cm.lineCells.size'), 1);
    run('smoothLines=true');
    assert.equal(run(`lineMask(${cols + 1})`), 0x47);
    const pen = { rectangles: 0, strokes: 0, points: [], save() {}, restore() {}, beginPath() {}, clip() {},
      rect() { this.rectangles++; }, stroke() { this.strokes++; }, moveTo(x,y) { this.points.push([x,y]); }, lineTo() {}, arc() {}, fill() {} };
    ctx.pen = pen; run('paintVectors(pen)');
    assert.equal(pen.rectangles, 4); assert.equal(pen.strokes, 1); assert.deepEqual(pen.points[0], [8,14]);
    const oldInk = run('cm.pal[15]'); run('cm.pal[15]^=1'); assert.equal(run(`lineMask(${cols + 1})`), 0);
    ctx.oldInk = oldInk; run('cm.pal[15]=oldInk');
    run(`cm.g[${cols + 1}]=65`); assert.equal(run(`lineMask(${cols + 1})`), 0, 'overwritten dots must not hide text');
    run('smoothLines=false');
    await push(path.join(dir, 'crop.zframes'));
    assert.equal(run('cm.picture.x'), -21); assert.equal(run('cm.picture.y'), -26);
    const p = run('cm.picture'), cropped = Buffer.from(run('visualImage().data'));
    const sourceAt = 64 + 32 + cols * rows * 4 + Math.ceil(cols * rows / 8);
    const c565 = c => (((c >> 11) * 255 / 31 | 0) | ((c >> 5 & 63) * 255 / 63 | 0) << 8 | ((c & 31) * 255 / 31 | 0) << 16 | 0xff000000) >>> 0;
    for (let y = 0; y < Math.min(rows * ch, p.y + p.h); y += 5) for (let x = 0; x < Math.min(cols * cw, p.x + p.w); x += 5) {
      const sx = (Math.floor((x - p.x) * p.sw / p.w) + p.x0) % p.sw;
      const sy = (Math.floor((y - p.y) * p.sh / p.h) + p.y0) % p.sh;
      const c = sy === p.y0 ? (sx * 319) & 65535 : b.readUInt16LE(sourceAt + (sy * p.sw + sx) * 2);
      assert.equal(cropped.readUInt32LE((y * cols * cw + x) * 4), c565(c));
    }
    const oldX = p.x0;
    await push(path.join(dir, 'column.zframes'));
    assert.equal(run('cm.picture.x0'), (oldX + 1) % p.sw);
    for (let y = 0; y < p.sh; y++) {
      assert.equal(p.px[y * p.sw + oldX], (y * 115) & 65535);
      assert.equal(p.valid[y], 1);
    }
    const shifted = Buffer.from(run('visualImage().data'));
    for (let y = 0; y < Math.min(rows * ch, p.y + p.h); y += 5) for (let x = 0; x < Math.min(cols * cw, p.x + p.w); x += 5) {
      const sx = (Math.floor((x - p.x) * p.sw / p.w) + p.x0) % p.sw;
      const sy = (Math.floor((y - p.y) * p.sh / p.h) + p.y0) % p.sh;
      const c = sx === oldX ? (sy * 115) & 65535 : sy === p.y0 ? (sx * 319) & 65535 : b.readUInt16LE(sourceAt + (sy * p.sw + sx) * 2);
      assert.equal(shifted.readUInt32LE((y * cols * cw + x) * 4), c565(c));
    }
    console.log(`PASS ${name}: ${checked} exact picture/glass pixels through framed z -> page, toggles, row/column rings and ownership; cached compose ${composeMs.toFixed(1)} ms`);
  }
  run('visualToggle("lines")'); assert.equal(saved.get('lakeshark-smooth-lines'), '1');
  assert.equal(sent.at(-1), 'term visual 1 1');
  // Old capabilities neither request visual records nor break desktop input.
  run('visualCaps={pixels:false,vectors:false};begin(160,45,true)');
  const before = sent.length; run("ws.onmessage({data:'term: caps ev1'})");
  assert.equal(sent.length, before);
  assert.equal(run('desktopInput'), true);
  console.log('PASS negotiated capabilities, old firmware fallback, per-viewer defaults/storage');
  // Actual production 2D scroll and RLE spans; keep all viewer pixels exact.
  const panDir=path.join(out,'desktop-map');
  run('boardPixels=true;begin(360,105,true)');
  await push(path.join(panDir,'pan-initial.zframes'));
  const initial=Array.from(run('cm.picture.px')), sw=690,sh=240;
  const originalX=run('cm.picture.x');
  await push(path.join(panDir,'pan-move.zframes'));assert.equal(run('cm.picture.x'),originalX+7);
  assert.deepEqual(Array.from(run('cm.picture.px')),initial);
  await push(path.join(panDir,'pan-move-back.zframes'));assert.equal(run('cm.picture.x'),originalX);
  await push(path.join(panDir,'pan-preview.zframes'));
  const preview=Array.from(run('cm.picture.px'));
  for(let y=0;y<sh;y++)for(let x=0;x<sw;x++)
    assert.equal(preview[y*sw+x],x+90<sw && y+16<sh?initial[(y+16)*sw+x+90]:0);
  await push(path.join(panDir,'pan-fine.zframes'));
  const expected=fs.readFileSync(path.join(panDir,'pan-expected.rgb565'));
  const final=Array.from(run('cm.picture.px'));
  for(let i=0;i<final.length;i++)assert.equal(final[i],expected.readUInt16LE(i*2));
  assert(run('cm.picture.valid.every(x=>x===1)'));
  await push(path.join(panDir,'pan-negative.zframes'));
  const negative=Array.from(run('cm.picture.px'));
  for(let y=0;y<sh;y++)for(let x=0;x<sw;x++)
    assert.equal(negative[y*sw+x],x>=12 && y>=8?final[(y-8)*sw+x-12]:0);
  // Truncated spans and impossible RLE lengths must not partially mutate pixels.
  const p=run('cm.picture'), bad=Buffer.alloc(33);
  bad[0]=73;bad[1]=6;
  [p.x,p.y,p.w,p.h,0,1,p.sw,p.sh,0,0].forEach((n,i)=>bad.writeUInt16LE(n&65535,2+i*2));
  bad.writeUInt16LE(0,22);bad.writeUInt16LE(1,24);bad[26]=1;
  bad.writeUInt16LE(1,27);bad.writeUInt16LE(2,29);bad.writeUInt16LE(65535,31);
  ctx.bad=bad;run('cellsApply(bad)');assert.deepEqual(Array.from(run('cm.picture.px')),negative);
  bad.writeUInt16LE(1,29);ctx.bad=bad.subarray(0,32);run('cellsApply(bad)');
  assert.deepEqual(Array.from(run('cm.picture.px')),negative);
  await push(path.join(panDir,'pan-lag.zframes'));
  assert(run('cm.picture.valid.every(x=>x===1)'));
  await push(path.join(panDir,'pan-index4.zframes'));
  const indexed4=Array.from(run('cm.picture.px'));
  for(let y=0;y<sh;y++)for(let x=0;x<sw;x++)assert.equal(indexed4[y*sw+x],1+((x+y)&1));
  await push(path.join(panDir,'pan-overflow.zframes'));
  const overflow=Array.from(run('cm.picture.px'));
  for(let y=0;y<sh;y++)for(let x=0;x<sw;x++)assert.equal(overflow[y*sw+x],(x*239+y*977)&65535);
  console.log('PASS production 2D preview, signed scroll, strip completion, indexed8/indexed4/RLE/raw and palette overflow recovery');
})().catch(e => { console.error(e); process.exitCode = 1; });
