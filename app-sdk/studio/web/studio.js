// LakeShark Studio: the page. Talks to server.py (api/...) on this computer;
// the run panel is the board's own screen.js in an iframe, fed by lssim.
'use strict';
const $ = (id) => document.getElementById(id);
const esc = (s) => String(s ?? '').replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c]));
const el = (html) => { const t = document.createElement('template'); t.innerHTML = html.trim(); return t.content.firstElementChild; };
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const store = {
  get(k, d) { try { const v = localStorage.getItem(k); return v == null ? d : JSON.parse(v); } catch (e) { return d; } },
  set(k, v) { try { localStorage.setItem(k, JSON.stringify(v)); } catch (e) { /* per viewer only */ } },
};

async function api(name, body, method, raw) {
  const opt = { method: method || (body === undefined ? 'GET' : 'POST'), headers: { 'X-Studio': '1' } };
  if (body !== undefined) opt.body = raw || typeof body === 'string' ? body : JSON.stringify(body);
  const r = await fetch('api/' + name, opt);
  const j = await r.json().catch(() => ({ ok: false, message: 'no answer from Studio' }));
  if (!r.ok) throw new Error(j.message || r.statusText);
  return j;
}
const pq = (p) => 'p=' + encodeURIComponent(p);

let statusUndo = null;
function status(text, kind, undo) {
  const s = $('st-text');
  s.textContent = text;
  s.className = kind || '';
  statusUndo = undo || null;
  if (undo) { const u = el('<span class="undo"> Undo</span>'); u.onclick = () => { statusUndo = null; undo(); }; s.appendChild(u); }
}
async function copy(text, what) {
  try { await navigator.clipboard.writeText(text); status('Copied ' + (what || text.length + ' characters'), 'good'); }
  catch (e) { status('The browser would not copy: select it and press Ctrl+C', 'bad'); }
}
function download(name, text, type) {
  const a = document.createElement('a');
  a.href = typeof text === 'string' && text.startsWith('data:') ? text : URL.createObjectURL(new Blob([text], { type: type || 'text/plain' }));
  a.download = name;
  a.click();
}
const ago = (t) => { const s = Date.now() / 1000 - t; return s < 90 ? 'just now' : s < 5400 ? Math.round(s / 60) + ' min ago' : s < 129600 ? Math.round(s / 3600) + ' h ago' : Math.round(s / 86400) + ' days ago'; };

// ================================================================== settings

const SET = Object.assign({ scheme: 'bay', darkEditor: false, fontSize: 14, autoIndent: true, formatOnSave: false, keys: {} },
  store.get('studio-settings', {}));
const SCHEMES = [['bay', 'Bay 95'], ['classic', 'Classic 95'], ['contrast', 'High Contrast'], ['teal', 'Teal'], ['amber', 'Amber phosphor'], ['night', 'Night']];
function applySettings() {
  document.documentElement.dataset.scheme = SET.scheme;
  document.documentElement.dataset.darkEditor = SET.darkEditor ? '1' : '0';
  document.documentElement.style.setProperty('--ed-font', SET.fontSize + 'px');
  store.set('studio-settings', SET);
  for (const pn of panes) if (pn.view) { reconfigure(pn.view); pn.view.requestMeasure(); }
}

// ================================================================== commands

const CMDS = new Map();
function cmd(id, title, key, run, menu) { CMDS.set(id, { id, title, key: key || '', run, menu }); }
function keyOf(id) { return SET.keys[id] !== undefined ? SET.keys[id] : (CMDS.get(id) || {}).key || ''; }
function canon(combo) {                 // "Shift+Alt+F" and "Alt+Shift+F" are one key
  if (!combo) return '';
  const parts = combo.split('+').filter(Boolean);
  const key = parts.pop() || '';
  const has = (m) => parts.some((x) => x.toLowerCase() === m);
  return (has('ctrl') ? 'ctrl+' : '') + (has('alt') ? 'alt+' : '') + (has('shift') ? 'shift+' : '') + key.toLowerCase();
}
function comboOf(e) {
  const k = e.key === ' ' ? 'Space' : e.key.length === 1 ? e.key.toUpperCase() : e.key;
  if (['Control', 'Shift', 'Alt', 'Meta'].includes(k)) return '';
  return (e.ctrlKey || e.metaKey ? 'Ctrl+' : '') + (e.altKey ? 'Alt+' : '') + (e.shiftKey ? 'Shift+' : '') + (k === '+' ? 'Plus' : k);
}
function run(id) {
  const c = CMDS.get(id);
  if (!c) return;
  Promise.resolve().then(() => c.run()).catch((e) => status(e.message || String(e), 'bad'));
}
let recording = null;                 // the settings page recording a key
document.addEventListener('keydown', (e) => {
  const combo = comboOf(e);
  if (!combo) return;
  if (recording) { e.preventDefault(); e.stopPropagation(); recording(combo === 'Escape' ? null : combo); return; }
  if (!$('palette').hidden || !$('modal').hidden) return;
  if (document.activeElement && document.activeElement.classList.contains('rename')) return;
  for (const c of CMDS.values()) {
    if (keyOf(c.id) && canon(keyOf(c.id)) === canon(combo)) {
      e.preventDefault(); e.stopPropagation();
      run(c.id);
      return;
    }
  }
}, true);

// ================================================================== panels and the dock

const PANELS = {};
function panel(id, title, html, opts = {}) {
  const node = el(`<div class="panel" data-panel="${id}">${html}</div>`);
  PANELS[id] = { id, title, el: node, closable: opts.closable !== false };
  $('holder').appendChild(node);
  return node;
}
const tabsNode = (panels, active) => ({ type: 'tabs', panels, active: active || panels[0] });
const PRESETS = {
  Default: { type: 'split', dir: 'row', sizes: [17, 53, 30], children: [
    { type: 'split', dir: 'col', sizes: [64, 36], children: [tabsNode(['explorer']), tabsNode(['outline', 'search'])] },
    { type: 'split', dir: 'col', sizes: [70, 30], children: [tabsNode(['editor']), tabsNode(['problems', 'output', 'simlog', 'boardlog', 'terminal'])] },
    tabsNode(['run', 'docs', 'ai'])] },
  Code: { type: 'split', dir: 'row', sizes: [15, 65, 20], children: [
    tabsNode(['explorer', 'outline', 'search']),
    { type: 'split', dir: 'col', sizes: [80, 20], children: [tabsNode(['editor']), tabsNode(['problems', 'output', 'terminal', 'simlog', 'boardlog'])] },
    tabsNode(['run', 'docs', 'ai'], 'docs')] },
  Board: { type: 'split', dir: 'row', sizes: [13, 42, 45], children: [
    tabsNode(['explorer', 'outline']),
    { type: 'split', dir: 'col', sizes: [72, 28], children: [tabsNode(['editor']), tabsNode(['problems', 'boardlog', 'simlog', 'output', 'terminal', 'search'], 'boardlog')] },
    tabsNode(['run', 'ai', 'docs'])] },
  Debug: { type: 'split', dir: 'row', sizes: [15, 55, 30], children: [
    tabsNode(['explorer', 'outline', 'search']),
    { type: 'split', dir: 'col', sizes: [45, 55], children: [tabsNode(['editor']),
      { type: 'split', dir: 'row', sizes: [50, 50], children: [tabsNode(['problems', 'output', 'terminal']), tabsNode(['boardlog', 'simlog'])] }] },
    tabsNode(['run', 'docs', 'ai'])] },
};
let LAYOUT = null;                    // { root, floats: [{panel, x, y, w, h}] }
const clone = (o) => JSON.parse(JSON.stringify(o));
function layoutPanels(n, out = []) { if (!n) return out; if (n.type === 'tabs') out.push(...n.panels); else n.children.forEach((c) => layoutPanels(c, out)); return out; }
function allPanels() { return layoutPanels(LAYOUT.root).concat(LAYOUT.floats.map((f) => f.panel)); }
function saveLayout() { store.set('studio-layout', LAYOUT); }
function setLayout(l, name) {
  LAYOUT = { root: clone(l.root || l), floats: clone(l.floats || []) };
  if (!allPanels().includes('editor'))
    LAYOUT.root = normalize({ type: 'split', dir: 'row', sizes: [30, 70], children: [LAYOUT.root, tabsNode(['editor'])] });
  renderDock();
  if (name) status('Layout: ' + name, 'good');
}
function normalize(n) {
  if (!n) return null;
  if (n.type === 'tabs') { if (!n.panels.length) return null; if (!n.panels.includes(n.active)) n.active = n.panels[0]; return n; }
  const kids = [], sizes = [];
  n.children.forEach((c, i) => { const k = normalize(c); if (k) { kids.push(k); sizes.push(n.sizes[i] || 10); } });
  if (!kids.length) return null;
  if (kids.length === 1) return kids[0];
  const total = sizes.reduce((a, b) => a + b, 0);
  return { type: 'split', dir: n.dir, children: kids, sizes: sizes.map((s) => s * 100 / total) };
}
function removePanel(id) {
  (function walk(n) { if (!n) return; if (n.type === 'tabs') n.panels = n.panels.filter((p) => p !== id); else n.children.forEach(walk); })(LAYOUT.root);
  LAYOUT.floats = LAYOUT.floats.filter((f) => f.panel !== id);
  LAYOUT.root = normalize(LAYOUT.root);
}
function findTabsWith(id, n = LAYOUT.root) {
  if (!n) return null;
  if (n.type === 'tabs') return n.panels.includes(id) ? n : null;
  for (const c of n.children) { const f = findTabsWith(id, c); if (f) return f; }
  return null;
}
function nodeByKey(key, n = LAYOUT.root, parent = null) {
  if (!n) return null;
  if (n._key === key) return { n, parent };
  if (n.type === 'split') for (const c of n.children) { const f = nodeByKey(key, c, n); if (f) return f; }
  return null;
}
const HOME = { explorer: 'outline', outline: 'explorer', search: 'outline', problems: 'output', output: 'problems', simlog: 'problems',
  boardlog: 'problems', terminal: 'problems', run: 'docs', docs: 'run', ai: 'run', editor: null };
function showPanel(id) {
  if (!allPanels().includes(id)) {
    const near = HOME[id] && findTabsWith(HOME[id]);
    if (near) { near.panels.push(id); near.active = id; }
    else LAYOUT.root = { type: 'split', dir: 'row', sizes: [75, 25], children: [LAYOUT.root, tabsNode([id])] };
    renderDock();
  } else {
    const t = findTabsWith(id);
    if (t && t.active !== id) activateTab(t, id);
  }
}
// Another tab of a group: the panels stay where they are, only shown or hidden,
// so the simulator's page is never reloaded by a tab click.
function activateTab(n, id) {
  n.active = id;
  const box = document.querySelector(`.tabsnode[data-key="${n._key}"]`);
  if (!box) { renderDock(); return; }
  box.querySelectorAll('.ttab').forEach((x) => x.classList.toggle('sel', x.dataset.panel === id));
  for (const pid of n.panels) PANELS[pid].el.hidden = pid !== id;
  saveLayout();
  for (const pn of panes) if (pn.view) pn.view.requestMeasure();
  if (id === 'docs' && !$('doc-list').children.length) listDocs();
}
function dockPanel(id, key, zone) {
  const target = nodeByKey(key);
  if (!target) return;
  const tn = target.n;
  if (zone === 'center') {
    if (tn.type === 'tabs' && tn.panels.includes(id)) { tn.active = id; renderDock(); return; }
    removePanel(id);
    const t2 = nodeByKey(key);
    if (t2 && t2.n.type === 'tabs') { t2.n.panels.push(id); t2.n.active = id; }
    else LAYOUT.root = { type: 'split', dir: 'row', sizes: [70, 30], children: [LAYOUT.root, tabsNode([id])] };
  } else {
    const fresh = tabsNode([id]);
    removePanel(id);
    const t2 = nodeByKey(key);
    const dir = zone === 'left' || zone === 'right' ? 'row' : 'col';
    const first = zone === 'left' || zone === 'top';
    if (!t2) { LAYOUT.root = { type: 'split', dir, sizes: first ? [30, 70] : [70, 30], children: first ? [fresh, LAYOUT.root] : [LAYOUT.root, fresh] }; }
    else {
      const wrap = { type: 'split', dir, sizes: first ? [35, 65] : [65, 35], children: first ? [fresh, t2.n] : [t2.n, fresh] };
      if (!t2.parent) LAYOUT.root = wrap;
      else t2.parent.children[t2.parent.children.indexOf(t2.n)] = wrap;
    }
  }
  LAYOUT.root = normalize(LAYOUT.root);
  renderDock();
}
function floatPanel(id, x, y) {
  removePanel(id);
  LAYOUT.floats.push({ panel: id, x: x ?? 200, y: y ?? 120, w: id === 'run' ? 420 : 560, h: id === 'run' ? 720 : 380 });
  renderDock();
}
function closePanel(id) {
  if (!PANELS[id].closable) { status('The editor stays: drag it somewhere else instead', 'bad'); return; }
  removePanel(id);
  renderDock();
}
let keyN = 0;
function renderDock() {
  const dock = $('dock');
  for (const p of Object.values(PANELS)) $('holder').appendChild(p.el);
  dock.innerHTML = '';
  if (LAYOUT.root) dock.appendChild(buildNode(LAYOUT.root));
  $('floats').innerHTML = '';
  for (const f of LAYOUT.floats) $('floats').appendChild(buildFloat(f));
  saveLayout();
  for (const pn of panes) if (pn.view) pn.view.requestMeasure();
  updateMarks();
}
function buildNode(n) {
  n._key = 'k' + (++keyN);
  if (n.type === 'tabs') return buildTabs(n);
  const box = el(`<div class="split ${n.dir}"></div>`);
  n.children.forEach((c, i) => {
    if (i) box.appendChild(splitter(n, i, box));
    const cell = el('<div class="cell"></div>');
    cell.style.flex = `${n.sizes[i]} 1 0`;
    cell.appendChild(buildNode(c));
    box.appendChild(cell);
  });
  return box;
}
function splitter(n, i, box) {
  const s = el('<div class="splitter"></div>');
  s.addEventListener('pointerdown', (e) => {
    e.preventDefault();
    s.setPointerCapture(e.pointerId);
    s.classList.add('drag');
    const cells = [...box.children].filter((c) => c.classList.contains('cell'));
    const a = cells[i - 1], b = cells[i];
    const ra = a.getBoundingClientRect(), rb = b.getBoundingClientRect();
    const row = n.dir === 'row', start = row ? e.clientX : e.clientY;
    const lenA = row ? ra.width : ra.height, lenB = row ? rb.width : rb.height, both = n.sizes[i - 1] + n.sizes[i];
    const move = (ev) => {
      const d = (row ? ev.clientX : ev.clientY) - start;
      const na = Math.max(40, Math.min(lenA + lenB - 40, lenA + d));
      n.sizes[i - 1] = both * na / (lenA + lenB);
      n.sizes[i] = both - n.sizes[i - 1];
      a.style.flex = `${n.sizes[i - 1]} 1 0`;
      b.style.flex = `${n.sizes[i]} 1 0`;
      for (const pn of panes) if (pn.view) pn.view.requestMeasure();
    };
    const up = () => { s.classList.remove('drag'); s.removeEventListener('pointermove', move); saveLayout(); updateMarks(); };
    s.addEventListener('pointermove', move);
    s.addEventListener('pointerup', up, { once: true });
  });
  return s;
}
function buildTabs(n) {
  const box = el(`<div class="tabsnode" data-key="${n._key}"><div class="tnhead"></div><div class="tnbody"></div></div>`);
  const head = box.firstChild;
  for (const id of n.panels) {
    const t = el(`<span class="ttab${id === n.active ? ' sel' : ''}" data-panel="${id}">${esc(PANELS[id].title)}${id === 'problems' ? ' <b id="n-problems"></b>' : ''}</span>`);
    t.addEventListener('pointerdown', (e) => startPanelDrag(e, id, () => activateTab(n, id)));
    t.addEventListener('contextmenu', (e) => { e.preventDefault(); ctx(e, panelMenu(id)); });
    head.appendChild(t);
  }
  const keys = el(`<span class="tnkeys"><span data-a="float" title="Float">[^]</span><span data-a="close" title="Close">[x]</span></span>`);
  keys.querySelector('[data-a=float]').onclick = () => floatPanel(n.active);
  keys.querySelector('[data-a=close]').onclick = () => closePanel(n.active);
  head.appendChild(keys);
  for (const pid of n.panels) { PANELS[pid].el.hidden = pid !== n.active; box.lastChild.appendChild(PANELS[pid].el); }
  box.addEventListener('pointerdown', () => { document.querySelectorAll('.tabsnode.focus').forEach((x) => x.classList.remove('focus')); box.classList.add('focus'); }, true);
  showProblemCount();
  return box;
}
function buildFloat(f) {
  const box = el(`<div class="float"><div class="tnhead"><span class="ttab sel">${esc(PANELS[f.panel].title)}</span>` +
    `<span class="tnkeys"><span data-a="dock" title="Back in the dock">[dock]</span><span data-a="close">[x]</span></span></div><div class="tnbody"></div></div>`);
  Object.assign(box.style, { left: f.x + 'px', top: f.y + 'px', width: f.w + 'px', height: f.h + 'px' });
  PANELS[f.panel].el.hidden = false;
  box.lastChild.appendChild(PANELS[f.panel].el);
  box.querySelector('[data-a=dock]').onclick = () => { LAYOUT.floats = LAYOUT.floats.filter((x) => x !== f); showPanel(f.panel); };
  box.querySelector('[data-a=close]').onclick = () => closePanel(f.panel);
  const head = box.firstChild;
  head.querySelector('.ttab').addEventListener('pointerdown', (e) => startPanelDrag(e, f.panel, null, f));
  head.addEventListener('pointerdown', (e) => {
    if (e.target.closest('.tnkeys, .ttab')) return;
    e.preventDefault();
    const sx = e.clientX - f.x, sy = e.clientY - f.y;
    head.setPointerCapture(e.pointerId);
    const move = (ev) => { f.x = ev.clientX - sx; f.y = Math.max(0, ev.clientY - sy); box.style.left = f.x + 'px'; box.style.top = f.y + 'px'; };
    window.addEventListener('pointermove', move);
    window.addEventListener('pointerup', () => { window.removeEventListener('pointermove', move); saveLayout(); }, { once: true });
  });
  new ResizeObserver(() => { f.w = box.offsetWidth; f.h = box.offsetHeight; saveLayout(); for (const pn of panes) if (pn.view) pn.view.requestMeasure(); }).observe(box);
  return box;
}
// Drag a panel by its tab: drop zones on every panel group (edges split, middle joins), or float it off the dock.
function startPanelDrag(e, id, onClick, fromFloat) {
  if (e.button !== 0) return;
  const sx = e.clientX, sy = e.clientY;
  try { e.target.setPointerCapture(e.pointerId); } catch (x) { /* fine */ }
  let dragging = false, target = null;
  const zone = $('dropzone'), ghost = $('dragghost');
  const move = (ev) => {
    if (!dragging && Math.hypot(ev.clientX - sx, ev.clientY - sy) < 6) return;
    if (!dragging) { dragging = true; ghost.textContent = PANELS[id].title; ghost.hidden = false; }
    ghost.style.left = ev.clientX + 12 + 'px';
    ghost.style.top = ev.clientY + 8 + 'px';
    zone.hidden = true;
    ghost.hidden = true;
    const under = document.elementFromPoint(ev.clientX, ev.clientY);
    ghost.hidden = false;
    const tn = under && under.closest('#dock .tabsnode');
    target = null;
    if (tn) {
      const r = tn.getBoundingClientRect();
      const fx = (ev.clientX - r.left) / r.width, fy = (ev.clientY - r.top) / r.height;
      let z = 'center';
      const m = Math.min(fx, 1 - fx, fy, 1 - fy);
      if (m < 0.25) z = m === fx ? 'left' : m === 1 - fx ? 'right' : m === fy ? 'top' : 'bottom';
      if (ev.clientY - r.top < 22) z = 'center';
      target = { key: tn.dataset.key, zone: z };
      const box = { left: r.left, top: r.top, width: r.width, height: r.height };
      if (z === 'left') box.width /= 2; if (z === 'right') { box.left += r.width / 2; box.width /= 2; }
      if (z === 'top') box.height /= 2; if (z === 'bottom') { box.top += r.height / 2; box.height /= 2; }
      Object.assign(zone.style, { left: box.left + 'px', top: box.top + 'px', width: box.width + 'px', height: box.height + 'px' });
      zone.hidden = false;
    }
  };
  const up = (ev) => {
    window.removeEventListener('pointermove', move);
    zone.hidden = true; ghost.hidden = true;
    if (!dragging) { if (onClick) onClick(); return; }
    if (target) { if (fromFloat) LAYOUT.floats = LAYOUT.floats.filter((x) => x !== fromFloat); dockPanel(id, target.key, target.zone); }
    else if (!fromFloat && !ev.target.closest('#dock')) floatPanel(id, ev.clientX - 100, ev.clientY - 10);
  };
  window.addEventListener('pointermove', move);
  window.addEventListener('pointerup', up, { once: true });
}
function panelMenu(id) {
  return [['Float', '', () => floatPanel(id)], ['Close', '', () => closePanel(id)], null,
    ['Move to the left', '', () => dockEdge(id, 'left')], ['Move to the right', '', () => dockEdge(id, 'right')],
    ['Move to the bottom', '', () => dockEdge(id, 'bottom')]].concat(id === 'run' ? [null, ['Pop out the simulator', '', popOut]] : []);
}
function dockEdge(id, zone) {
  removePanel(id);
  const t = tabsNode([id]);
  const dir = zone === 'bottom' ? 'col' : 'row';
  LAYOUT.root = { type: 'split', dir, sizes: zone === 'left' ? [25, 75] : [75, 25], children: zone === 'left' ? [t, LAYOUT.root] : [LAYOUT.root, t] };
  LAYOUT.root = normalize(LAYOUT.root);
  renderDock();
}
function layoutNames() { return Object.keys(store.get('studio-layouts', {})); }
function saveLayoutAs() {
  const d = dialog('Save Layout', `<label class="field"><span>Name</span> <input type="text" id="lay-name" spellcheck="false" value="My layout"></label>` +
    `<p class="dlg-note">Saved layouts are under View > Layout. Presets: ${Object.keys(PRESETS).join(', ')}.</p>`,
  [{ label: 'Save', fn: () => {
    const n = $('lay-name').value.trim();
    if (!n) return;
    const all = store.get('studio-layouts', {});
    all[n] = { root: LAYOUT.root, floats: LAYOUT.floats };
    store.set('studio-layouts', all);
    closeDialog(); status('Saved the layout "' + n + '"', 'good');
  } }, { label: 'Cancel', fn: closeDialog }]);
  d.querySelector('#lay-name').select();
}

