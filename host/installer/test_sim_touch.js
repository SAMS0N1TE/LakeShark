// Headless pointer routing/geometry checks. Run: node host/installer/test_sim_touch.js
'use strict';
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const handlers = {}, windowHandlers = {}, sent = [], rings = [];
let box = { left: 80, top: 40, width: 240, height: 400 };
const canvas = {
  style: {}, width: 1440, height: 2400, // deliberately unrelated DPR backing size
  getBoundingClientRect: () => box,
  addEventListener(type, fn, options) { assert.equal(options.capture, true); handlers[type] = fn; },
  setPointerCapture(id) { this.captured = id; },
  releasePointerCapture(id) { assert.equal(this.captured, id); this.captured = null; },
};
const context = {
  canvas, cm: { cols: 48, rows: 47, cw: 10, ch: 17 }, live: {}, term: null,
  grid: () => [context.cm.cols, context.cm.rows],
  say: s => sent.push(s),
  document: {
    createElement() { return { style: {}, animate: () => {}, remove: () => {} }; },
    body: { appendChild: e => rings.push(e) },
  },
  window: { addEventListener: (type, fn) => { windowHandlers[type] = fn; } },
  setTimeout: fn => fn(),
};
vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../../app-sdk/studio/web/sim-touch.js'), 'utf8'), context);
function pointer(type, x, y, extra = {}) {
  const e = { clientX: x, clientY: y, pointerId: 1, button: 0, pointerType: 'mouse',
    preventDefault() { this.prevented = true; },
    stopImmediatePropagation() { this.stopped = true; }, ...extra };
  handlers[type](e);
  assert.equal(e.stopped, true); // no duplicate SGR mouse reports
  return e;
}
assert.equal(canvas.style.cursor, 'crosshair');
assert.equal(pointer('pointerdown', 200, 240).prevented, true);
assert.equal(sent.pop(), 'term touch down 240 399');
assert.equal(canvas.style.cursor, 'grabbing');
pointer('pointermove', 201, 241);
assert.equal(sent.pop(), 'term touch move 242 401'); // pixel changes inside a cell
pointer('pointermove', 201, 241);
assert.equal(sent.length, 0); // identical samples are suppressed
pointer('pointerup', 201, 241);
assert.equal(sent.pop(), 'term touch up 242 401');
assert.equal(rings.length, 1);
assert.match(rings[0].style.cssText, /left:200px;top:240px/); // ring marks the tap's origin
assert.equal(canvas.style.cursor, 'crosshair');

// Landscape comes from lssim's already rotated G record, with a different font.
context.cm = { cols: 100, rows: 30, cw: 8, ch: 14 };
box = { left: 50, top: 175, width: 1000, height: 525 };
pointer('pointerdown', 300, 437.5, { pointerType: 'touch' });
assert.equal(sent.pop(), 'term touch down 200 210');
pointer('pointermove', 400, 500, { pointerId: 2 });
assert.equal(sent.length, 0); // a second finger cannot steal the gesture
pointer('pointermove', 3000, -50);
assert.equal(sent.pop(), 'term touch move 799 0'); // captured drags clamp at glass edges
pointer('pointerup', 700, 437.5);
assert.equal(sent.pop(), 'term touch up 520 210');
assert.equal(rings.length, 1); // swipes do not show a tap ring

pointer('pointerdown', 300, 437.5, { pointerType: 'pen' });
sent.length = 0;
pointer('pointercancel', 300, 437.5);
assert.equal(sent.pop(), 'term touch cancel 200 210');
pointer('lostpointercapture', 300, 437.5);
assert.equal(sent.length, 0);
pointer('pointerdown', 300, 437.5);
sent.length = 0;
windowHandlers.blur();
assert.equal(sent.pop(), 'term touch cancel 200 210');
pointer('pointerup', 300, 437.5);
assert.equal(sent.length, 0); // blur cannot leave a held finger or synthesize a tap
context.live = null;
pointer('pointerdown', 300, 437.5);
assert.equal(sent.length, 0);
assert.equal(handlers.keydown, undefined); // keyboard handling stays with screen.js
console.log('PASS sim pointer scaling, letterboxing, landscape/font geometry, mouse/touch/pen, capture, cancellation and ring');
