// Headless combined tilt/touch routing. node host/installer/test_sim_tilt.js
'use strict';
const assert = require('node:assert/strict'), fs = require('node:fs'), vm = require('node:vm');
const path = require('node:path');
const nodes = {}, sent = [], windows = {}, callbacks = new Map(), shown = [];
let now = 0, next = 0;
class Element {
  constructor(id) { this.id = id; this.style = {}; this.handlers = {}; if (id) nodes[id] = this; }
  appendChild(e) { e.parentNode = this; }
  insertBefore(e) { e.parentNode = this; }
  setAttribute() {}
  set innerHTML(s) { for (const m of s.matchAll(/id="([^"]+)"/g)) new Element(m[1]); }
  getBoundingClientRect() { return this.id === 'screen' ? {left:80, top:40, width:240, height:400} : {left:330, top:200, width:56, height:56}; }
  addEventListener(type, fn) { (this.handlers[type] ||= []).push(fn); }
  setPointerCapture(id) { this.capture = id; }
  releasePointerCapture() { this.capture = null; }
  animate() {} remove() {}
}
const canvas = new Element('screen'), stage = new Element('stage');
stage.parentNode = new Element('body');
const ctx = { canvas, cm:{cols:48,rows:47,cw:10,ch:17}, live:{}, term:null,
  grid:() => [48,47], say:s => sent.push(s), performance:{now:() => now},
  document:{head:new Element(), body:new Element(), createElement:() => new Element(), getElementById:id => nodes[id]},
  window:{addEventListener:(type,fn) => (windows[type] ||= []).push(fn), parent:{studioTiltShow:(x,y,r) => shown.push([x,y,r])}},
  setTimeout:fn => fn(), requestAnimationFrame:fn => {callbacks.set(++next,fn); return next;},
  cancelAnimationFrame:id => callbacks.delete(id) };
for (const file of ['sim-tilt.js','sim-touch.js'])
  vm.runInNewContext(fs.readFileSync(path.join(__dirname,'../../app-sdk/studio/web',file),'utf8'),ctx);
function event(target, type, x=200, y=240, extra={}) {
  const e = {clientX:x,clientY:y,pointerId:1,button:0,preventDefault(){},stopImmediatePropagation(){this.stopped=true;},...extra};
  for (const fn of target.handlers[type] || []) { fn(e); if(e.stopped) break; }
}
function tilt() { return JSON.parse(sent.filter(s => s.startsWith('term tilt ')).at(-1).slice(10)); }
function tick(ms) { now += ms; const pending = [...callbacks.values()]; callbacks.clear(); pending.forEach(fn => fn(now)); }
for (const extra of [{button:2}, {shiftKey:true}]) {
  sent.length=0;
  event(canvas,'pointerdown',200,240,extra);
  event(canvas,'pointermove',280,240);
  assert.equal(tilt().x,45); assert.equal(tilt().y,0);
  event(canvas,'pointermove',10000,-10000);
  assert.ok(Math.abs(Math.hypot(tilt().x,tilt().y)-45)<1e-8);
  const count=sent.length; event(canvas,'pointermove',100,100,{pointerId:2}); assert.equal(sent.length,count);
  event(canvas,'pointerup',280,240);
  assert.equal(tilt().phase,'release');
  assert.ok(sent.every(s => !s.startsWith('term touch')));
  assert.deepEqual(shown.at(-1).slice(2),[true]); assert.ok(shown.some(v => v[0] === 45 && v[2] === false));
}
sent.length=0;
event(canvas,'pointerdown'); event(canvas,'pointermove',240,240); event(canvas,'pointerup',240,240);
assert.ok(sent.every(s=>s.startsWith('term touch'))); assert.equal(sent.length,3);
for (const end of ['pointercancel','lostpointercapture']) {
  event(canvas,'pointerdown',200,240,{button:2}); event(canvas,'pointermove',250,250);
  event(canvas,end); assert.equal(tilt().phase,'release');
}
event(canvas,'pointerdown',200,240,{button:2}); event(canvas,'pointermove',250,250);
windows.blur.forEach(fn=>fn()); assert.equal(tilt().phase,'release');
ctx.live=null; sent.length=0; event(canvas,'pointerdown',200,240,{button:2}); assert.equal(sent.length,0);
assert.equal(canvas.handlers.keydown,undefined);
console.log('PASS tilt modifiers, direction, radial clamp, Studio readout, pointer ownership/cancel/blur, plain touch');