// ================================================================== menus

function ctx(e, items) {
  e.preventDefault();
  closeMenus();
  showDrop(e.clientX, e.clientY, items);
}
function showDrop(x, y, items, under) {
  const d = document.createElement('div');
  d.className = 'drop';
  for (const it of items) {
    if (!it) { d.insertAdjacentHTML('beforeend', '<div class="sepr"></div>'); continue; }
    const [label, key, fn, opts = {}] = it;
    const row = el(`<div class="it${opts.off ? ' off' : ''}">${opts.check !== undefined ? `<span class="chk">${opts.check ? '\u221a' : ''}</span>` : ''}<span>${esc(label)}</span><span class="r">${esc(key || '')}</span></div>`);
    if (opts.sub) row.addEventListener('mouseenter', () => {
      d.querySelectorAll('.drop').forEach((x) => x.remove());
      document.querySelectorAll('.drop.sub').forEach((x) => x.remove());
      const r = row.getBoundingClientRect();
      const s = showDrop(r.right - 4, r.top - 2, opts.sub());
      s.classList.add('sub');
    });
    else row.addEventListener('mouseenter', () => document.querySelectorAll('.drop.sub').forEach((x) => { if (!d.classList.contains('sub')) x.remove(); }));
    if (!opts.off && fn) row.addEventListener('mouseup', () => { closeMenus(); fn(); });
    d.appendChild(row);
  }
  $('menus').appendChild(d);
  const r = d.getBoundingClientRect();
  d.style.left = Math.max(0, Math.min(x, innerWidth - r.width - 4)) + 'px';
  d.style.top = Math.max(0, Math.min(y, innerHeight - r.height - 4)) + 'px';
  return d;
}
function closeMenus() { $('menus').innerHTML = ''; document.querySelectorAll('.menu.open').forEach((m) => m.classList.remove('open')); openMenu = null; }
document.addEventListener('mousedown', (e) => { if (!e.target.closest('.drop, .menu')) closeMenus(); });
document.addEventListener('keydown', (e) => { if (e.key === 'Escape') closeMenus(); });

const C = (id, label) => [label || CMDS.get(id).title, keyOf(id), () => run(id)];
function recentSub() {
  const rows = (S.recent || []).slice(0, 14).map((r) => [(r.pinned ? '* ' : '  ') + r.name + '   ' + (r.opened ? ago(r.opened) : 'not opened yet'), '', () => openProject(r.path), { off: !r.exists }]);
  return rows.length ? rows.concat([null, C('app.openFolder', 'Open folder...')]) : [['(none yet)', '', null, { off: true }]];
}
function layoutSub() {
  const saved = layoutNames();
  return Object.keys(PRESETS).map((n) => [n, '', () => setLayout(PRESETS[n], n)])
    .concat(saved.length ? [null] : [], saved.map((n) => [n, '', () => setLayout(store.get('studio-layouts', {})[n], n)]),
      [null, ['Save current layout as...', '', saveLayoutAs], ['Reset to the default', '', () => setLayout(PRESETS.Default, 'Default')],
        ['Delete a saved layout', '', null, { off: !saved.length, sub: () => saved.map((n) => [n, '', () => {
          const all = store.get('studio-layouts', {}); delete all[n]; store.set('studio-layouts', all); status('Deleted the layout "' + n + '"');
        }]) }]]);
}
function panelsSub() { return Object.values(PANELS).map((p) => [p.title, '', () => (allPanels().includes(p.id) ? closePanel(p.id) : showPanel(p.id)), { check: allPanels().includes(p.id) }]); }
const MENUS = {
  File: () => [C('app.new', 'New app...'), C('app.openFolder', 'Open folder...'), ['Open recent', '', null, { sub: recentSub }], C('app.back'), C('app.close'), null,
    C('file.newFile', 'New file...'), C('file.newFolder', 'New folder...'), C('file.quickOpen'), null, C('file.save'), C('file.saveAll'), C('file.closeTab'), null,
    C('view.start'), C('view.settings', 'Settings...')],
  Edit: () => [['Undo', 'Ctrl+Z', () => ed() && CM.undo(ed())], ['Redo', 'Ctrl+Y', () => ed() && CM.redo(ed())], null,
    ['Cut', 'Ctrl+X', () => clip('cut')], ['Copy', 'Ctrl+C', () => clip('copy')], ['Paste', 'Ctrl+V', () => clip('paste')], ['Select all', 'Ctrl+A', () => ed() && CM.selectAll(ed())], null,
    C('edit.find'), C('edit.replace'), C('edit.gotoLine'), C('view.search'), null, C('edit.comment'), C('edit.format'), C('edit.selectNext'), C('edit.gotoDef'), null, C('file.undoDelete')],
  Build: () => [C('build.build'), C('build.run'), C('build.stop'), null, C('build.nextProblem'), C('build.copyLog'), C('build.copyProblems')],
  Board: () => [C('board.install'), C('board.open'), null, C('board.log'), ['Connect to the console', '', () => setTail(true), { check: !!S.tailing }]],
  Sim: () => [C('sim.restart'), C('sim.rotate'), ['Board without a radio', '', () => run('sim.noRadio'), { check: !!noRadio }], C('sim.shot'), C('sim.popout')],
  View: () => [C('view.palette'), ['Layout', '', null, { sub: layoutSub }], ['Panels', '', null, { sub: panelsSub }], null, C('view.split'), C('view.problems'),
    C('view.terminal'), C('view.search', 'Search in the project'), null,
    ['Scheme', '', null, { sub: () => SCHEMES.map(([k, n]) => [n, '', () => { SET.scheme = k; applySettings(); }, { check: SET.scheme === k }]) }],
    ['Dark editor', '', () => { SET.darkEditor = !SET.darkEditor; applySettings(); }, { check: SET.darkEditor }],
    ['Bigger text', 'Ctrl+=', () => fontStep(1)], ['Smaller text', 'Ctrl+-', () => fontStep(-1)]],
  AI: () => [['AI panel', '', () => showPanel('ai')], C('ai.connect'), C('ai.vscode')],
  Help: () => [C('view.start'), C('view.help'), ['Your first app in 5 minutes', '', firstApp], ['API docs', '', () => showPanel('docs')], null, ['About Studio', '', about]],
};
let openMenu = null;
function buildMenubar() {
  const bar = $('menubar');
  for (const name of Object.keys(MENUS)) {
    const m = el(`<div class="menu" data-menu="${name}"><u>${name[0]}</u>${name.slice(1)}</div>`);
    m.addEventListener('mousedown', (e) => {
      e.preventDefault();
      const was = openMenu === name;
      closeMenus();
      if (was) return;
      openMenu = name;
      m.classList.add('open');
      const r = m.getBoundingClientRect();
      showDrop(r.left, r.bottom, MENUS[name]());
    });
    m.addEventListener('mouseenter', () => { if (openMenu && openMenu !== name) m.dispatchEvent(new MouseEvent('mousedown')); });
    bar.appendChild(m);
  }
}
// Right-click in a field: Studio's menu, not the browser's.
document.addEventListener('contextmenu', (e) => {
  if (e.defaultPrevented) return;
  const f = e.target.closest('input[type=text], input[type=search], textarea, input:not([type])');
  if (f) {
    ctx(e, [['Cut', 'Ctrl+X', () => { f.focus(); document.execCommand('cut'); }], ['Copy', 'Ctrl+C', () => copy(f.value.slice(f.selectionStart, f.selectionEnd) || f.value)],
      ['Paste', 'Ctrl+V', async () => { try { const t = await navigator.clipboard.readText(); f.focus(); document.execCommand('insertText', false, t); } catch (x) { status('Press Ctrl+V to paste here', 'bad'); } }],
      ['Select all', 'Ctrl+A', () => { f.focus(); f.select(); }]]);
    return;
  }
  const sel = String(getSelection());
  ctx(e, [['Copy selection', 'Ctrl+C', () => copy(sel), { off: !sel }], null, C('view.palette'), ['Layout', '', null, { sub: layoutSub }]]);
});

// ================================================================== projects

let S = { recent: [], templates: [], sim: {} };
const projects = [];                   // open project tabs
let P = null;                          // the one showing
const mru = [];
function proj(path) { return projects.find((p) => p.path === path); }
function dirtyFiles(p) { return [...p.files.entries()].filter(([, b]) => b.kind === 'text' && docOf(p, b).toString() !== b.saved).map(([k]) => k); }
function docOf(p, b) { for (const pn of panes) if (pn.view && pn.path === b.path && p === P) return pn.view.state.doc; return b.state.doc; }
async function refreshState() { S = await api('state'); return S; }
async function openProject(path, opts = {}) {
  let p = proj(path);
  if (!p) {
    const info = await api('project?' + pq(path));
    p = { path: info.path, name: info.name, id: info.id, tree: info.tree, files: new Map(), order: [], active: null,
      split: null, expanded: new Set(), sel: new Set(), anchor: null, problems: [], built: null };
    projects.push(p);
  }
  await switchProject(p, opts);
  refreshState().then(drawStartIfShown);
  return p;
}
async function switchProject(p, opts = {}) {
  stashViews();
  if (P && P !== p) { const i = mru.indexOf(P.path); if (i >= 0) mru.splice(i, 1); mru.unshift(P.path); }
  P = p;
  store.set('studio-open', { paths: projects.map((x) => x.path), active: p.path });
  $('title-app').textContent = '- ' + p.name;
  drawProjTabs();
  drawTree();
  if (!opts.noFile) {
    const want = p.active || (p.tree.find((f) => f.name === 'main.c') || p.tree.find((f) => !f.dir && f.kind === 'text') || {}).path;
    if (want) await openFile(want); else showStart();
  }
  showProblems(p.problems);
  drawOutline();
  gitStatus();
}
function stashViews() {
  if (!P) return;
  for (const pn of panes) if (pn.view && pn.path && P.files.has(pn.path)) P.files.get(pn.path).state = pn.view.state;
}
function drawProjTabs() {
  const bar = $('projtabs');
  bar.innerHTML = '<span class="ptlabel">Apps</span>';
  for (const p of projects) {
    const dirty = dirtyFiles(p).length;
    const t = el(`<span class="ptab${p === P ? ' sel' : ''}" title="${esc(p.path)}"><span>${esc(p.name)}</span>${dirty ? '<span class="dot">\u25cf</span>' : ''}<span class="x" title="Close the app">x</span></span>`);
    t.onclick = (e) => { if (e.target.classList.contains('x')) closeProject(p); else switchProject(p); };
    t.onauxclick = (e) => { if (e.button === 1) closeProject(p); };
    t.oncontextmenu = (e) => ctx(e, [['Close', '', () => closeProject(p)], ['Close the others', '', () => projects.filter((x) => x !== p).forEach((x) => closeProject(x))],
      ['Reveal in Explorer', '', () => api('reveal?' + pq(p.path), {})], ['Copy path', '', () => copy(p.path, 'the path')]]);
    bar.appendChild(t);
  }
  const add = el('<span class="ptab add" title="New app">+ New</span>');
  add.onclick = () => run('app.new');
  const start = el('<span class="ptab add" title="The start page">Start</span>');
  start.onclick = () => run('view.start');
  bar.append(add, start);
}
async function closeProject(p) {
  const dirty = dirtyFiles(p);
  if (dirty.length) {
    const ans = await ask(`Close ${p.name}?`, `<p>${esc(p.name)} has unsaved changes in ${esc(dirty.join(', '))}.</p>`, ['Save and close', "Close, don't save", 'Cancel']);
    if (ans === 2 || ans < 0) return;
    if (ans === 0) { const was = P; await switchProject(p, { noFile: true }); await saveAll(); if (was !== p) await switchProject(was); }
  }
  projects.splice(projects.indexOf(p), 1);
  if (P === p) {
    P = null;
    const back = mru.map(proj).find(Boolean) || projects[0];
    if (back) await switchProject(back); else { $('title-app').textContent = ''; drawProjTabs(); drawTree(); showStart(); }
  } else drawProjTabs();
  store.set('studio-open', { paths: projects.map((x) => x.path), active: P && P.path });
}
async function backProject() {
  const back = mru.map(proj).find((p) => p && p !== P);
  if (back) switchProject(back); else status('No other app is open: File > Open recent', 'bad');
}

