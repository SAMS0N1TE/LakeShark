// Updater UI stays independent of Studio's editor and command registry.
(() => {
  'use strict';
  const desktop = window.LS_DESKTOP_UPDATER === true || document.body.dataset.updaterDesktop === 'true';
  const base = new URL('../api/updater/', document.currentScript.src).href;
  const board = document.getElementById('updater-board');
  if (board) {
    const host = new URLSearchParams(location.search).get('board');
    if (/^(?:\d{1,3}\.){3}\d{1,3}$/.test(host || '')) board.src = 'http://' + host + '/';
    board.onload = () => board.focus();
  }
  let state, banner, dismissed, busy = false;
  const node = (tag, text, cls) => {
    const n = document.createElement(tag);
    if (text) n.textContent = text;
    if (cls) n.className = cls;
    return n;
  };
  async function request(action, data) {
    const r = await fetch(base + action, data === undefined ? {} : {
      method: 'POST', headers: { 'X-Studio': '1', 'Content-Type': 'application/json' }, body: JSON.stringify(data)
    });
    const s = await r.json();
    if (!r.ok || s.ok === false) throw new Error(s.message || 'Update request failed');
    state = s;
    return s;
  }
  function button(text, fn) {
    const b = node('button', '[' + text + ']', 'key tiny');
    b.type = 'button';
    b.onclick = fn;
    return b;
  }
  function notice(text) {
    if (!banner) {
      banner = node('div', '', 'bar');
      banner.id = 'updater-notice';
      banner.setAttribute('role', 'status');
      Object.assign(banner.style, { flexBasis: '100%', whiteSpace: 'nowrap', overflowX: 'auto',
        padding: '0 1ch', background: 'var(--face, #000)', color: 'var(--text, #e9eaee)',
        borderTop: '1px solid var(--lo, #2a2e35)', font: 'inherit' });
      (desktop ? document.body : document.getElementById('toolbar')).appendChild(banner);
    }
    banner.hidden = false;
    banner.replaceChildren(document.createTextNode(text + '  '));
    return banner;
  }
  function later() {
    dismissed = state.available && state.available.version;
    banner.hidden = true;
  }
  function render(force = false) {
    const m = state.available;
    if (!m || (!force && m.version === dismissed)) return;
    const bar = notice('LakeShark ' + m.version + ' is out: ' + m.notes);
    if (state.frozen) bar.appendChild(button('Update now', install));
    else {
      const a = node('a', '[Release page]', 'key tiny');
      a.href = m.release_url;
      a.target = '_blank'; a.rel = 'noopener noreferrer';
      bar.appendChild(a);
    }
    bar.append(' ', button('Later', later));
  }
  async function check(manual) {
    if (busy) return;
    busy = true;
    try {
      await request('check', { manual });
      if (state.available) render(manual);
      else if (manual && state.checked) {
        notice('LakeShark ' + state.version + ' is up to date.').appendChild(button('Close', later));
      }
      // Offline checks are silent, including checks requested from Help.
    } catch (_) { /* offline is silent */ }
    finally { busy = false; }
  }
  async function install() {
    if (busy) return;
    busy = true;
    try {
      await request('install', { relaunch: desktop ? 'desktop' : 'studio' });
      while (true) {
        const j = state.job;
        if (j.state === 'error') throw new Error(j.message);
        if (j.state === 'launching') { notice('Starting LakeShark update...'); break; }
        const percent = j.total ? Math.floor(j.sent * 100 / j.total) : 0;
        notice('Downloading LakeShark update: ' + percent + '% (' + j.sent + '/' + j.total + ' bytes)');
        await new Promise(resolve => setTimeout(resolve, 400));
        await request('status');
      }
    } catch (e) {
      notice('Update stopped: ' + e.message).appendChild(button('Close', later));
    } finally { busy = false; }
  }
  async function toggleAutomatic() {
    try {
      await request('settings', { automatic: !state.automatic });
      notice('Automatic update checks ' + (state.automatic ? 'on' : 'off') + '.').appendChild(button('Close', later));
    } catch (e) { notice(e.message).appendChild(button('Close', later)); }
  }
  function hookHelp() {
    const menus = document.getElementById('menus');
    if (!menus) return;
    new MutationObserver(() => {
      if (!document.querySelector('[data-menu="Help"].open')) return;
      const drop = menus.querySelector('.drop');
      if (!drop || drop.querySelector('[data-updater]')) return;
      drop.appendChild(node('div', '', 'sepr'));
      for (const [text, fn] of [['Check for updates', () => check(true)],
        [(state.automatic ? '[x]' : '[ ]') + ' Automatic update checks', toggleAutomatic]]) {
        const item = node('div', text, 'it');
        item.dataset.updater = '1';
        item.setAttribute('role', 'menuitem');
        item.tabIndex = 0;
        item.onmousedown = e => e.stopPropagation();
        const activate = () => {
          document.dispatchEvent(new KeyboardEvent('keydown', { key: 'Escape' }));
          fn();
        };
        item.onclick = activate;
        item.onkeydown = e => { if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); activate(); } };
        drop.appendChild(item);
      }
    }).observe(menus, { childList: true, subtree: true });
  }
  async function init() {
    try {
      await request('status');
      if (desktop) {
        // The desktop window shows nothing until an update is out; checking by
        // hand and the automatic switch live in Studio's Help menu.
        const style = node('style', '#updater-notice .key { font: inherit; color: inherit; background: transparent; border: 0; border-radius: 0; cursor: pointer; }');
        document.head.appendChild(style);
      } else hookHelp();
      render();
      await check(false);
      setInterval(() => check(false), 60 * 60 * 1000);
    } catch (_) { /* A board or source simulator has no installed updater. */ }
  }
  init();
})();
