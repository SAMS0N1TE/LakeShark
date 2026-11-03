// This page also runs on boards and in simulator frames. Only the frozen
// local Desktop bridge opts into updater UI; all other screens stay unchanged.
(() => {
  'use strict';
  if (window.top !== window || !['/', '/index.html'].includes(location.pathname) ||
      !['127.0.0.1', 'localhost'].includes(location.hostname)) return;
  fetch('/studio/api/updater/status').then(r => r.ok ? r.json() : null).then(s => {
    if (!s || !s.frozen) return;
    window.LS_DESKTOP_UPDATER = true;
    const script = document.createElement('script');
    script.src = '/studio/static/updater.js';
    document.body.appendChild(script);
  }).catch(() => {});
})();