// ================================================================== explorer

const ICON = { c: ['C', 'c'], h: ['H', 'h'], json: ['{}', 'j'], md: ['M', 'md'], txt: ['T', 'o'], obj: ['3D', 'o'], py: ['Py', 'o'] };
function iconOf(f) {
  if (f.dir) return ['+', 'dir'];
  if (f.kind === 'image') return ['[]', 'img'];
  const ext = f.name.includes('.') ? f.name.split('.').pop().toLowerCase() : '';
  return ICON[ext] || ['-', 'o'];
}
let visible = [];                      // rows as drawn, for shift-select and keys
function drawTree() {
  const t = $('tree');
  if (!P) { t.innerHTML = '<div class="none">No app open. File > New app, or Open recent.</div>'; return; }
  const q = $('tree-q').value.trim().toLowerCase();
  const rows = [];
  const match = (f) => !q || f.name.toLowerCase().includes(q) || (f.children || []).some(match);
  (function walk(list, depth) {
    for (const f of list) {
      if (!match(f)) continue;
      rows.push({ f, depth });
      if (f.dir && (P.expanded.has(f.path) || q)) walk(f.children, depth + 1);
    }
  })(P.tree, 0);
  visible = rows.map((r) => r.f);
  t.innerHTML = `<div class="root" data-path="">${esc(P.name)}/</div>`;
  for (const { f, depth } of rows) {
    const [ic, cls] = iconOf(f);
    const b = P.files.get(f.path);
    const dirty = b && b.kind === 'text' && docOf(P, b).toString() !== b.saved;
    const row = el(`<div class="row${P.sel.has(f.path) ? ' sel' : ''}${f.path === curPath() ? ' cur' : ''}${dirty ? ' dirty' : ''}" draggable="true" data-path="${esc(f.path)}">` +
      `<span style="width:${depth * 2}ch"></span><span class="tw">${f.dir ? (P.expanded.has(f.path) || q ? 'v' : '>') : ''}</span>` +
      `<span class="ico ${cls}">${ic}</span><span class="nm">${esc(f.name)}</span></div>`);
    t.appendChild(row);
  }
  if (!rows.length) t.insertAdjacentHTML('beforeend', '<div class="none">Nothing matches.</div>');
}
function findEntry(path, list = P && P.tree) { for (const f of list || []) { if (f.path === path) return f; if (f.dir) { const g = findEntry(path, f.children); if (g) return g; } } return null; }
function treeClick(e) {
  const row = e.target.closest('.row, .root');
  if (!row || !P) return;
  const path = row.dataset.path;
  if (!path) { P.sel.clear(); drawTree(); return; }
  const f = findEntry(path);
  if (e.ctrlKey || e.metaKey) { P.sel.has(path) ? P.sel.delete(path) : P.sel.add(path); P.anchor = path; }
  else if (e.shiftKey && P.anchor) {
    const a = visible.findIndex((x) => x.path === P.anchor), b = visible.findIndex((x) => x.path === path);
    P.sel = new Set(visible.slice(Math.min(a, b), Math.max(a, b) + 1).map((x) => x.path));
  } else {
    P.sel = new Set([path]); P.anchor = path;
    if (f.dir) { P.expanded.has(path) ? P.expanded.delete(path) : P.expanded.add(path); }
    else openFile(path);
  }
  drawTree();
}
function selectedPaths(fallback) { const s = [...P.sel]; return s.length ? s : fallback ? [fallback] : []; }
async function reloadTree() { const info = await api('tree?' + pq(P.path)); P.tree = info.tree; P.id = info.id; drawTree(); }
function folderOf(path) { const f = path && findEntry(path); return !path ? '' : f && f.dir ? path : path.includes('/') ? path.slice(0, path.lastIndexOf('/')) : ''; }
async function newFile(kind, base) {
  if (!P) return run('app.new');
  const dir = folderOf(base ?? [...P.sel][0] ?? '');
  const name = await promptText(kind === 'dir' ? 'New folder' : 'New file', kind === 'dir' ? 'Folder name' : 'File name (.c, .h, .json, .md ...)', kind === 'dir' ? 'assets' : 'extra.c');
  if (!name) return;
  const path = (dir ? dir + '/' : '') + name;
  const text = /\.h$/.test(name) ? '#pragma once\n#include "lakeshark_app.h"\n' : /\.c$/.test(name) ? '#include "lakeshark_app.h"\n' : '';
  const r = await api('fs?' + pq(P.path), kind === 'dir' ? { op: 'mkdir', path } : { op: 'newfile', path, text });
  if (dir) P.expanded.add(dir);
  await reloadTree();
  if (kind !== 'dir') openFile(r.path);
}
function startRename(path) {
  const row = [...$('tree').querySelectorAll('.row')].find((r) => r.dataset.path === path);
  if (!row) return;
  const nm = row.querySelector('.nm');
  const old = nm.textContent;
  const input = el(`<input class="rename" type="text" spellcheck="false" value="${esc(old)}">`);
  nm.replaceWith(input);
  input.focus();
  input.setSelectionRange(0, old.includes('.') ? old.lastIndexOf('.') : old.length);
  let done = false;
  const finish = async (ok) => {
    if (done) return; done = true;
    const v = input.value.trim();
    if (ok && v && v !== old) {
      const to = (path.includes('/') ? path.slice(0, path.lastIndexOf('/') + 1) : '') + v;
      try { await api('fs?' + pq(P.path), { op: 'rename', pairs: [[path, to]] }); renamedBuffers([[path, to]]); }
      catch (e) { status(e.message, 'bad'); }
    }
    await reloadTree();
  };
  input.addEventListener('keydown', (e) => { if (e.key === 'Enter') finish(true); if (e.key === 'Escape') finish(false); e.stopPropagation(); });
  input.addEventListener('blur', () => finish(true));
}
function renamedBuffers(pairs) {
  for (const [a, b] of pairs) {
    for (const [k, buf] of [...P.files]) {
      if (k === a || k.startsWith(a + '/')) {
        const nk = b + k.slice(a.length);
        P.files.delete(k); buf.path = nk; P.files.set(nk, buf);
        P.order = P.order.map((x) => (x === k ? nk : x));
        for (const pn of panes) if (pn.path === k) pn.path = nk;
        if (P.active === k) P.active = nk;
      }
    }
  }
  drawFileTabs();
}
async function moveTo(paths, dir) {
  const pairs = paths.filter((p) => p !== dir && folderOf(p) !== dir).map((p) => [p, (dir ? dir + '/' : '') + p.split('/').pop()]);
  if (!pairs.length) return;
  try { await api('fs?' + pq(P.path), { op: 'move', pairs }); renamedBuffers(pairs); status(`Moved ${pairs.length} into ${dir || P.name + '/'}`, 'good'); }
  catch (e) { status(e.message, 'bad'); }
  if (dir) P.expanded.add(dir);
  await reloadTree();
}
async function deletePaths(paths) {
  if (!paths.length) return;
  const ans = await ask('Delete', `<p>Move ${paths.length === 1 ? esc(paths[0]) : paths.length + ' items'} to Studio's trash?</p><p class="dlg-note">Edit > Undo delete puts ${paths.length === 1 ? 'it' : 'them'} back.</p>`, ['Delete', 'Cancel']);
  if (ans !== 0) return;
  await api('fs?' + pq(P.path), { op: 'delete', paths });
  for (const p of paths) for (const k of [...P.files.keys()]) if (k === p || k.startsWith(p + '/')) closeFile(k, true);
  P.sel.clear();
  await reloadTree();
  status(`Moved ${paths.length} to Studio's trash.`, 'good', undoDelete);
}
async function undoDelete() {
  if (!P) return;
  try { const r = await api('fs?' + pq(P.path), { op: 'undo' }); status('Put back ' + r.restored.length + ' item(s)', 'good'); }
  catch (e) { status(e.message, 'bad'); }
  await reloadTree();
}
async function duplicate(paths) { const r = await api('fs?' + pq(P.path), { op: 'duplicate', paths }); await reloadTree(); P.sel = new Set(r.paths); drawTree(); }
async function uploadFiles(files, dir) {
  let n = 0;
  for (const f of files) {
    const buf = await f.arrayBuffer();
    await api('upload?' + pq(P.path) + '&path=' + encodeURIComponent((dir ? dir + '/' : '') + f.name), buf, 'POST', true);
    n++;
  }
  if (dir) P.expanded.add(dir);
  await reloadTree();
  status(`Added ${n} file(s) from the PC`, 'good');
}
function treeMenu(e) {
  const row = e.target.closest('.row, .root');
  const path = row && row.dataset.path;
  if (path && !P.sel.has(path)) { P.sel = new Set([path]); P.anchor = path; drawTree(); }
  const sel = path ? selectedPaths(path) : [];
  const f = path && findEntry(path);
  ctx(e, [['New file...', keyOf('file.newFile'), () => newFile('file', path || '')], ['New folder...', '', () => newFile('dir', path || '')], null,
    ['Open', 'Enter', () => f && !f.dir && openFile(path), { off: !f || f.dir }], ['Open to the side', '', () => openSplit(path), { off: !f || f.dir || f.kind !== 'text' }],
    ['Rename', 'F2', () => startRename(path), { off: !path || sel.length > 1 }], ['Duplicate', '', () => duplicate(sel), { off: !sel.length }],
    ['Delete', 'Del', () => deletePaths(sel), { off: !sel.length }], null,
    ['Reveal in Explorer', '', () => api('reveal?' + pq(P.path) + (path ? '&path=' + encodeURIComponent(path) : ''), {})],
    ['Copy path', '', () => copy(sel.length ? sel.map((p) => P.path + '\\' + p.replace(/\//g, '\\')).join('\n') : P.path, 'the path')],
    ['Copy relative path', '', () => copy(sel.join('\n') || '.', 'the relative path')], null,
    ['Undo delete', '', undoDelete], ['Refresh', '', reloadTree]]);
}

// ================================================================== editor

const panes = [{ id: 'A', view: null, path: null }, { id: 'B', view: null, path: null }];
let focusPane = panes[0];
function ed() { return focusPane.view && focusPane.path && P && P.files.get(focusPane.path)?.kind === 'text' ? focusPane.view : panes[0].view; }
function curPath() { return focusPane.path || panes[0].path; }
const remote = CM.Annotation.define();
const indentComp = new CM.Compartment(), enterComp = new CM.Compartment();
const t = CM.tags;
const v = (n) => `var(--s-${n})`;
const HIGHLIGHT = CM.HighlightStyle.define([
  { tag: [t.keyword, t.controlKeyword, t.operatorKeyword, t.modifier, t.definitionKeyword], color: v('kw') },
  { tag: [t.typeName, t.standard(t.typeName)], color: v('type') },
  { tag: [t.string, t.character, t.special(t.string)], color: v('str') },
  { tag: [t.number, t.bool, t.null], color: v('num') },
  { tag: [t.lineComment, t.blockComment, t.comment], color: v('com'), fontStyle: 'italic' },
  { tag: [t.processingInstruction, t.macroName], color: v('pre') },
  { tag: [t.function(t.variableName), t.function(t.definition(t.variableName))], color: v('fn') },
  { tag: [t.definition(t.variableName)], color: v('def') },
  { tag: [t.propertyName], color: v('prop') },
  { tag: [t.operator, t.punctuation, t.bracket, t.separator], color: v('op') },
]);
const C_WORDS = ('static const void int char bool float double unsigned signed long short struct union enum typedef return if else for while do ' +
  'switch case default break continue sizeof true false NULL uint8_t uint16_t uint32_t uint64_t int8_t int16_t int32_t int64_t size_t').split(' ');
let apiData = null, apiIndex = new Map();
function completions(ctx) {
  const w = ctx.matchBefore(/[A-Za-z_]\w*/);
  if (!w || (w.from === w.to && !ctx.explicit)) return null;
  const opts = [];
  if (apiData) for (const e of apiData.entries) {
    opts.push({ label: e.name, type: { function: 'function', macro: 'constant', constant: 'enum', type: 'type' }[e.kind] || 'variable',
      detail: e.kind === 'function' ? e.signature.replace(/^.*?\b\w+\s*(?=\()/, '') : '', boost: e.name.startsWith('lap_') ? 2 : 0,
      info: () => { const d = document.createElement('div'); d.className = 'hoverdoc'; d.innerHTML = `<span class="sig">${esc(e.signature)}</span>${esc(e.doc || e.header)}`; return d; } });
  }
  for (const k of C_WORDS) opts.push({ label: k, type: 'keyword' });
  for (const fn of localFunctions()) opts.push({ label: fn.name, type: 'function', detail: 'this app', boost: 1 });
  return { from: w.from, options: opts, validFor: /^\w*$/ };
}
function wordAt(state, pos) {
  const line = state.doc.lineAt(pos);
  const s = line.text;
  let a = pos - line.from, b = a;
  while (a > 0 && /\w/.test(s[a - 1])) a--;
  while (b < s.length && /\w/.test(s[b])) b++;
  return a === b ? null : { from: line.from + a, to: line.from + b, text: s.slice(a, b) };
}
const hover = CM.hoverTooltip((view, pos) => {
  const w = wordAt(view.state, pos);
  const e = w && apiIndex.get(w.text);
  if (!e) return null;
  return { pos: w.from, end: w.to, above: true, create() {
    const d = el(`<div class="hoverdoc"><span class="sig">${esc(e.signature)}</span>${esc(e.doc || '')}\n<span class="hdr">${esc(e.kind)} in ${esc(e.header)}</span>  <a>Open in API docs</a></div>`);
    d.querySelector('a').onclick = () => showDoc(e.name);
    return { dom: d };
  } };
});
function callAt(state) {
  const pos = state.selection.main.head;
  const from = Math.max(0, pos - 400);
  const text = state.doc.sliceString(from, pos);
  let depth = 0, commas = 0;
  for (let i = text.length - 1; i >= 0; i--) {
    const ch = text[i];
    if (ch === ')') depth++;
    else if (ch === '(') {
      if (depth === 0) { const m = text.slice(0, i).match(/(\w+)\s*$/); return m ? { name: m[1], arg: commas, pos: from + i } : null; }
      depth--;
    } else if (ch === ',' && depth === 0) commas++;
    else if (ch === ';' || ch === '{' || ch === '}') return null;
  }
  return null;
}
const sigField = CM.StateField.define({
  create: () => null,
  update(val, tr) {
    if (!tr.docChanged && !tr.selection) return val;
    const c = callAt(tr.state);
    const e = c && apiIndex.get(c.name);
    if (!e || e.kind !== 'function') return null;
    return { pos: c.pos, above: true, strictSide: true, arrow: false, create() {
      const m = e.signature.match(/^(.*?\()(.*)(\))\s*$/);
      const args = m ? m[2].split(',') : [];
      const html = m ? esc(m[1]) + args.map((a, i) => (i === c.arg ? `<b>${esc(a.trim())}</b>` : esc(a.trim()))).join(', ') + ')' : esc(e.signature);
      return { dom: el(`<div class="sighelp">${html}</div>`) };
    } };
  },
  provide: (f) => CM.showTooltip.from(f),
});
const gitFx = CM.StateEffect.define();
class GitMark extends CM.GutterMarker { constructor(k) { super(); this.k = k; } toDOM() { const s = document.createElement('span'); s.className = 'git-' + this.k; return s; } eq(o) { return o.k === this.k; } }
const GIT = { add: new GitMark('add'), mod: new GitMark('mod'), del: new GitMark('del') };
const gitGutter = CM.gutter({ class: 'cm-git-gutter',
  lineMarker(view, line) {
    const b = P && paneOf(view) && P.files.get(paneOf(view).path);
    if (!b || !b.git) return null;
    const n = view.state.doc.lineAt(line.from).number;
    const h = b.git.find((g) => n >= g.from && n <= g.to);
    return h ? GIT[h.kind] : null;
  },
  lineMarkerChange: (u) => u.transactions.some((tr) => tr.effects.some((e) => e.is(gitFx))) });
function paneOf(view) { return panes.find((p) => p.view === view); }
function editorKeys() {
  const drop = new Set(['Mod-f', 'Mod-Alt-g', 'Mod-d', 'Mod-/', 'Mod-g', 'F3']);
  const keep = (km) => km.filter((k) => !drop.has(k.key));
  return CM.keymap.of([...CM.closeBracketsKeymap, ...keep(CM.defaultKeymap), ...keep(CM.searchKeymap), ...CM.historyKeymap,
    ...CM.foldKeymap, ...CM.completionKeymap, ...CM.lintKeymap, CM.indentWithTab]);
}
function extensions() {
  const cpp = CM.cpp();
  return [
    CM.lineNumbers(), gitGutter, CM.foldGutter(), CM.lintGutter(), CM.highlightActiveLineGutter(), CM.highlightSpecialChars(), CM.history(),
    CM.drawSelection(), CM.dropCursor(), CM.EditorState.allowMultipleSelections.of(true), CM.EditorView.clickAddsSelectionRange.of((e) => e.altKey || e.ctrlKey),
    indentComp.of(SET.autoIndent ? CM.indentOnInput() : []),
    enterComp.of(CM.Prec.high(CM.keymap.of([{ key: 'Enter', run: SET.autoIndent ? CM.insertNewlineAndIndent : CM.insertNewlineKeepIndent }]))),
    CM.syntaxHighlighting(HIGHLIGHT), CM.syntaxHighlighting(CM.defaultHighlightStyle, { fallback: true }),
    CM.bracketMatching(), CM.closeBrackets(), CM.autocompletion({ icons: false }), CM.rectangularSelection(), CM.crosshairCursor(),
    CM.highlightActiveLine(), CM.highlightSelectionMatches(), CM.search({ top: true }), CM.indentUnit.of('    '), CM.EditorState.tabSize.of(4),
    cpp, cpp.language.data.of({ autocomplete: completions }), hover, sigField, editorKeys(),
    CM.EditorView.domEventHandlers({
      contextmenu(e, view) { focusPane = paneOf(view) || focusPane; ctx(e, editorMenu(view)); return true; },
      focus(e, view) { focusPane = paneOf(view) || focusPane; crumbs(); },
      click(e, view) {
        const pos = view.posAtCoords({ x: e.clientX, y: e.clientY });
        if (pos == null) return false;
        let hit = null;
        CM.forEachDiagnostic(view.state, (d, from, to) => { if (pos >= from && pos <= to) hit = d; });
        if (hit) { status(hit.message, hit.severity === 'error' ? 'bad' : ''); const i = problems().findIndex((p) => p.message === hit.message); markProblem(i); }
        return false;
      },
    }),
    CM.EditorView.updateListener.of((u) => {
      const pn = paneOf(u.view);
      if (u.docChanged) {
        if (pn && !u.transactions.some((tr) => tr.annotation(remote))) {
          const other = panes.find((x) => x !== pn && x.view && x.path === pn.path);
          if (other) other.view.dispatch({ changes: u.changes, annotations: remote.of(true) });
        }
        markDirty();
      }
      if (u.selectionSet || u.docChanged) {
        const p = u.state.selection.main.head, l = u.state.doc.lineAt(p);
        const n = u.state.selection.ranges.length;
        $('st-pos').textContent = `Ln ${l.number}, Col ${p - l.from + 1}${n > 1 ? `  (${n} cursors)` : ''}`;
        crumbs();
      }
      if (u.docChanged || u.geometryChanged || u.viewportChanged) updateMarks();
      if (u.docChanged) scheduleOutline();
    }),
  ];
}
function reconfigure(view) {
  view.dispatch({ effects: [indentComp.reconfigure(SET.autoIndent ? CM.indentOnInput() : []),
    enterComp.reconfigure(CM.Prec.high(CM.keymap.of([{ key: 'Enter', run: SET.autoIndent ? CM.insertNewlineAndIndent : CM.insertNewlineKeepIndent }])))] });
}
let dirtyT = 0;
function markDirty() { clearTimeout(dirtyT); dirtyT = setTimeout(() => { drawFileTabs(); drawProjTabs(); drawTree(); }, 150); }
function makePane(pn) {
  const host = $('host-' + pn.id);
  pn.view = new CM.EditorView({ parent: host, state: CM.EditorState.create({ doc: '', extensions: extensions() }) });
}
async function openFile(path, line, col, pane) {
  if (!P) return;
  const pn = pane || (focusPane.view && $('pane-B').hidden === false && focusPane === panes[1] ? panes[1] : panes[0]);
  stashViews();
  let b = P.files.get(path);
  const entry = findEntry(path);
  if (!b) {
    if (entry && entry.kind === 'image') b = { path, kind: 'image' };
    else if (entry && entry.kind === 'other') { status(path + ' is not a text file', 'bad'); return; }
    else {
      const j = await api('file?' + pq(P.path) + '&path=' + encodeURIComponent(path));
      b = { path, kind: 'text', state: CM.EditorState.create({ doc: j.text, extensions: extensions() }), saved: j.text };
    }
    P.files.set(path, b);
  }
  if (!P.order.includes(path)) P.order.push(path);
  if (pn === panes[0]) P.active = path;
  pn.path = path;
  hideStart();
  showPanel('editor');
  if (b.kind === 'image') showAsset(pn, b);
  else {
    $('asset-' + pn.id).hidden = true;
    $('host-' + pn.id).hidden = false;
    pn.view.setState(b.state);
    reconfigure(pn.view);
    applyDiagnostics(pn);
    if (!b.git) fetchGit(b);
    else pn.view.dispatch({ effects: gitFx.of(null) });
  }
  focusPane = pn;
  drawFileTabs();
  drawTree();
  crumbs();
  drawOutline();
  if (line && b.kind === 'text') goTo(line, col, pn);
  else if (b.kind === 'text') pn.view.focus();
}
function openSplit(path) {
  $('pane-B').hidden = false;
  if (!panes[1].view) makePane(panes[1]);
  openFile(path || curPath(), null, null, panes[1]);
}
function closeSplit() { stashViews(); panes[1].path = null; $('pane-B').hidden = true; focusPane = panes[0]; updateMarks(); }
async function closeFile(path, force) {
  const b = P.files.get(path);
  if (!force && b && b.kind === 'text' && docOf(P, b).toString() !== b.saved) {
    const ans = await ask('Close ' + path, `<p>${esc(path)} has unsaved changes.</p>`, ['Save', "Don't save", 'Cancel']);
    if (ans === 2 || ans < 0) return;
    if (ans === 0) await save(path);
  }
  P.files.delete(path);
  const i = P.order.indexOf(path);
  P.order = P.order.filter((x) => x !== path);
  if (panes[1].path === path) closeSplit();
  if (panes[0].path === path) {
    panes[0].path = null;
    const next = P.order[Math.min(i, P.order.length - 1)];
    if (next) openFile(next); else { P.active = null; showStart(); }
  }
  drawFileTabs(); drawTree();
}
function drawFileTabs() {
  const bar = $('ftabs');
  bar.innerHTML = '';
  if (startShown) bar.appendChild(el('<span class="ftab sel">Start</span>'));
  if (!P) return;
  for (const path of P.order) {
    const b = P.files.get(path);
    if (!b) continue;
    const dirty = b.kind === 'text' && docOf(P, b).toString() !== b.saved;
    const [ic, cls] = iconOf({ name: path, kind: b.kind });
    const tab = el(`<span class="ftab${path === panes[0].path && !startShown ? ' sel' : ''}" title="${esc(path)}"><span class="ico ${cls}">${ic}</span>` +
      `<span>${esc(path.split('/').pop())}</span><span class="dot">${dirty ? '\u25cf' : ''}</span><span class="x">x</span></span>`);
    tab.onclick = (e) => { if (e.target.classList.contains('x')) closeFile(path); else openFile(path, null, null, panes[0]); };
    tab.onauxclick = (e) => { if (e.button === 1) closeFile(path); };
    tab.oncontextmenu = (e) => ctx(e, [['Close', keyOf('file.closeTab'), () => closeFile(path)], ['Close the others', '', () => P.order.filter((x) => x !== path).forEach((x) => closeFile(x))],
      ['Open to the side', '', () => openSplit(path), { off: b.kind !== 'text' }], null, ['Save', '', () => save(path)], ['Copy path', '', () => copy(P.path + '/' + path, 'the path')],
      ['Reveal in Explorer', '', () => api('reveal?' + pq(P.path) + '&path=' + encodeURIComponent(path), {})]]);
    bar.appendChild(tab);
  }
  $('phead-B').firstChild.textContent = panes[1].path ? panes[1].path + (P.files.get(panes[1].path)?.kind === 'text' && docOf(P, P.files.get(panes[1].path)).toString() !== P.files.get(panes[1].path).saved ? ' \u25cf' : '') : '';
}
function goTo(line, col, pn) {
  const view = (pn || focusPane).view;
  const doc = view.state.doc;
  const l = doc.line(Math.max(1, Math.min(line, doc.lines)));
  const pos = Math.min(l.to, l.from + Math.max(0, (col || 1) - 1));
  view.dispatch({ selection: { anchor: pos }, effects: CM.EditorView.scrollIntoView(pos, { y: 'center' }) });
  view.focus();
}
function editorMenu(view) {
  const w = wordAt(view.state, view.state.selection.main.head);
  const has = !view.state.selection.main.empty;
  return [['Cut', 'Ctrl+X', () => clip('cut'), { off: !has }], ['Copy', 'Ctrl+C', () => clip('copy'), { off: !has }], ['Paste', 'Ctrl+V', () => clip('paste')],
    ['Select all', 'Ctrl+A', () => CM.selectAll(view)], null,
    C('edit.comment', 'Comment line'), C('edit.format', 'Format'), ['Go to definition' + (w ? ': ' + w.text : ''), keyOf('edit.gotoDef'), () => run('edit.gotoDef'), { off: !w }],
    C('edit.selectNext', 'Add the next match'), null, C('edit.find', 'Find'), C('edit.replace', 'Find and replace'), C('edit.gotoLine', 'Go to line'), null,
    ['Open to the side', '', () => openSplit(curPath())], ['Copy path', '', () => copy(P.path + '/' + curPath(), 'the path')]];
}
async function clip(what) {
  const view = ed();
  if (!view) return;
  view.focus();
  const r = view.state.selection.main;
  if (what === 'paste') {
    try { const text = await navigator.clipboard.readText(); view.dispatch(view.state.replaceSelection(text)); }
    catch (e) { status('The browser keeps the clipboard to itself here: press Ctrl+V', 'bad'); }
    return;
  }
  const text = view.state.sliceDoc(r.from, r.to);
  await copy(text);
  if (what === 'cut') view.dispatch({ changes: { from: r.from, to: r.to } });
}
function gotoDef() {
  const view = ed();
  const w = view && wordAt(view.state, view.state.selection.main.head);
  if (!w) return;
  if (apiIndex.has(w.text)) { showDoc(w.text); return; }
  const fn = localFunctions().find((f) => f.name === w.text);
  if (fn) { goTo(fn.line, 1); return; }
  showPanel('search'); $('search-q').value = w.text; projectSearch();
}
async function save(path) {
  path = path || curPath();
  if (!P || !path) return;
  stashViews();
  const b = P.files.get(path);
  if (!b || b.kind !== 'text') return;
  let text = docOf(P, b).toString();
  if (SET.formatOnSave && /\.(c|h)$/.test(path)) {
    const f = await api('format', { text, path });
    if (f.text !== text) { replaceDoc(path, f.text); text = f.text; }
  }
  if (text === b.saved) { drawFileTabs(); return; }
  await api('file?' + pq(P.path) + '&path=' + encodeURIComponent(path), text, 'POST');
  b.saved = text;
  fetchGit(b);
  drawFileTabs(); drawTree(); drawProjTabs();
}
function replaceDoc(path, text) {
  const pn = panes.find((x) => x.path === path && x.view) || null;
  const view = pn ? pn.view : null;
  const old = view ? view.state.doc.toString() : P.files.get(path).state.doc.toString();
  let a = 0; while (a < old.length && a < text.length && old[a] === text[a]) a++;
  let b = 0; while (b < old.length - a && b < text.length - a && old[old.length - 1 - b] === text[text.length - 1 - b]) b++;
  const change = { from: a, to: old.length - b, insert: text.slice(a, text.length - b) };
  if (view) view.dispatch({ changes: change });
  else { const buf = P.files.get(path); buf.state = buf.state.update({ changes: change }).state; }
}
async function saveAll() { if (!P) return; stashViews(); for (const [k, b] of P.files) if (b.kind === 'text' && b.state.doc.toString() !== b.saved) await save(k); }
async function format() {
  const view = ed(); if (!view) return;
  const f = await api('format', { text: view.state.doc.toString(), path: curPath() });
  replaceDoc(curPath(), f.text);
  status('Formatted (' + f.by + ')', 'good');
}
function fontStep(d) { SET.fontSize = Math.max(9, Math.min(28, SET.fontSize + d)); applySettings(); }

// ---- outline and breadcrumbs
let outlineT = 0;
function scheduleOutline() { clearTimeout(outlineT); outlineT = setTimeout(drawOutline, 300); }
function localFunctions() {
  const view = panes[0].view;
  if (!view || !P || !panes[0].path) return [];
  const out = [];
  const doc = view.state.doc;
  for (let n = 1; n <= doc.lines; n++) {
    const s = doc.line(n).text;
    const m = s.match(/^(?:static\s+|inline\s+|const\s+)*[A-Za-z_][\w\s\*]*?\b(\w+)\s*\(([^;]*)$/);
    if (m && !/^(if|for|while|switch|return|else|LAP_APP|LAP_ICON_PX)$/.test(m[1]) && !/^\s/.test(s) && !/^#/.test(s)) out.push({ name: m[1], line: n });
  }
  return out;
}
function drawOutline() {
  const box = $('outline');
  if (!box) return;
  const fns = localFunctions();
  box.innerHTML = fns.length ? fns.map((f, i) => `<div class="p" data-i="${i}"><span class="ico c">f</span><span>${esc(f.name)}()</span><span class="where">${f.line}</span></div>`).join('')
    : '<div class="none">No functions in this file.</div>';
  box.onclick = (e) => { const r = e.target.closest('.p'); if (r) goTo(fns[+r.dataset.i].line, 1, panes[0]); };
}
function crumbs() {
  const c = $('crumbs');
  if (!P || startShown) { c.textContent = ''; return; }
  const path = curPath() || '';
  const view = focusPane.view;
  let fn = '';
  if (view && path.endsWith('.c')) {
    const line = view.state.doc.lineAt(view.state.selection.main.head).number;
    const fns = localFunctions().filter((f) => f.line <= line);
    if (fns.length) fn = fns[fns.length - 1].name + '()';
  }
  c.innerHTML = `${esc(P.name)} \u203a ${path.split('/').map(esc).join(' \u203a ')}${fn ? ' \u203a <b>' + esc(fn) + '</b>' : ''}`;
}

// ---- error marks beside the scrollbar
function updateMarks() {
  for (const pn of panes) {
    const box = $('marks-' + pn.id);
    if (!box) continue;
    box.innerHTML = '';
    if (!pn.view || !pn.path || !P) continue;
    const lines = pn.view.state.doc.lines;
    const sc = pn.view.scrollDOM;
    if (sc.scrollHeight > sc.clientHeight) {
      const vm = el('<i class="view"></i>');
      vm.style.top = (sc.scrollTop / sc.scrollHeight * 100) + '%';
      vm.style.height = (sc.clientHeight / sc.scrollHeight * 100) + '%';
      box.appendChild(vm);
    }
    for (const p of problems().filter((x) => x.path === pn.path && x.severity !== 'note')) {
      const m = el(`<i class="${p.severity}" title="${esc(p.line + ': ' + p.message)}"></i>`);
      m.style.top = `calc(${((p.line - 1) / Math.max(1, lines)) * 100}% )`;
      m.onclick = () => goTo(p.line, p.col, pn);
      box.appendChild(m);
    }
  }
}

// ---- git
async function fetchGit(b) {
  if (!P || !b || b.kind !== 'text') return;
  try {
    const g = await api('git?' + pq(P.path) + '&path=' + encodeURIComponent(b.path));
    b.git = g.repo ? g.hunks : [];
    $('st-git').textContent = g.repo ? 'git: ' + g.branch : '';
    for (const pn of panes) if (pn.view && pn.path === b.path) pn.view.dispatch({ effects: gitFx.of(null) });
  } catch (e) { /* not a repo */ }
}
function gitStatus() { if (P) api('git?' + pq(P.path)).then((g) => { $('st-git').textContent = g.repo ? 'git: ' + g.branch : ''; }).catch(() => {}); }

// ---- assets
async function showAsset(pn, b) {
  $('host-' + pn.id).hidden = true;
  const box = $('asset-' + pn.id);
  box.hidden = false;
  const src = 'api/raw?' + pq(P.path) + '&path=' + encodeURIComponent(b.path);
  box.innerHTML = `<h3>${esc(b.path)}</h3><div class="imgbox"><img id="asset-img-${pn.id}" src="${src}" alt=""></div><p id="asset-info-${pn.id}" class="dlg-note"></p><p id="asset-uses-${pn.id}"></p>`;
  const img = $('asset-img-' + pn.id);
  img.onload = () => {
    const scale = Math.max(1, Math.floor(Math.min(480 / img.naturalWidth, 360 / img.naturalHeight)));
    img.style.width = img.naturalWidth * scale + 'px';
    const f = findEntry(b.path);
    $('asset-info-' + pn.id).textContent = `${img.naturalWidth} x ${img.naturalHeight} pixels, ${f ? f.size.toLocaleString() : '?'} bytes, shown x${scale}`;
  };
  const name = b.path.split('/').pop();
  const hits = await api('search?' + pq(P.path) + '&q=' + encodeURIComponent(name.replace(/\.[^.]+$/, '')));
  $('asset-uses-' + pn.id).innerHTML = hits.hits.length ? 'Used by: ' + hits.hits.slice(0, 8).map((h) => `<a class="use" data-p="${esc(h.path)}" data-l="${h.line}">${esc(h.path)}:${h.line}</a>`).join(', ')
    : '<span class="dlg-note">Not named in the app\'s sources yet.</span>';
  box.querySelectorAll('.use').forEach((a) => { a.style.cursor = 'pointer'; a.onclick = () => openFile(a.dataset.p, +a.dataset.l); });
}

// ================================================================== start page

let startShown = false;
const TEMPLATE_ART = {
  blank: '   /|     HELLO 3\n  / |    [===|=>--]\n /__|__  [HELLO h]\n~~~~~~~~ [OPTIONS o]',
  radio: ' 433.920 MHz  RX SPEC\n .:|:.:|||:. spectrum\n ##+##%##+#  waterfall\n [RADIO][TUNE][OPTIONS]',
  dots: ' .-----------.\n |   (@)     | tilt\n |      .    | swipe\n \'-----------\'',
  '3d': '    .----.\n   ( (()) )  drag to\n    \'----\'   walk round\n     ====',
  service: '  ( ( o ) )  watching\n  decodes 41  in the\n  matches  2  background\n  :|:.:|:..|',
};
function showStart() {
  startShown = true;
  $('start').hidden = false;
  $('panes').hidden = true;
  $('crumbs').textContent = '';
  drawFileTabs();
  drawStart();
  showPanel('editor');
}
function hideStart() { if (!startShown) return; startShown = false; $('start').hidden = true; $('panes').hidden = false; }
function drawStartIfShown() { if (startShown) drawStart(); }
function drawStart() {
  const box = $('start');
  const cards = (S.templates || []).map((tp) => `<div class="card" data-t="${tp.name}"><b>${esc(tp.name)}</b><div class="art">${esc(TEMPLATE_ART[tp.name] || '')}</div>${esc(tp.text)}</div>`).join('');
  const recent = (S.recent || []).map((r, i) => `<div class="rcard${r.exists ? '' : ' gone'}" data-i="${i}"><img class="thumb" data-p="${esc(r.path)}" alt="">` +
    `<div class="meta"><div class="nm">${r.pinned ? '* ' : ''}${esc(r.name)}</div><div>${!r.exists ? 'not on disk any more' : r.opened ? 'opened ' + ago(r.opened) : 'in the workspace, not opened yet'}</div><div class="pth">${esc(r.path)}</div></div>` +
    `<div class="acts"><button class="key tiny" data-a="pin">${r.pinned ? 'unpin' : 'pin'}</button><button class="key tiny" data-a="rm" title="From this list only; the folder stays">remove</button></div></div>`).join('')
    || '<p class="dlg-note">Nothing yet: pick a template above.</p>';
  box.innerHTML = `<pre class="hero">   /|      L A K E S H A R K   S T U D I O\n  / |      apps for the board, made beside a board that runs them\n /__|__\n~~~~~~~~~</pre>` +
    `<div class="steps"><div class="step"><b>1</b> Pick a template</div><div class="step"><b>2</b> It runs on the simulated board</div>` +
    `<div class="step"><b>3</b> <b>F8</b> puts it on your real board</div></div>` +
    `<h2>Start an app</h2><div class="cards">${cards}</div>` +
    `<h2>Recent apps</h2><div class="recent">${recent}</div><p><button class="key small" data-a="open">Open folder...</button> <button class="key small" data-a="new">New app with a name...</button></p>` +
    `<h2>What only Studio does</h2><div class="strengths">` +
    `<div class="strength"><b>The board beside the code.</b> Run (F5) shows the app drawn by the board's own screen code, fed by the firmware's own UI; tilt, swipe, feed it packets, take it away its radio.</div>` +
    `<div class="strength"><b>One key to the real board.</b> F8 builds and installs over USB or through LakeShark Desktop, then opens it in a desk window.</div>` +
    `<div class="strength"><b>The board's own log and screen.</b> Board log follows the console; the AI tools read the board's screen.</div>` +
    `<div class="strength"><b>An AI with the same tools.</b> The AI panel and any MCP client build, run the sim, read its screen and install. <a data-a="ai">Connect an AI</a></div>` +
    `<div class="strength"><b>Still use VS Code.</b> Build, sim, install and the AI tools work there too. <a data-a="vscode">VS Code users</a></div></div>`;
  box.querySelectorAll('.card').forEach((c) => { c.onclick = () => quickStart(c.dataset.t); });
  box.querySelectorAll('.rcard').forEach((c) => {
    const r = S.recent[+c.dataset.i];
    c.onclick = (e) => {
      const a = e.target.dataset.a;
      if (a === 'pin') api('recent', { path: r.path, pinned: !r.pinned }).then((j) => { S.recent = j.recent; drawStart(); });
      else if (a === 'rm') api('recent', { path: r.path, remove: true }).then((j) => { S.recent = j.recent; drawStart(); status('Removed ' + r.name + ' from the list; the folder is still there'); });
      else if (r.exists) openProject(r.path);
    };
  });
  box.querySelectorAll('img.thumb').forEach((im) => {
    const r = S.recent.find((x) => x.path === im.dataset.p);
    if (r && r.thumb) api('thumb?' + pq(r.path)).then((j) => { if (j.png) im.src = j.png; }).catch(() => {});
  });
  box.querySelector('[data-a=open]').onclick = () => run('app.openFolder');
  box.querySelector('[data-a=new]').onclick = () => run('app.new');
  box.querySelector('[data-a=ai]').onclick = () => run('ai.connect');
  box.querySelector('[data-a=vscode]').onclick = () => run('ai.vscode');
  box.querySelectorAll('a').forEach((a) => { a.style.cursor = 'pointer'; a.style.color = 'var(--info)'; });
}
async function quickStart(template) {
  const taken = new Set((S.projects || []).concat((S.recent || []).map((r) => r.name)));
  let n = 1;
  while (taken.has(`my_${template}${n}`)) n++;
  await createApp(`my_${template}${n}`, template, true);
}
async function createApp(name, template, autoRun) {
  if (P && dirtyFiles(P).length) {
    const ans = await ask('Unsaved changes', `<p>${esc(P.name)} has unsaved changes. They stay open in its tab either way.</p>`, ['Save them, then make the app', 'Keep them unsaved', 'Cancel']);
    if (ans === 2 || ans < 0) return;
    if (ans === 0) await saveAll();
  }
  const info = await api('new', { name, template });
  await openProject(info.path);
  status(`Made ${name} from the ${template} template. ${autoRun ? 'Running it in the simulator...' : 'F5 runs it, F8 puts it on the board.'}`, 'good');
  if (autoRun) runSim();
}

// ================================================================== problems, logs, terminal, search

function problems() { return P ? P.problems : []; }
function showProblems(list, output) {
  if (P) P.problems = list || [];
  const ps = problems();
  const ne = ps.filter((p) => p.severity === 'error').length, nw = ps.filter((p) => p.severity === 'warning').length;
  showProblemCount();
  const b = $('st-problems');
  b.textContent = `x ${ne}  ! ${nw}`;
  b.className = 'badge' + (ne ? ' err' : nw ? ' warn' : '');
  $('problems').innerHTML = ps.length ? ps.map((p, i) =>
    `<div class="p" data-i="${i}"><button class="key tiny" data-copy="${i}" title="Copy this one">copy</button><span class="sev-${p.severity}">${p.severity}</span>` +
    `<span class="where">${esc((p.path || p.file) + ':' + p.line + ':' + p.col)}</span><span class="msg">${esc(p.message)}</span></div>`).join('')
    : '<div class="none">No problems.</div>';
  if (output !== undefined) $('output').textContent = output || '';
  for (const pn of panes) applyDiagnostics(pn);
  updateMarks();
  return ne;
}
function showProblemCount() { const n = $('n-problems'); if (n) n.textContent = problems().length ? String(problems().length) : ''; }
function problemText(p) { return `${p.path || p.file}:${p.line}:${p.col}: ${p.severity}: ${p.message}`; }
function applyDiagnostics(pn) {
  if (!pn.view || !pn.path || !P || P.files.get(pn.path)?.kind !== 'text') return;
  const doc = pn.view.state.doc;
  const diags = problems().filter((p) => p.path === pn.path && p.severity !== 'note').map((p) => {
    const l = doc.line(Math.max(1, Math.min(p.line, doc.lines)));
    const from = Math.min(l.to, l.from + Math.max(0, p.col - 1));
    const word = doc.sliceString(from, l.to).match(/^\w+/);
    return { from, to: Math.min(l.to, from + (word ? word[0].length : 1)), severity: p.severity, message: p.message,
      actions: [{ name: 'Copy', apply: () => copy(problemText(p), 'the error') }] };
  });
  pn.view.dispatch(CM.setDiagnostics(pn.view.state, diags));
}
function markProblem(i) {
  document.querySelectorAll('#problems .p').forEach((r) => r.classList.toggle('sel', +r.dataset.i === i));
}
let probAt = -1;
function nextProblem() { const ps = problems(); if (!ps.length) return; probAt = (probAt + 1) % ps.length; const p = ps[probAt]; if (p.path) openFile(p.path, p.line, p.col); markProblem(probAt); }
function logMenu(pre, name) {
  return (e) => {
    const sel = String(getSelection());
    ctx(e, [['Copy selection', 'Ctrl+C', () => copy(sel), { off: !sel }], ['Copy all', '', () => copy(pre.textContent, 'the whole ' + name)],
      ['Clear', '', () => { pre.textContent = ''; }], ['Save as...', '', () => download(name.replace(/\s/g, '-') + '.txt', pre.textContent)]]);
  };
}
let logSeq = 0;
async function pollLogs() {
  for (;;) {
    try {
      const j = await api('logs?since=' + logSeq + '&wait=1.5');
      logSeq = j.seq;
      for (const l of j.lines) {
        const pre = $({ board: 'boardlog', term: 'termout' }[l.src] || 'simlog');
        const span = document.createElement('span');
        span.className = /^\[app /.test(l.text) ? 'log-app' : /^\$ /.test(l.text) ? 'log-cmd' : /ERR|error|fail/i.test(l.text) ? 'log-err' : l.text.startsWith('--') ? 'log-' + l.src : '';
        span.textContent = l.text + '\n';
        const sc = pre.closest('.scroll');
        const atEnd = sc.scrollHeight - sc.scrollTop - sc.clientHeight < 60;
        pre.appendChild(span);
        while (pre.childNodes.length > 2000) pre.removeChild(pre.firstChild);
        if (atEnd) sc.scrollTop = sc.scrollHeight;
      }
    } catch (e) { await sleep(2000); }
  }
}
const termHist = store.get('studio-term-history', []);
let termAt = termHist.length;
async function termRun(line) {
  line = line.trim();
  if (!line) return;
  termHist.push(line); store.set('studio-term-history', termHist.slice(-50)); termAt = termHist.length;
  showPanel('terminal');
  try { await saveAll(); await api('term', { line, p: P && P.path }); } catch (e) { $('termout').appendChild(document.createTextNode(e.message + '\n')); }
}
async function projectSearch() {
  if (!P) return;
  const q = $('search-q').value;
  const r = await api(`search?${pq(P.path)}&q=${encodeURIComponent(q)}&case=${$('search-case').checked ? 1 : 0}&regex=${$('search-re').checked ? 1 : 0}`);
  let last = '';
  $('search-res').innerHTML = r.hits.map((h, i) => (h.path !== last ? `<div class="grp">${esc(last = h.path)}</div>` : '') +
    `<div class="p" data-i="${i}"><span class="where">${h.line}</span><span class="msg">${esc(h.text)}</span></div>`).join('') +
    (r.more ? '<div class="none">More than 500: narrow it.</div>' : '') || '<div class="none">' + (q ? 'Nothing found.' : '') + '</div>';
  $('search-res').onclick = (e) => { const row = e.target.closest('.p'); if (row) { const h = r.hits[+row.dataset.i]; openFile(h.path, h.line, h.col); } };
}

// ================================================================== build, run, install

const busy = new Set();
async function guarded(name, fn) {
  if (busy.has(name)) return;
  busy.add(name);
  document.querySelectorAll(`[data-cmd="${name}"]`).forEach((b) => b.classList.add('down'));
  try { await fn(); } catch (e) { status(e.message, 'bad'); }
  finally { busy.delete(name); document.querySelectorAll(`[data-cmd="${name}"]`).forEach((b) => b.classList.remove('down')); }
}
async function needProject() { if (!P) { run('app.new'); throw new Error('Make or open an app first'); } }
async function build() {
  await needProject();
  await saveAll();
  status('Building for the board...');
  const r = await api('build?' + pq(P.path), {});
  P.built = r;
  const errors = showProblems(r.problems, r.output);
  $('st-build').textContent = `built in ${r.seconds} s`;
  if (r.ok) status(`Built ${r.lap}: ${r.size} bytes, ${r.manifest.id} ${r.manifest.version}, API ${r.manifest.api_major}.${r.manifest.api_minor}` +
    (r.problems.length ? `, ${r.problems.length} warning(s)` : ''), 'good');
  else { status(`Build failed: ${errors} error(s)`, 'bad'); showPanel('problems'); }
  return r.ok;
}
let landscape = store.get('studio-landscape', false), noRadio = store.get('studio-noradio', false);
async function runSim() {
  await needProject();
  await saveAll();
  status('Building the app into the simulator...');
  showPanel('run');
  idle('\n   building ' + P.name + ' into lssim...\n');
  const r = await api('run?' + pq(P.path), { landscape, no_radio: noRadio });
  showProblems(r.problems, r.output);
  $('st-build').textContent = `sim built in ${r.seconds} s`;
  if (!r.ok) { status('The simulator build failed: see Problems', 'bad'); idle('\n   the build failed:\n   see Problems\n'); showPanel('problems'); return; }
  loadSim();
  status(`${P.name} runs in the simulator${noRadio ? ', with no radio (as a Nano without an SDR)' : ''}: click it, type to it, tilt it`, 'good');
  pollState();
  setTimeout(saveThumb, 3000);
}
let popped = null;
function loadSim() {
  if (popped && !popped.closed) { popped.location.reload(); idle('\n   the simulator is in its own window\n'); return; }
  const f = $('sim');
  f.src = 'screen/screen.html?run=' + Date.now();
  $('sim-idle').hidden = true;
  f.onload = () => {
    try {
      f.contentDocument.addEventListener('contextmenu', (e) => {
        e.preventDefault();
        const r = f.getBoundingClientRect();
        ctx({ preventDefault() {}, clientX: r.left + e.clientX, clientY: r.top + e.clientY }, simMenu());
      });
    } catch (x) { /* another origin: never here */ }
  };
}
function idle(text) { $('sim-idle').textContent = text; $('sim-idle').hidden = false; }
function simCanvas() { try { return $('sim').contentDocument.getElementById('screen'); } catch (e) { return null; } }
async function saveThumb() {
  const c = simCanvas();
  if (!c || !P || !c.width) return;
  const sm = document.createElement('canvas');
  const k = Math.min(1, 160 / c.height);
  sm.width = Math.round(c.width * k); sm.height = Math.round(c.height * k);
  sm.getContext('2d').drawImage(c, 0, 0, sm.width, sm.height);
  const p = S.sim.path || P.path;
  await api('thumb?' + pq(p), sm.toDataURL('image/png'), 'POST', true).catch(() => {});
}
async function screenshot() {
  const c = simCanvas();
  if (!c || !c.width) { status('Nothing is running: Run (F5) first', 'bad'); return; }
  const url = c.toDataURL('image/png');
  let saved = '';
  try { saved = (await api('shot?' + pq(S.sim.path || P.path), url, 'POST', true)).file; } catch (e) { /* still to the clipboard */ }
  try {
    const blob = await (await fetch(url)).blob();
    await navigator.clipboard.write([new ClipboardItem({ 'image/png': blob })]);
    status('Screenshot on the clipboard' + (saved ? ' and in ' + saved : ''), 'good');
  } catch (e) { status(saved ? 'Screenshot saved in ' + saved : 'Could not take the screenshot', saved ? 'good' : 'bad'); }
}
function simMenu() {
  return [C('sim.restart', 'Restart'), C('sim.shot', 'Screenshot to clipboard and file'), C('sim.rotate', 'Rotate'),
    ['Board without a radio', '', () => run('sim.noRadio'), { check: noRadio }], null, C('sim.popout', 'Pop out the simulator'), C('build.stop', 'Stop')];
}
function popOut() {
  popped = window.open('screen/screen.html?pop=' + Date.now(), 'lakeshark-sim', 'width=560,height=940');
  if (!popped) { status('The browser blocked the window: allow pop-ups for Studio', 'bad'); return; }
  $('sim').src = 'about:blank';
  idle('\n   the simulator is in its own window\n   (close it to bring it back here)\n');
  const t = setInterval(() => { if (popped.closed) { clearInterval(t); popped = null; if (S.sim.running) loadSim(); } }, 1000);
}
async function stopSim() { await api('stop', {}); $('sim').src = 'about:blank'; idle('\n   stopped: Run (F5) starts it\n'); pollState(); }
async function step(spec) { await api('sim', { step: spec }); try { $('sim').focus(); } catch (e) { /* popped out */ } }
async function boardOpen() { await needProject(); const r = await api('board/open?' + pq(P.path), {}); status('Board: ' + (r.lines || []).join(' '), 'good'); }
function setTail(on) { api('board/tail', { on }).then(() => pollState()); }
function meter(sent, total, w = 40) {
  const k = total ? Math.round(w * sent / total) : 0;
  return '[' + '='.repeat(Math.max(0, k - 1)) + (k ? (k >= w ? '=' : '>') : '') + '-'.repeat(w - k) + ']';
}
async function install() {
  await needProject();
  await saveAll();
  const d = dialog('Install on the Board',
    `<pre class="art-big">   ___________        .-------.\n  |  ${esc((P.id || P.name).slice(0, 8).padEnd(8))} |  ==>  | board |\n  |___________|  USB  '-------'</pre>` +
    `<p id="in-via" class="dlg-note">Looking for the board...</p><pre id="in-bar" class="meter">${meter(0, 1)}</pre>` +
    `<p id="in-msg" class="dlg-note">building</p><p id="in-err" class="dlg-bad"></p>`,
  [{ label: 'Open on board', fn: async () => { try { await boardOpen(); closeDialog(); } catch (e) { $('in-err').textContent = e.message; } } },
    { label: 'Close', fn: closeDialog }]);
  const openBtn = d.querySelector('[data-b="0"]');
  openBtn.disabled = true;
  const t0 = Date.now();
  try { await api('install?' + pq(P.path), {}); } catch (e) { $('in-err').textContent = e.message; return; }
  for (;;) {
    await sleep(300);
    const j = await api('job');
    if ($('modal').hidden) return;
    if (j.via) $('in-via').textContent = 'Through ' + j.via;
    $('in-bar').textContent = meter(j.sent || 0, j.total || 1) + `  ${(j.sent || 0).toLocaleString()} / ${(j.total || 0).toLocaleString()} bytes`;
    $('in-msg').textContent = j.message || '';
    if (j.state === 'done') {
      $('in-msg').className = 'dlg-good';
      openBtn.disabled = false;
      $('st-build').textContent = `installed in ${((Date.now() - t0) / 1000).toFixed(1)} s`;
      status(`Installed ${P.name} on the board`, 'good');
      return;
    }
    if (j.state === 'error') {
      $('in-msg').textContent = '';
      $('in-err').textContent = j.message;
      if (j.problems) showProblems(j.problems);
      status('Install failed', 'bad');
      return;
    }
  }
}

// ================================================================== dialogs

function dialog(title, html, buttons, wide) {
  const d = $('dialog');
  d.className = 'dialog win' + (wide ? ' wide' : '');
  d.innerHTML = `<div class="title on">${esc(title)}<span class="keys">[x]</span></div><div class="body">${html}</div>` +
    `<div class="foot">${buttons.map((b, i) => `<button class="key${i === 0 ? ' primary' : ''}" data-b="${i}">${esc(b.label)}</button>`).join('')}</div>`;
  $('modal').hidden = false;
  d.querySelector('.keys').onclick = closeDialog;
  d.querySelectorAll('.foot .key').forEach((x) => { x.onclick = () => buttons[+x.dataset.b].fn(); });
  return d;
}
let dialogDone = null;
function closeDialog() { $('modal').hidden = true; $('dialog').innerHTML = ''; if (dialogDone) { const f = dialogDone; dialogDone = null; f(-1); } }
document.addEventListener('keydown', (e) => { if (e.key === 'Escape' && !$('modal').hidden && !recording) closeDialog(); });
function ask(title, html, labels) {
  return new Promise((resolve) => {
    dialog(title, html, labels.map((label, i) => ({ label, fn: () => { dialogDone = null; closeDialog(); resolve(i); } })));
    dialogDone = resolve;
  });
}
function promptText(title, label, value) {
  return new Promise((resolve) => {
    const d = dialog(title, `<label class="field"><span>${esc(label)}</span> <input type="text" id="pt-v" spellcheck="false" value="${esc(value || '')}"></label>`,
      [{ label: 'OK', fn: () => { const v = $('pt-v').value.trim(); dialogDone = null; closeDialog(); resolve(v); } },
        { label: 'Cancel', fn: () => { dialogDone = null; closeDialog(); resolve(null); } }]);
    dialogDone = () => resolve(null);
    const i = d.querySelector('#pt-v');
    i.focus(); i.setSelectionRange(0, (value || '').includes('.') ? value.lastIndexOf('.') : (value || '').length);
    i.onkeydown = (e) => { if (e.key === 'Enter') d.querySelector('[data-b="0"]').click(); };
  });
}
function newAppDialog() {
  let pick = 'blank';
  const rows = S.templates.map((tp) => `<div class="tpl${tp.name === pick ? ' sel' : ''}" data-t="${tp.name}"><span>${tp.name === pick ? '(*)' : '( )'}</span>` +
    `<b>${esc(tp.name)}</b><span>${esc(tp.text)}</span><pre class="art">${esc(TEMPLATE_ART[tp.name] || '')}</pre></div>`).join('');
  const dirty = P && dirtyFiles(P).length;
  const d = dialog('New App', `<p class="dlg-note">A working app from a template, in ${esc(S.workspace)}. Nothing there is overwritten; ` +
    `${P ? esc(P.name) + ' stays open in its tab.' : ''}</p>` + (dirty ? `<p class="dlg-warn">${esc(P.name)} has unsaved changes: you will be asked first.</p>` : '') +
    `<label class="field"><span><u>N</u>ame</span> <input type="text" id="na-name" maxlength="31" spellcheck="false" placeholder="demo"></label>` +
    `<label><input type="checkbox" id="na-run" checked> run it in the simulator straight away</label><div id="na-tpls">${rows}</div><p id="na-err" class="dlg-bad"></p>`,
  [{ label: 'Create', fn: create }, { label: 'Cancel', fn: closeDialog }]);
  d.querySelectorAll('.tpl').forEach((x) => {
    x.onclick = () => { pick = x.dataset.t; d.querySelectorAll('.tpl').forEach((y) => { y.classList.toggle('sel', y === x); y.firstChild.textContent = y === x ? '(*)' : '( )'; }); };
  });
  const name = $('na-name');
  name.focus();
  name.onkeydown = (e) => { if (e.key === 'Enter') create(); };
  async function create() {
    const n = name.value.trim().toLowerCase() || 'demo';
    if (!/^[a-z0-9_.-]{1,31}$/.test(n)) { $('na-err').textContent = 'A name is 1-31 of a-z 0-9 _ . - (it becomes the app\'s id).'; return; }
    if ((S.projects || []).includes(n)) { $('na-err').textContent = n + ' already exists in the workspace: open it from Recent, or pick another name.'; return; }
    const autoRun = $('na-run').checked;
    closeDialog();
    try { await createApp(n, pick, autoRun); } catch (e) { status(e.message, 'bad'); }
  }
}
async function openFolder() {
  const v = await promptText('Open folder', 'An app folder (with .c files)', S.workspace + (S.projects && S.projects[0] ? '\\' + S.projects[0] : ''));
  if (!v) return;
  try { const info = await api('open', { path: v }); await openProject(info.path); } catch (e) { status(e.message, 'bad'); }
}
function about() {
  dialog('About LakeShark Studio',
    `<pre class="art-big">     /|      L A K E S H A R K\n    / |      S T U D I O\n   /__|__\n  ~~~~~~~~~  apps for the board, made on this PC</pre>` +
    `<p>New app, edit, build (F7), run in the simulator (F5), install on the board (F8).</p>` +
    `<p class="dlg-note">The run panel is the board's own screen.js fed by lssim, the simulator that runs the firmware's own code. ` +
    `The editor is CodeMirror 6 (MIT, static/vendor/CODEMIRROR-LICENSE.txt). Everything stays on 127.0.0.1.</p>`, [{ label: 'OK', fn: closeDialog }]);
}
function helpKeys() {
  const rows = [...CMDS.values()].filter((c) => keyOf(c.id)).map((c) => `<tr><td class="k">${esc(keyOf(c.id))}</td><td>${esc(c.title)}</td></tr>`).join('');
  dialog('Keyboard Shortcuts', `<table class="keys-table">${rows}<tr><td class="k">Alt+click, Ctrl+click</td><td>Another cursor</td></tr>` +
    `<tr><td class="k">Ctrl+Z / Ctrl+Y</td><td>Undo / redo (kept across saves)</td></tr><tr><td class="k">F2</td><td>Rename in the Explorer</td></tr>` +
    `<tr><td class="k">Del</td><td>Delete in the Explorer (to Studio's trash)</td></tr></table><p class="dlg-note">Change them in Settings > Keyboard.</p>`,
  [{ label: 'OK', fn: closeDialog }, { label: 'Settings...', fn: () => { closeDialog(); settingsPage('keys'); } }], true);
}
function settingsPage(tab) {
  const general = `<label class="field"><span>Scheme</span> <select id="s-scheme">${SCHEMES.map(([k, n]) => `<option value="${k}"${SET.scheme === k ? ' selected' : ''}>${n}</option>`).join('')}</select></label>` +
    `<label class="field"><input type="checkbox" id="s-dark"${SET.darkEditor ? ' checked' : ''}> a dark editor whatever the scheme</label>` +
    `<label class="field"><span>Editor text size</span> <input type="number" id="s-font" min="9" max="28" value="${SET.fontSize}"> px</label>` +
    `<label class="field"><input type="checkbox" id="s-indent"${SET.autoIndent ? ' checked' : ''}> auto-indent (Enter and closing braces line up the code)</label>` +
    `<label class="field"><input type="checkbox" id="s-fmt"${SET.formatOnSave ? ' checked' : ''}> format on save (${S.clang_format ? 'clang-format' : 'basic: no clang-format found'})</label>`;
  const keys = `<table class="keys-table" id="s-keys">${[...CMDS.values()].map((c) => `<tr data-id="${c.id}"><td>${esc(c.title)}</td><td class="k">${esc(keyOf(c.id) || '-')}</td>` +
    `<td><button class="key tiny" data-a="set">change</button> <button class="key tiny" data-a="clear">none</button> <button class="key tiny" data-a="reset">default</button></td></tr>`).join('')}</table>`;
  const d = dialog('Settings', `<div class="settab"><button class="key small" data-tab="general">General</button><button class="key small" data-tab="keys">Keyboard</button></div>` +
    `<div id="s-general">${general}</div><div id="s-keyspage" hidden><p class="dlg-note">Press change, then the keys. Esc leaves it as it was.</p>${keys}</div>`,
  [{ label: 'Done', fn: closeDialog }], true);
  const showTab = (n) => { $('s-general').hidden = n !== 'general'; $('s-keyspage').hidden = n !== 'keys'; };
  d.querySelectorAll('[data-tab]').forEach((b) => { b.onclick = () => showTab(b.dataset.tab); });
  showTab(tab || 'general');
  $('s-scheme').onchange = (e) => { SET.scheme = e.target.value; applySettings(); };
  $('s-dark').onchange = (e) => { SET.darkEditor = e.target.checked; applySettings(); };
  $('s-font').onchange = (e) => { SET.fontSize = Math.max(9, Math.min(28, +e.target.value || 14)); applySettings(); };
  $('s-indent').onchange = (e) => { SET.autoIndent = e.target.checked; applySettings(); };
  $('s-fmt').onchange = (e) => { SET.formatOnSave = e.target.checked; applySettings(); };
  $('s-keys').onclick = (e) => {
    const b = e.target.closest('button'); if (!b) return;
    const tr = b.closest('tr'), id = tr.dataset.id;
    const done = () => { tr.querySelector('.k').textContent = keyOf(id) || '-'; store.set('studio-settings', SET); tr.classList.remove('rec'); };
    if (b.dataset.a === 'clear') { SET.keys[id] = ''; done(); }
    else if (b.dataset.a === 'reset') { delete SET.keys[id]; done(); }
    else { tr.classList.add('rec'); tr.querySelector('.k').textContent = 'press keys...'; recording = (combo) => { recording = null; if (combo) SET.keys[id] = combo; done(); }; }
  };
}
function firstApp() { showPanel('docs'); const i = apiData ? apiData.sections.findIndex((s) => /5 minutes/.test(s.title)) : -1; if (i >= 0) showSection(i); }

// ---- AI pages
async function connectAI() {
  const c = await api('ai/config');
  const sdk = S.sdk.replace(/\\/g, '/');
  const py = S.python.replace(/\\/g, '/');
  const srv = `${sdk}/mcp/server.py`;
  const snips = [
    ['Claude Code', `claude mcp add lakeshark -- "${py}" "${srv}"`],
    ['Claude Desktop (claude_desktop_config.json)', JSON.stringify({ mcpServers: { lakeshark: { command: py, args: [srv] } } }, null, 2)],
    ['Codex (~/.codex/config.toml)', `[mcp_servers.lakeshark]\ncommand = "${py}"\nargs = ["${srv}"]`],
    ['VS Code (.vscode/mcp.json in your app folder)', JSON.stringify({ servers: { lakeshark: { type: 'stdio', command: py, args: [srv] } } }, null, 2)],
  ];
  const tools = c.tools.map((tl) => `<tr><td class="k">${esc(tl.name)}</td><td>${esc(tl.description)}${tl.board ? ' <span class="dlg-warn">(asks first: the board)</span>' : ''}</td></tr>`).join('');
  const d = dialog('Connect an AI',
    `<p>The LakeShark tools let an AI do what you do here: read the API, build, run the app in the simulator and look at its screen, and (when you say yes) install it on the board.</p>` +
    `<h3>In another AI tool, over MCP</h3><p class="dlg-note">The server is app-sdk/mcp; paste one of these:</p>` +
    snips.map(([n, s], i) => `<div>${esc(n)}</div><div class="snip">${esc(s)}<button class="key tiny" data-copy="${i}">copy</button></div>`).join('') +
    `<h3>In Studio's AI panel</h3>` +
    `<label class="field"><span>Model</span> <select id="ai-prov">${c.providers.map((p) => `<option value="${p.id}"${p.id === c.provider ? ' selected' : ''}>${esc(p.text)}</option>`).join('')}</select></label>` +
    `<label class="field"><span>Endpoint URL</span> <input type="text" id="ai-url" spellcheck="false" placeholder="https://.../v1  or  http://127.0.0.1:11434/v1" value="${esc(c.url)}"></label>` +
    `<label class="field"><span>Model name</span> <input type="text" id="ai-model" spellcheck="false" value="${esc(c.model)}"></label>` +
    `<label class="field"><span>Key from the variable</span> <input type="text" id="ai-env" spellcheck="false" value="${esc(c.key_env)}"></label>` +
    `<label class="field"><span>or a key</span> <input type="password" id="ai-key" placeholder="${c.key_set ? 'set (' + esc(c.key_from) + ')' : 'not set'}" autocomplete="off"></label>` +
    `<p class="dlg-note">A key typed here is kept in ${esc(c.settings_file)} on this PC, never in a repository, and never sent back to this page.</p>` +
    `<h3>The tools</h3><table class="keys-table">${tools}</table>`,
  [{ label: 'Save', fn: async () => {
    await api('ai/config', { provider: $('ai-prov').value, url: $('ai-url').value, model: $('ai-model').value, key_env: $('ai-env').value, key: $('ai-key').value || undefined });
    closeDialog(); aiStatus(); status('AI settings saved', 'good');
  } }, { label: 'Forget the key', fn: async () => { await api('ai/config', { forget_key: true }); closeDialog(); aiStatus(); } }, { label: 'Close', fn: closeDialog }], true);
  d.querySelectorAll('[data-copy]').forEach((b) => { b.onclick = () => copy(snips[+b.dataset.copy][1], 'the snippet'); });
}
function vscodePage() {
  const sdk = S.sdk.replace(/\\/g, '/');
  dialog('VS Code Users', `<p>Studio and VS Code work on the same folders. Everything Studio does is a command underneath, so VS Code gets it too:</p>` +
    `<table class="keys-table"><tr><td class="k">Ctrl+Shift+B</td><td>lsdk build: errors land in Problems, click to the line (problem matcher in .vscode/tasks.json)</td></tr>` +
    `<tr><td class="k">Run Task</td><td>lsdk: sim, sim frames, install, install and open, logs, studio</td></tr>` +
    `<tr><td class="k">typing lap_</td><td>snippets for every API call, with their arguments (.vscode/lakeshark.code-snippets)</td></tr>` +
    `<tr><td class="k">MCP</td><td>.vscode/mcp.json points Copilot or any MCP client at the LakeShark tools (Connect an AI has the text)</td></tr></table>` +
    `<h3>Where Studio still helps</h3><p>Keep Studio open beside VS Code for the run panel: the app drawn by the board's own code, with tilt, swipe and radio controls, ` +
    `and F8 to the real board. Studio picks up the changes VS Code saves when you press Run.</p>` +
    `<h3>Set up an app folder</h3><div class="snip">python "${esc(sdk)}/lsdk.py" new myapp\ncode myapp</div>` +
    `<p class="dlg-note">lsdk new writes .vscode/tasks.json, the snippets and c_cpp_properties.json for the SDK's headers.</p>`,
  [{ label: 'OK', fn: closeDialog }, { label: 'Connect an AI...', fn: () => { closeDialog(); connectAI(); } }], true);
}
let aiMsgs = [];
async function aiStatus() {
  try {
    const c = await api('ai/config');
    $('ai-state').textContent = c.provider === 'openai' ? `model: ${c.model || '?'}${c.key_set ? '' : ' (no key)'}` : 'no model: /tool commands';
  } catch (e) { /* later */ }
}
function aiShow(role, html) {
  const m = el(`<div class="msg ${role}"></div>`);
  m.innerHTML = html;
  $('chat').appendChild(m);
  $('chat').scrollTop = $('chat').scrollHeight;
  return m;
}
function aiResult(name, res) {
  let html = `<b>${esc(name)}</b>\n`;
  if (res && res.png) html += `<img src="${res.png}" alt="the simulator's screen">`;
  if (res && res.problems && res.problems.length) html += esc(res.problems.map(problemText).join('\n'));
  const rest = Object.assign({}, res); delete rest.png; delete rest.problems;
  html += '\n' + esc(JSON.stringify(rest, null, 1).slice(0, 4000));
  aiShow('tool', html);
}
async function aiSend(text, confirmed) {
  if (!confirmed) { aiShow('user', esc(text)); aiMsgs.push({ role: 'user', content: text }); }
  const r = await api('ai/chat', { messages: aiMsgs, project: P && P.path, confirmed: !!confirmed });
  if (r.messages) aiMsgs = r.messages;
  if (r.tool) aiResult(r.tool, r.result);
  if (r.reply) aiShow('ai', esc(r.reply));
  if (r.confirm) {
    const m = aiShow('ai', `Run <b>${esc(r.confirm.tool)}</b> ${esc(JSON.stringify(r.confirm.args))} on the board? <button class="key tiny" data-a="yes">Run it</button> <button class="key tiny" data-a="no">No</button>`);
    m.querySelector('[data-a=yes]').onclick = async () => {
      m.querySelectorAll('button').forEach((b) => b.remove());
      const res = await api('ai/run', { tool: r.confirm.tool, args: r.confirm.args, project: P && P.path, confirmed: true });
      aiResult(r.confirm.tool, res.result);
      if (r.messages) aiMsgs.push({ role: 'user', content: `(ran ${r.confirm.tool}: ${JSON.stringify(res.result).slice(0, 2000)})` });
    };
    m.querySelector('[data-a=no]').onclick = () => { m.querySelectorAll('button').forEach((b) => b.remove()); aiShow('ai', 'Not run.'); };
  }
}

// ================================================================== docs

function md(text) {
  const out = [];
  const lines = text.split('\n');
  let i = 0;
  const inline = (s) => esc(s).replace(/`([^`]+)`/g, '<code>$1</code>').replace(/\*\*([^*]+)\*\*/g, '<b>$1</b>');
  while (i < lines.length) {
    const l = lines[i];
    if (/^```/.test(l)) {
      const code = [];
      for (i++; i < lines.length && !/^```/.test(lines[i]); i++) code.push(lines[i]);
      out.push('<pre>' + esc(code.join('\n')) + '</pre>'); i++;
    } else if (/^#{2,4} /.test(l)) { out.push('<h4>' + inline(l.replace(/^#+ /, '')) + '</h4>'); i++; }
    else if (/^\s*\|/.test(l)) {
      const rows = [];
      for (; i < lines.length && /^\s*\|/.test(lines[i]); i++) if (!/^\s*\|[-| :]+\|$/.test(lines[i].trim())) rows.push(lines[i]);
      out.push('<table>' + rows.map((r, k) => '<tr>' + r.trim().replace(/^\||\|$/g, '').split('|').map((c) => (k ? '<td>' : '<th>') + inline(c.trim()) + (k ? '</td>' : '</th>')).join('') + '</tr>').join('') + '</table>');
    } else if (/^\s*([-*]|\d+\.) /.test(l)) {
      const items = [];
      for (; i < lines.length && (/^\s*([-*]|\d+\.) /.test(lines[i]) || (/^\s{2,}\S/.test(lines[i]) && items.length)); i++) {
        if (/^\s*([-*]|\d+\.) /.test(lines[i])) items.push(lines[i].replace(/^\s*([-*]|\d+\.) /, '')); else items[items.length - 1] += ' ' + lines[i].trim();
      }
      out.push('<ul>' + items.map((x) => '<li>' + inline(x) + '</li>').join('') + '</ul>');
    } else if (!l.trim()) i++;
    else {
      const para = [];
      for (; i < lines.length && lines[i].trim() && !/^(```|#|\s*\||\s*[-*] |\s*\d+\. )/.test(lines[i]); i++) para.push(lines[i]);
      out.push('<p>' + inline(para.join(' ')) + '</p>');
    }
  }
  return out.join('');
}
let docRows = [];
async function loadApi() {
  if (apiData) return apiData;
  apiData = await api('api');
  apiIndex = new Map(apiData.entries.map((e) => [e.name, e]));
  $('st-api').textContent = `API: ${apiData.entries.length} names`;
  return apiData;
}
async function listDocs() {
  await loadApi();
  const q = $('doc-q').value.trim().toLowerCase();
  docRows = [];
  apiData.sections.forEach((s, i) => { if (!q || s.title.toLowerCase().includes(q) || s.markdown.toLowerCase().includes(q)) docRows.push({ sec: i }); });
  apiData.entries.forEach((e, i) => { if (!q || e.name.toLowerCase().includes(q) || (q.length > 3 && e.doc.toLowerCase().includes(q))) docRows.push({ ent: i }); });
  const KIND = { function: 'fn', macro: '#', constant: 'k', type: 'T' };
  $('doc-list').innerHTML = docRows.slice(0, 400).map((r, k) => (r.sec !== undefined
    ? `<div class="p" data-k="${k}"><span class="k">\u00a7</span><span>${esc(apiData.sections[r.sec].title)}</span></div>`
    : `<div class="p" data-k="${k}"><span class="k">${KIND[apiData.entries[r.ent].kind] || ''}</span><span>${esc(apiData.entries[r.ent].name)}</span></div>`)).join('');
}
function showSection(i) { const s = apiData.sections[i]; $('doc-view').innerHTML = `<h3>${esc(s.title)}</h3>` + md(s.markdown); $('doc-view').scrollTop = 0; }
function showEntry(i) {
  const e = apiData.entries[i];
  $('doc-view').innerHTML = `<h3>${esc(e.name)}</h3><div class="sig">${esc(e.signature)}</div><p>${esc(e.doc || '')}</p><p class="hdr">${esc(e.kind)} in include/${esc(e.header)}</p>` +
    '<button class="key small" id="doc-ins">Insert at cursor</button> <button class="key small" id="doc-copy">Copy</button>';
  $('doc-ins').onclick = () => {
    const text = e.kind === 'function' ? e.name + '(' + (e.signature.match(/\((.*)\)/) || ['', ''])[1].split(',').map((a) => (a.trim().match(/(\w+)\s*(\[[^\]]*\])?$/) || ['', ''])[1]).filter((a) => a && a !== 'void').join(', ') + ')' : e.name;
    const view = ed(); if (view) { view.dispatch(view.state.replaceSelection(text)); view.focus(); }
  };
  $('doc-copy').onclick = () => copy(e.signature, e.name);
}
async function showDoc(name) {
  await loadApi();
  showPanel('docs');
  $('doc-q').value = name;
  await listDocs();
  const k = docRows.findIndex((r) => r.ent !== undefined && apiData.entries[r.ent].name === name);
  if (k >= 0) { showEntry(docRows[k].ent); $('doc-list').querySelector(`[data-k="${k}"]`)?.classList.add('sel'); }
}

// ================================================================== palette

function palette(mode) {
  const box = $('palette'), q = $('pal-q'), list = $('pal-list');
  box.hidden = false;
  q.value = mode === 'files' ? '' : '>';
  let hot = 0, items = [];
  const files = [];
  if (P) (function walk(l) { for (const f of l) { if (f.dir) walk(f.children); else files.push(f); } })(P.tree);
  const draw = () => {
    const s = q.value;
    const cmds = s.startsWith('>');
    const term = (cmds ? s.slice(1) : s).trim().toLowerCase();
    const fuzzy = (txt) => { let j = 0; for (const ch of txt.toLowerCase()) if (ch === term[j]) j++; return j === term.length; };
    items = cmds ? [...CMDS.values()].filter((c) => fuzzy(c.title)).map((c) => ({ t: c.title, r: keyOf(c.id), go: () => run(c.id) }))
      : files.filter((f) => fuzzy(f.path)).map((f) => ({ t: f.path, r: f.kind, go: () => openFile(f.path) }));
    hot = Math.min(hot, Math.max(0, items.length - 1));
    list.innerHTML = items.slice(0, 60).map((it, i) => `<div class="p${i === hot ? ' hot' : ''}" data-i="${i}"><span>${esc(it.t)}</span><span class="r">${esc(it.r)}</span></div>`).join('')
      || '<div class="none">Nothing matches.</div>';
  };
  const close = () => { box.hidden = true; };
  q.oninput = () => { hot = 0; draw(); };
  q.onkeydown = (e) => {
    if (e.key === 'Escape') close();
    else if (e.key === 'ArrowDown') { hot = Math.min(items.length - 1, hot + 1); draw(); e.preventDefault(); }
    else if (e.key === 'ArrowUp') { hot = Math.max(0, hot - 1); draw(); e.preventDefault(); }
    else if (e.key === 'Enter' && items[hot]) { close(); items[hot].go(); }
  };
  list.onclick = (e) => { const r = e.target.closest('.p'); if (r) { close(); items[+r.dataset.i].go(); } };
  box.onmousedown = (e) => { if (e.target === box) close(); };
  draw();
  q.focus();
}

// ================================================================== state polling

async function pollState() {
  try {
    const s = await api('state');
    const was = S.sim && S.sim.running;
    Object.assign(S, s);
    /* A sim that ended without Stop (the app quit, or Studio's server
       restarted with the desk bridge): the panel's screen kept reconnecting
       to nothing and flashed "not answering". Put it away instead. */
    if (was && !s.sim.running && !(popped && !popped.closed) && $('sim').src !== 'about:blank') {
      $('sim').src = 'about:blank';
      idle('\n   the simulator stopped: Run (F5) starts it\n');
    }
    const c = $('chip-sim');
    c.textContent = s.sim.running ? `SIM ${s.sim.app} ${s.sim.landscape ? 'landscape' : 'portrait'}${s.sim.no_radio ? ' no radio' : ''}` : 'SIM off';
    c.className = 'chip' + (s.sim.running ? ' live' : '');
    $('tail').checked = s.tailing;
  } catch (e) { /* next time */ }
}
async function pollBoard() {
  try {
    const b = await api('board');
    const c = $('chip-board');
    c.textContent = b.ok ? 'BOARD ' + (b.kind === 'bridge' ? 'via desk bridge' : b.text.replace('USB serial ', 'USB ')) : 'BOARD none';
    c.title = b.text;
    c.className = 'chip' + (b.ok ? ' live' : ' warn');
  } catch (e) { /* next time */ }
}

// ================================================================== the panels themselves

function makePanels() {
  panel('explorer', 'Explorer', `<div class="bar"><input id="tree-q" type="search" placeholder="filter files" spellcheck="false" style="flex:1;min-width:0"></div>` +
    `<div class="scroll"><div id="tree" class="tree" tabindex="0"></div></div>` +
    `<div class="bar"><button class="key tiny" data-cmd="file.newFile">+ file</button><button class="key tiny" data-cmd="file.newFolder">+ folder</button>` +
    `<button class="key tiny" data-cmd="app.openFolder">open folder</button><button class="key tiny" data-cmd="file.undoDelete">undo delete</button></div>`, { closable: true });
  panel('outline', 'Outline', `<div class="scroll"><div id="outline" class="list"></div></div>`);
  panel('search', 'Search', `<div class="bar"><input id="search-q" type="search" placeholder="search the app's files" spellcheck="false" style="flex:1;min-width:0">` +
    `<label><input type="checkbox" id="search-case">Aa</label><label><input type="checkbox" id="search-re">.*</label></div><div class="scroll"><div id="search-res" class="list"></div></div>`);
  panel('editor', 'Editor', `<div id="ftabs" class="ftabs"></div><div id="crumbs" class="crumbs"></div><div id="start" class="start" hidden></div>` +
    `<div id="panes" class="panes"><div class="pane" id="pane-A"><div class="host" id="host-A"></div><div class="asset" id="asset-A" hidden></div><div class="marks" id="marks-A"></div></div>` +
    `<div class="pane" id="pane-B" hidden><div class="phead" id="phead-B"><span></span><span class="x" title="Close the split">[x]</span></div>` +
    `<div class="host" id="host-B"></div><div class="asset" id="asset-B" hidden></div><div class="marks" id="marks-B"></div></div></div>`, { closable: false });
  panel('problems', 'Problems', `<div class="bar"><button class="key tiny" data-cmd="build.copyProblems">copy all</button><button class="key tiny" data-cmd="build.copyLog">copy build log</button>` +
    `<button class="key tiny" data-cmd="build.nextProblem">next F4</button></div><div class="scroll"><div id="problems" class="list"></div></div>`);
  panel('output', 'Output', `<div class="scroll"><pre id="output"></pre></div>`);
  panel('simlog', 'Sim log', `<div class="scroll"><pre id="simlog"></pre></div>`);
  panel('boardlog', 'Board log', `<div class="bar"><label><input type="checkbox" id="tail"> follow the board's console</label></div><div class="scroll"><pre id="boardlog"></pre></div>`);
  panel('terminal', 'Terminal', `<div class="bar"><button class="key tiny" data-term="build">build</button><button class="key tiny" data-term="sim --frames 30">sim --frames 30</button>` +
    `<button class="key tiny" data-term="info">info</button><button class="key tiny" data-term="list">list</button><button class="key tiny" data-term="help">help</button>` +
    `<button class="key tiny" data-a="term-stop">stop</button></div><div class="scroll"><pre id="termout"></pre></div>` +
    `<div class="term-in"><span>lsdk</span><input id="term-q" type="text" spellcheck="false" placeholder="build | sim --frames 30 | install | list | info --board | rm ID" autocomplete="off"></div>`);
  panel('run', 'Simulator', `<div class="bar"><button class="key tiny" data-cmd="build.run">Run F5</button><button class="key tiny" data-cmd="sim.restart">restart</button>` +
    `<button class="key tiny" data-cmd="sim.rotate">rotate</button><label><input type="checkbox" id="noradio"> no radio</label><button class="key tiny" data-cmd="sim.shot">shot</button>` +
    `<button class="key tiny" data-cmd="sim.popout">pop out</button><button class="key tiny" data-cmd="build.stop">stop</button></div>` +
    `<div class="runwrap"><iframe id="sim" title="The app in the simulator" src="about:blank"></iframe><pre id="sim-idle" class="idle">\n   .-----------------.\n   |  press RUN  F5  |\n   |   to see the    |\n   |  app running    |\n   '-----------------'\n</pre></div>` +
    `<div class="sensors"><div class="srow"><b>TILT</b><button class="key tiny" data-step="tilt=-0.5,0" title="left edge down">&lt;</button><button class="key tiny" data-step="tilt=0.5,0" title="right edge down">&gt;</button>` +
    `<button class="key tiny" data-step="tilt=0,0.5">^</button><button class="key tiny" data-step="tilt=0,-0.5">v</button><button class="key tiny" data-step="tilt=0,0,1">flat</button><button class="key tiny" data-a="shake">shake</button></div>` +
    `<div class="srow"><b>SWIPE</b><button class="key tiny" data-step="swipe=L">L</button><button class="key tiny" data-step="swipe=R">R</button><button class="key tiny" data-step="swipe=U">U</button>` +
    `<button class="key tiny" data-step="swipe=D">D</button><b>KEY</b><button class="key tiny" data-step="key=o">o</button><button class="key tiny" data-step="key=@enter">enter</button></div>` +
    `<div class="srow"><b>RADIO</b><button class="key tiny" data-step="radio=sim">SIM radio</button><button class="key tiny" data-step="radio=none">no radio</button>` +
    `<button class="key tiny" data-step="page=THIS IS A TEST" title="a pager decode on the timeline">page</button><button class="key tiny" data-step="aprs=WX 72F">aprs</button>` +
    `<button class="key tiny" data-step="mesh" title="the mesh is up, three nodes heard">mesh up</button><button class="key tiny" data-step="rx=1">rx +1</button>` +
    `<button class="key tiny" data-step="ble=6">ble 6</button></div><div class="srow"><b>CLOCK</b><button class="key tiny" data-step="hour=23">night</button><button class="key tiny" data-step="hour=12">noon</button></div></div>`);
  panel('docs', 'API docs', `<div class="bar"><input id="doc-q" type="search" placeholder="find: lap_ui_keys, swipe, waterfall..." autocomplete="off" style="flex:1;min-width:0"></div>` +
    `<div id="doc-list" class="list" style="padding:0 1ch"></div><div id="doc-view" class="doc scroll"></div>`);
  panel('ai', 'AI', `<div class="bar"><span id="ai-state" class="dlg-note"></span><span class="grow"></span><button class="key tiny" data-cmd="ai.connect">connect</button>` +
    `<button class="key tiny" data-a="ai-clear">clear</button></div><div class="chat" id="chat"><div class="msg ai">Ask about the app, or run a tool: /build, /sim_run, /sdk_docs lap_ui_keys, /install. ` +
    `Tools that change the board ask first.</div></div><div class="chatin"><textarea id="ai-q" placeholder="/sim_run   or a question (Ctrl+Enter sends)"></textarea><button class="key small" data-a="ai-send">Send</button></div>`);
}

function wire() {
  $('tree-q').addEventListener('input', drawTree);
  const tree = $('tree');
  tree.addEventListener('click', treeClick);
  tree.addEventListener('contextmenu', (e) => { if (P) treeMenu(e); else e.preventDefault(); });
  tree.addEventListener('keydown', (e) => {
    if (!P) return;
    const cur = [...P.sel][0];
    if (e.key === 'F2' && cur) { e.preventDefault(); startRename(cur); }
    else if (e.key === 'Delete' && P.sel.size) { e.preventDefault(); deletePaths([...P.sel]); }
    else if ((e.key === 'ArrowDown' || e.key === 'ArrowUp') && visible.length) {
      e.preventDefault();
      const i = Math.max(0, Math.min(visible.length - 1, visible.findIndex((f) => f.path === cur) + (e.key === 'ArrowDown' ? 1 : -1)));
      P.sel = new Set([visible[i].path]); P.anchor = visible[i].path; drawTree();
    } else if (e.key === 'Enter' && cur) { const f = findEntry(cur); if (f && !f.dir) openFile(cur); }
  });
  tree.addEventListener('dragstart', (e) => {
    const row = e.target.closest('.row'); if (!row) return;
    if (!P.sel.has(row.dataset.path)) { P.sel = new Set([row.dataset.path]); }
    e.dataTransfer.setData('application/x-studio-paths', JSON.stringify([...P.sel]));
    e.dataTransfer.effectAllowed = 'move';
  });
  tree.addEventListener('dragover', (e) => {
    e.preventDefault();
    tree.querySelectorAll('.over').forEach((x) => x.classList.remove('over'));
    const row = e.target.closest('.row, .root');
    if (row) row.classList.add('over');
  });
  tree.addEventListener('dragleave', (e) => { if (!tree.contains(e.relatedTarget)) tree.querySelectorAll('.over').forEach((x) => x.classList.remove('over')); });
  tree.addEventListener('drop', async (e) => {
    e.preventDefault();
    tree.querySelectorAll('.over').forEach((x) => x.classList.remove('over'));
    if (!P) return;
    const row = e.target.closest('.row, .root');
    const dir = folderOf(row ? row.dataset.path : '');
    const inner = e.dataTransfer.getData('application/x-studio-paths');
    if (inner) await moveTo(JSON.parse(inner), dir);
    else if (e.dataTransfer.files.length) await uploadFiles([...e.dataTransfer.files], dir);
  });
  $('phead-B').querySelector('.x').onclick = closeSplit;
  $('problems').addEventListener('click', (e) => {
    const b = e.target.closest('[data-copy]');
    if (b) { copy(problemText(problems()[+b.dataset.copy]), 'the error'); return; }
    const row = e.target.closest('.p');
    if (!row || getSelection().toString()) return;
    const p = problems()[+row.dataset.i];
    markProblem(+row.dataset.i);
    if (p.path) openFile(p.path, p.line, p.col);
  });
  $('problems').addEventListener('contextmenu', (e) => {
    const row = e.target.closest('.p');
    const p = row && problems()[+row.dataset.i];
    ctx(e, [['Copy message', '', () => copy(p.message, 'the message'), { off: !p }], ['Copy with its place', '', () => copy(problemText(p), 'the error'), { off: !p }],
      ['Copy all', '', () => run('build.copyProblems')], ['Go to line', '', () => p && p.path && openFile(p.path, p.line, p.col), { off: !p || !p.path }], null, C('build.copyLog')]);
  });
  for (const [id, name] of [['output', 'build output'], ['simlog', 'sim log'], ['boardlog', 'board log'], ['termout', 'terminal']]) $(id).closest('.scroll').addEventListener('contextmenu', logMenu($(id), name));
  $('tail').addEventListener('change', (e) => setTail(e.target.checked));
  $('term-q').addEventListener('keydown', (e) => {
    if (e.key === 'Enter') { termRun(e.target.value); e.target.value = ''; }
    else if (e.key === 'ArrowUp') { termAt = Math.max(0, termAt - 1); e.target.value = termHist[termAt] || ''; e.preventDefault(); }
    else if (e.key === 'ArrowDown') { termAt = Math.min(termHist.length, termAt + 1); e.target.value = termHist[termAt] || ''; e.preventDefault(); }
  });
  let st = 0;
  $('search-q').addEventListener('input', () => { clearTimeout(st); st = setTimeout(projectSearch, 300); });
  $('search-case').onchange = projectSearch; $('search-re').onchange = projectSearch;
  $('doc-q').addEventListener('input', listDocs);
  $('doc-list').addEventListener('click', (e) => {
    const row = e.target.closest('.p'); if (!row) return;
    $('doc-list').querySelectorAll('.p.sel').forEach((x) => x.classList.remove('sel'));
    row.classList.add('sel');
    const r = docRows[+row.dataset.k];
    if (r.sec !== undefined) showSection(r.sec); else showEntry(r.ent);
  });
  $('noradio').checked = noRadio;
  $('noradio').onchange = (e) => { noRadio = e.target.checked; store.set('studio-noradio', noRadio); if (S.sim.running) guarded('build.run', runSim); };
  $('ai-q').addEventListener('keydown', (e) => { if (e.key === 'Enter' && e.ctrlKey) { e.preventDefault(); document.querySelector('[data-a=ai-send]').click(); } });
  for (const pn of panes) $('host-' + pn.id).addEventListener('contextmenu', (e) => {   // the gutter and the space below the text too
    if (e.defaultPrevented || !pn.view) return;
    focusPane = pn;
    ctx(e, editorMenu(pn.view));
  });
  $('host-A').addEventListener('mousedown', () => { focusPane = panes[0]; });
  $('host-B').addEventListener('mousedown', () => { focusPane = panes[1]; });
  for (const id of ['A', 'B']) $('host-' + id).addEventListener('wheel', () => setTimeout(updateMarks, 50), { passive: true });
  document.addEventListener('click', (e) => {
    const b = e.target.closest('[data-cmd], [data-step], [data-a], [data-term]');
    if (!b || b.closest('.drop') || b.closest('.dialog')) return;
    if (b.dataset.cmd) run(b.dataset.cmd);
    else if (b.dataset.step) step(b.dataset.step).catch((x) => status(x.message, 'bad'));
    else if (b.dataset.term) termRun(b.dataset.term);
    else if (b.dataset.a === 'term-stop') api('term', { stop: true });
    else if (b.dataset.a === 'shake') (async () => { for (const s of ['tilt=0.9,0.2', 'tilt=-0.9,-0.2', 'tilt=0.9,0', 'tilt=-0.9,0', 'tilt=0,0,1']) { await step(s); await sleep(90); } })();
    else if (b.dataset.a === 'ai-send') { const q = $('ai-q'); const text = q.value.trim(); if (text) { q.value = ''; aiSend(text).catch((x) => aiShow('ai', esc(x.message))); } }
    else if (b.dataset.a === 'ai-clear') { aiMsgs = []; $('chat').innerHTML = ''; }
  });
  window.addEventListener('beforeunload', (e) => { if (projects.some((p) => dirtyFiles(p).length)) e.preventDefault(); });
  window.addEventListener('resize', () => updateMarks());
}

function defineCommands() {
  cmd('app.new', 'New app', '', newAppDialog);
  cmd('app.openFolder', 'Open folder', 'Ctrl+O', openFolder);
  cmd('app.back', 'Back to the previous app', 'Ctrl+Alt+ArrowLeft', backProject);
  cmd('app.close', 'Close this app', '', () => P && closeProject(P));
  cmd('file.save', 'Save', 'Ctrl+S', () => save());
  cmd('file.saveAll', 'Save all', 'Ctrl+Shift+S', saveAll);
  cmd('file.newFile', 'New file', 'Ctrl+Alt+N', () => newFile('file'));
  cmd('file.newFolder', 'New folder', '', () => newFile('dir'));
  cmd('file.closeTab', 'Close the file', 'Ctrl+Alt+W', () => curPath() && closeFile(curPath()));
  cmd('file.quickOpen', 'Go to file', 'Ctrl+P', () => palette('files'));
  cmd('file.undoDelete', 'Undo delete', '', undoDelete);
  cmd('edit.find', 'Find', 'Ctrl+F', () => ed() && CM.openSearchPanel(ed()));
  cmd('edit.replace', 'Find and replace', 'Ctrl+H', () => { const v = ed(); if (!v) return; CM.openSearchPanel(v); setTimeout(() => v.dom.querySelector('.cm-search input[name=replace]')?.focus(), 30); });
  cmd('edit.gotoLine', 'Go to line', 'Ctrl+G', () => ed() && CM.gotoLine(ed()));
  cmd('edit.comment', 'Comment the line', 'Ctrl+/', () => ed() && CM.toggleComment(ed()));
  cmd('edit.format', 'Format the file', 'Shift+Alt+F', format);
  cmd('edit.selectNext', 'Add the next match (another cursor)', 'Ctrl+D', () => ed() && CM.selectNextOccurrence(ed()));
  cmd('edit.gotoDef', 'Go to definition', 'F12', gotoDef);
  cmd('build.build', 'Build for the board', 'F7', () => guarded('build.build', build));
  cmd('build.run', 'Run in the simulator', 'F5', () => guarded('build.run', runSim));
  cmd('build.stop', 'Stop the simulator', 'Shift+F5', stopSim);
  cmd('build.nextProblem', 'Next problem', 'F4', nextProblem);
  cmd('build.copyLog', 'Copy build log', '', () => copy($('output').textContent || '(no build yet)', 'the build log'));
  cmd('build.copyProblems', 'Copy all problems', '', () => copy(problems().map(problemText).join('\n') || 'No problems.', 'the problems'));
  cmd('board.install', 'Install on the board', 'F8', () => guarded('board.install', install));
  cmd('board.open', 'Open on the board', 'Shift+F8', () => guarded('board.open', boardOpen));
  cmd('board.log', 'Board log', '', () => { showPanel('boardlog'); setTail(true); });
  cmd('sim.restart', 'Restart the simulator', 'Ctrl+F5', () => guarded('build.run', runSim));
  cmd('sim.rotate', 'Rotate the simulator', '', () => { landscape = !landscape; store.set('studio-landscape', landscape); guarded('build.run', runSim); });
  cmd('sim.noRadio', 'Board without a radio', '', () => { noRadio = !noRadio; $('noradio').checked = noRadio; store.set('studio-noradio', noRadio); guarded('build.run', runSim); });
  cmd('sim.shot', 'Screenshot of the simulator', 'Ctrl+Alt+P', screenshot);
  cmd('sim.popout', 'Pop out the simulator', '', popOut);
  cmd('view.palette', 'Command palette', 'Ctrl+Shift+P', () => palette('cmds'));
  cmd('view.split', 'Split the editor', 'Ctrl+\\', () => ($('pane-B').hidden ? openSplit(curPath()) : closeSplit()));
  cmd('view.search', 'Search in the app', 'Ctrl+Shift+F', () => { showPanel('search'); $('search-q').focus(); });
  cmd('view.problems', 'Problems', 'Ctrl+Shift+M', () => showPanel('problems'));
  cmd('view.terminal', 'Terminal', 'Ctrl+`', () => { showPanel('terminal'); $('term-q').focus(); });
  cmd('view.settings', 'Settings', 'Ctrl+,', () => settingsPage());
  cmd('view.help', 'Keyboard shortcuts', 'F1', helpKeys);
  cmd('view.start', 'Start page', '', showStart);
  cmd('view.bigger', 'Bigger text', 'Ctrl+=', () => fontStep(1));
  cmd('view.smaller', 'Smaller text', 'Ctrl+-', () => fontStep(-1));
  for (const n of Object.keys(PRESETS)) cmd('layout.' + n.toLowerCase(), 'Layout: ' + n, '', () => setLayout(PRESETS[n], n));
  cmd('layout.save', 'Save the layout as...', '', saveLayoutAs);
  cmd('ai.connect', 'Connect an AI', '', connectAI);
  cmd('ai.vscode', 'VS Code users', '', vscodePage);
}

// ================================================================== start

(async function start() {
  applySettings();
  defineCommands();
  makePanels();
  buildMenubar();
  wire();
  const saved = store.get('studio-layout', null);
  setLayout(saved && saved.root ? saved : PRESETS.Default);
  makePane(panes[0]);
  await refreshState();
  loadApi().catch(() => {});
  showProblems([], '');
  pollLogs();
  pollBoard();
  setInterval(pollBoard, 6000);
  setInterval(pollState, 3000);
  pollState();
  aiStatus();
  const open = store.get('studio-open', { paths: [] });
  for (const path of open.paths || []) { try { const info = await api('project?' + pq(path)); projects.push({ path: info.path, name: info.name, id: info.id, tree: info.tree, files: new Map(), order: [], active: null, split: null, expanded: new Set(), sel: new Set(), anchor: null, problems: [] }); } catch (e) { /* gone */ } }
  const active = proj(open.active) || projects[0];
  if (active) await switchProject(active); else { drawProjTabs(); drawTree(); showStart(); }
  if (S.sim.running) loadSim();
  const want = new URLSearchParams(location.search).get('show');
  if (want === 'start') showStart();
})();
