// No browser/window is opened. Execute the actual viewer request handlers.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const source = fs.readFileSync(process.argv[2], 'utf8');
const start = source.indexOf('let hostTransfer = null;');
const end = source.indexOf('\nfunction say(text)', start);
const messages = [], dialogs = [], saves = [], opens = [];
function element(tag) {
  const node = { tag, style:{}, dataset:{}, children:[], disabled:false, append(...children){this.children.push(...children);}, close(){this.closed=true;}, remove(){this.removed=true;}, showModal(){dialogs.push(this);} };
  return node;
}
const window = { addEventListener(){}, pywebview:{api:{
  async open_url(url){opens.push(url);return 1;},
  async save_file(name,data){saves.push([name,Buffer.from(data,'base64')]);return 1;}
}}};
window.parent=window;
const context = {window, document:{createElement:element,body:element('body')},
  TextDecoder,Uint8Array,devicePixelRatio:1.5,live:{},URL,Number,setTimeout,clearTimeout,
  btoa:s=>Buffer.from(s,'binary').toString('base64'), say:s=>messages.push(s)};
vm.createContext(context);
vm.runInContext(source.slice(start,end)+'\nthis.handlers={hostRecord,hostHello};', context);
const {hostRecord,hostHello}=context.handlers;
const hex=s=>Buffer.from(s).toString('hex');
(async()=>{
  hostHello();assert.equal(messages.pop(),'term desk hello 3 1500');
  assert(hostRecord(`term: desk1 1 1 0 0 ${hex('https://example.com')} -`));
  assert.equal(opens.length,0); // URL must be visible before an OS action.
  assert.equal(dialogs[0].children[1].textContent,'https://example.com');
  await dialogs[0].children[2].onclick();
  assert.deepEqual(opens,['https://example.com']);assert.equal(messages.pop(),'term desk result 1 1');
  hostRecord(`console term: desk1 2 2 0 4 ${hex('out.bin')} 0001`);
  assert.equal(dialogs.length,1);
  hostRecord(`term: desk1 2 2 2 4 ${hex('out.bin')} 02ff`);
  await dialogs[1].children[2].onclick();
  assert.equal(saves[0][0],'out.bin');assert.deepEqual(saves[0][1],Buffer.from([0,1,2,255]));
  assert.equal(messages.pop(),'term desk result 2 1');
  hostRecord(`term: desk1 3 1 0 0 ${hex('http://example.com')} -`);
  await dialogs[2].children[2].onclick();assert.equal(opens.length,1);assert.equal(messages.pop(),'term desk result 3 -5');
  hostRecord(`term: desk1 4 2 0 65537 ${hex('too-big')} 00`);assert.equal(messages.pop(),'term desk result 4 -5');
  hostRecord(`term: desk1 5 2 0 4 ${hex('out.bin')} 00`);
  hostRecord(`term: desk1 5 2 2 4 ${hex('out.bin')} 02`);assert.equal(messages.pop(),'term desk result 5 -5');
  hostRecord(`term: desk1 6 1 0 0 ${hex('https://example.com')} -`);
  hostRecord('term: deskcancel 6');assert(dialogs[3].closed&&dialogs[3].removed);
  console.log('desk1 viewer handlers PASS');
})().catch(error=>{console.error(error);process.exitCode=1;});
