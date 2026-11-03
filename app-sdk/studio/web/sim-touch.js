// Studio only: screen.js supplies the displayed logical grid, already rotated
// by lssim. Use the canvas CSS bounds, never its DPR-sized backing bitmap or
// the surrounding stage (which includes letterboxing).
(() => {
  'use strict';
  let finger = null;
  canvas.style.cursor = 'crosshair';
  function position(e) {
    const box = canvas.getBoundingClientRect();
    if (!box.width || !box.height) return null;
    const [cols, rows] = grid();
    const w = cols * (cm ? cm.cw : CELL_W), h = rows * (cm ? cm.ch : CELL_H);
    return { x: Math.max(0, Math.min(w - 1, Math.floor((e.clientX - box.left) * w / box.width))),
             y: Math.max(0, Math.min(h - 1, Math.floor((e.clientY - box.top) * h / box.height))) };
  }
  function emit(phase, p) { say(`term touch ${phase} ${p.x} ${p.y}`); }
  function ring(e) {
    const dot = document.createElement('span');
    dot.style.cssText = `position:fixed;left:${e.clientX}px;top:${e.clientY}px;width:24px;height:24px;` +
      'border:2px solid #7de5ff;border-radius:50%;pointer-events:none;z-index:100;transform:translate(-50%,-50%)';
    document.body.appendChild(dot);
    dot.animate([{ opacity: .9 }, { opacity: 0, transform: 'translate(-50%,-50%) scale(1.8)' }], 350);
    setTimeout(() => dot.remove(), 350);
  }
  function cancel() {
    if (!finger) return;
    emit('cancel', finger.last);
    finger = null;
    canvas.style.cursor = 'crosshair';
  }
  // Capture keeps screen.js's terminal mouse/wheel emulation out of this path.
  // Keyboard handlers and focus remain screen.js's own.
  for (const type of ['pointerdown', 'pointermove', 'pointerup', 'pointercancel', 'lostpointercapture']) {
    canvas.addEventListener(type, e => {
      e.stopImmediatePropagation();
      if (type === 'pointerdown') {
        if (!live || (!cm && !term) || finger || e.button !== 0) return;
        const p = position(e); if (!p) return;
        e.preventDefault();
        canvas.setPointerCapture?.(e.pointerId);
        finger = { id: e.pointerId, start: p, last: p, clientX: e.clientX, clientY: e.clientY };
        canvas.style.cursor = 'grabbing';
        emit('down', p);
      } else if (finger && e.pointerId === finger.id) {
        e.preventDefault();
        if (type === 'pointercancel' || type === 'lostpointercapture') { cancel(); return; }
        const p = position(e); if (!p) { cancel(); return; }
        if (type === 'pointermove') {
          if (p.x !== finger.last.x || p.y !== finger.last.y) emit('move', p);
          finger.last = p;
        } else {
          emit('up', p);
          if (Math.abs(p.x - finger.start.x) <= 32 && Math.abs(p.y - finger.start.y) <= 32) ring(finger);
          finger = null;
          canvas.style.cursor = 'crosshair';
          canvas.releasePointerCapture?.(e.pointerId);
        }
      }
    }, { capture: true, passive: false });
  }
  window.addEventListener('blur', cancel);
  window.addEventListener('pagehide', cancel);
})();
