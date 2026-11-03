// Studio-only motion: right-drag or Shift + left-drag the screen tilts the board.
// Runs before sim-touch.js so modified drags never enter the finger channel. The
// TILT row in Studio (studio.js) has the pad, level and shake, and shows the
// angle a screen drag makes.
(() => {
  'use strict';
  let drag = null, x = 0, y = 0;
  const ready = () => live && (cm || term);
  const studio = () => { try { return window.parent !== window ? window.parent.studioTiltShow : null; } catch (e) { return null; } };
  function send(event) { if (ready()) say('term tilt ' + JSON.stringify(event)); }
  function move(nx, ny) {
    const n = Math.hypot(nx, ny), k = n > 45 ? 45 / n : 1;
    x = nx * k; y = ny * k;
    send({ phase: 'move', x, y });
    studio()?.(x, y, false);
  }
  function finish() {
    const old = drag; drag = null;
    canvas.style.cursor = 'crosshair';
    if (!old) return;
    old.target.releasePointerCapture?.(old.id);
    send({ phase: 'release' });
    studio()?.(x, y, true);
  }
  function sample(e) {
    const b = canvas.getBoundingClientRect();
    const scale = 45 / Math.max(1, Math.min(b.width, b.height) / 3);
    move((e.clientX - drag.cx) * scale, (e.clientY - drag.cy) * scale);
  }
  canvas.addEventListener('contextmenu', e => { e.preventDefault(); e.stopImmediatePropagation(); });
  for (const type of ['pointerdown', 'pointermove', 'pointerup', 'pointercancel', 'lostpointercapture']) {
    canvas.addEventListener(type, e => {
      const starts = type === 'pointerdown' && (e.button === 2 || (e.button === 0 && e.shiftKey));
      if (!starts && (!drag || e.pointerId !== drag.id)) return;
      e.stopImmediatePropagation(); e.preventDefault();
      if (starts) {
        if (!ready() || drag) return;
        drag = { target: canvas, id: e.pointerId, cx: e.clientX, cy: e.clientY };
        canvas.setPointerCapture?.(e.pointerId); canvas.style.cursor = 'grabbing'; sample(e);
      } else if (type === 'pointermove') sample(e);
      else { if (type === 'pointerup') sample(e); finish(); }
    }, { capture: true, passive: false });
  }
  window.addEventListener('blur', finish);
  window.addEventListener('pagehide', finish);
})();
