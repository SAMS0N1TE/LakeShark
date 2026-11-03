// Headless integration with the real Studio page, local HTTP fixtures and CDP.
// node host/installer/test_updater_browser.js [path/to/chrome-headless-shell.exe]
const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const { spawn } = require('node:child_process');
const { once } = require('node:events');
const repo = path.resolve(__dirname, '../..');
const evidence = fs.mkdtempSync(path.join(repo, 'build_installer/updater-browser-'));
const python = spawn('python', ['-u', '-c', `
import json, os, sys, threading
from pathlib import Path
sys.path[:0] = ['host/installer', 'app-sdk/studio']
from test_updater import Fixture, manifest
from http.server import ThreadingHTTPServer
import server
class Feed(Fixture):
    def do_GET(self):
        if self.path == '/same': type(self).data = manifest('2.20.0-rc1')
        if self.path == '/newer': type(self).data = manifest()
        super().do_GET()
feed = ThreadingHTTPServer(('127.0.0.1', 0), Feed)
threading.Thread(target=feed.serve_forever, daemon=True).start()
os.environ['LSDK_UPDATER_TEST_URL'] = 'http://127.0.0.1:%d/latest.json' % feed.server_port
os.environ['LSDK_STUDIO_SETTINGS'] = str(Path(sys.argv[1]) / 'studio.json')
studio = server.serve(0, Path(sys.argv[1]) / 'workspace')
print(json.dumps({'studio': 'http://127.0.0.1:%d/' % studio.server_port, 'feed': 'http://127.0.0.1:%d' % feed.server_port}))
studio.serve_forever()
`, evidence], { cwd: repo, stdio: ['ignore', 'pipe', 'pipe'], windowsHide: true });
let chrome, ws;
const delay = ms => new Promise(r => setTimeout(r, ms));
async function until(fn, message, timeout = 15000) {
  const end = Date.now() + timeout;
  while (Date.now() < end) { if (await fn()) return; await delay(100); }
  throw Error(message);
}
(async () => {
  let output = '', pythonErrors = '';
  python.stdout.on('data', b => { output += b; });
  python.stderr.on('data', b => { pythonErrors += b; });
  await until(() => output.includes('\n') || python.exitCode !== null, 'fixture failed to start');
  if (python.exitCode !== null) throw Error(pythonErrors);
  const fixture = JSON.parse(output.trim().split('\n')[0]);
  const shell = process.argv[2] || path.join(process.env.LOCALAPPDATA,
    'ms-playwright/chromium_headless_shell-1234/chrome-headless-shell-win64/chrome-headless-shell.exe');
  chrome = spawn(shell, ['--headless', '--disable-gpu', '--no-sandbox', '--remote-debugging-address=127.0.0.1',
    '--remote-debugging-port=0', '--user-data-dir=' + path.join(evidence, 'chrome'), 'about:blank'],
    { stdio: ['ignore', 'ignore', 'pipe'], windowsHide: true });
  let errors = '';
  chrome.stderr.on('data', b => { errors += b; });
  await until(() => errors.includes('DevTools listening on') || chrome.exitCode !== null, 'headless browser failed to start');
  if (chrome.exitCode !== null) throw Error(errors);
  const browserURL = errors.match(/DevTools listening on (ws:\/\/[^\s]+)/)[1];
  const port = new URL(browserURL).port;
  const target = await (await fetch('http://127.0.0.1:' + port + '/json/new?about:blank', { method: 'PUT' })).json();
  ws = new WebSocket(target.webSocketDebuggerUrl);
  await once(ws, 'open');
  let seq = 0;
  const pending = new Map();
  ws.addEventListener('message', e => {
    const m = JSON.parse(e.data);
    if (pending.has(m.id)) { const [resolve, reject] = pending.get(m.id); pending.delete(m.id); m.error ? reject(Error(JSON.stringify(m.error))) : resolve(m.result); }
  });
  const cdp = (method, params = {}) => new Promise((resolve, reject) => {
    const id = ++seq; pending.set(id, [resolve, reject]); ws.send(JSON.stringify({ id, method, params }));
  });
  const evaluate = async expression => {
    const r = await cdp('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
    if (r.exceptionDetails) throw Error(JSON.stringify(r.exceptionDetails));
    return r.result.value;
  };
  await cdp('Page.navigate', { url: fixture.studio });
  await until(() => evaluate(`!!document.querySelector('#updater-notice a')`), 'source update banner missing');
  assert.match(await evaluate(`document.querySelector('#updater-notice').textContent`), /LakeShark 2\.20\.1 is out:/);
  assert.match(await evaluate(`document.querySelector('#updater-notice a').href`), /github\.com\/SAMS0N1TE\/LakeShark\/releases\/tag\/v2\.20\.1/);
  assert.equal(await evaluate(`getComputedStyle(document.querySelector('#updater-notice .key')).borderRadius`), '0px');
  async function helpItem(text) {
    await evaluate(`document.dispatchEvent(new KeyboardEvent('keydown', {key: 'Escape'})); document.querySelector('[data-menu="Help"]').dispatchEvent(new MouseEvent('mousedown', {bubbles:true}))`);
    await until(() => evaluate(`!!document.querySelector('[data-updater]')`), 'Help updater entries missing');
    await evaluate(`Array.from(document.querySelectorAll('[data-updater]')).find(n => n.textContent.includes(${JSON.stringify(text)})).click()`);
  }
  await helpItem('Automatic update checks');
  await until(() => evaluate(`document.querySelector('#updater-notice').textContent.includes('checks off')`), 'automatic toggle failed');
  const status = await (await fetch(fixture.studio + 'api/updater/status')).json();
  assert.equal(status.automatic, false);
  await fetch(fixture.feed + '/same');
  await helpItem('Check for updates');
  await until(() => evaluate(`document.querySelector('#updater-notice').textContent.includes('up to date')`), 'manual same-version check failed');
  await fetch(fixture.feed + '/newer');
  await helpItem('Check for updates');
  await until(() => evaluate(`!!document.querySelector('#updater-notice a')`), 'manual newer-version check failed');
  await evaluate(`Array.from(document.querySelectorAll('#updater-notice button')).find(n => n.textContent.includes('Later')).click()`);
  assert.equal(await evaluate(`document.querySelector('#updater-notice').hidden`), true);
  await cdp('Page.navigate', { url: fixture.studio + 'static/desktop.html?board=127.0.0.1' });
  await until(() => evaluate(`!!document.querySelector('#updater-notice a')`), 'network Desktop notice missing');
  assert.equal(await evaluate(`document.querySelector('#updater-controls')`), null);   // no standing bar on the desktop
  assert.equal(await evaluate(`Array.from(document.querySelectorAll('#updater-notice .key')).every(n => getComputedStyle(n).borderRadius === '0px')`), true);
  fs.writeFileSync(path.join(evidence, 'results.json'), JSON.stringify({ passed: true, checks: ['source release link', 'square character style', 'Help entries', 'automatic preference persists', 'manual no-update', 'manual newer update', 'Later hides banner', 'network Desktop controls', 'Desktop square character style'] }, null, 2));
  console.log('PASS: 9 headless updater UI checks; evidence ' + evidence);
})().catch(e => { console.error(e); process.exitCode = 1; }).finally(async () => {
  if (ws) ws.close();
  if (chrome) chrome.kill();
  python.kill();
});
