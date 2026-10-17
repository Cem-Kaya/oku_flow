// Desktop-free contracts for the actual script compiled into local notes.
// Only browser primitives and file handles are simulated; no model or media runs.
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import {dirname, resolve} from 'node:path';
import vm from 'node:vm';
import test from 'node:test';

const root = resolve(dirname(fileURLToPath(import.meta.url)), '..');
const header = await readFile(resolve(root, 'include/okuflow/common/notes_transcribe_bridge.hpp'), 'utf8');
const source = [...header.matchAll(/R"JS\(([\s\S]*?)\)JS"/g)].map(match => match[1]).join('')
  .replace(/^<script>\s*/, '').replace(/\s*<\/script>\s*$/, '');
assert(source.includes('DOMContentLoaded'), 'Actual embedded bridge source was not extracted.');

function deferred() {
  let resolve;
  const promise = new Promise(complete => { resolve = complete; });
  return {promise, resolve};
}
class Element {
  constructor() { this.dataset = {}; this.hidden = false; this.disabled = false; this.textContent = ''; this.children = []; this.handlers = {}; }
  addEventListener(name, handler) { this.handlers[name] = handler; }
  append(...children) { this.children.push(...children); }
  replaceChildren(...children) { this.children = [...children]; }
}
async function harness() {
  const nodes = new Map();
  const card = new Element();
  card.dataset.transcribeSrc = '../Recordings/video.mp4';
  card.querySelector = selector => {
    if (!nodes.has(selector)) nodes.set(selector, new Element());
    return nodes.get(selector);
  };
  const listeners = new Map();
  const channels = [];
  const writes = [];
  const entered = deferred(), release = deferred();
  let creates = 0;
  const media = {name: 'video.mp4', size: 42, lastModified: 1234};
  const directory = {
    async getDirectoryHandle() { return directory; },
    async getFileHandle(name, options) {
      if (!options?.create) {
        if (name.endsWith('.okuflow-transcript.json')) throw new Error('NotFound');
        return {async getFile() { return media; }};
      }
      ++creates;
      if (creates === 1) { entered.resolve(); await release.promise; }
      return {async createWritable() {
        let bytes;
        return {
          async write(value) { bytes = value; },
          async close() { writes.push(JSON.parse(bytes)); },
          async abort() {},
        };
      }};
    },
  };
  const folder = {...directory, name: 'OkuFlow', async queryPermission() { return 'granted'; }};
  const popup = {closed: false, focus() {}, postMessage() {}};
  const window = {
    showDirectoryPicker: async () => folder,
    open: () => popup,
    addEventListener: (name, callback) => listeners.set(name, callback),
    removeEventListener: (name, callback) => { if (listeners.get(name) === callback) listeners.delete(name); },
  };
  let ready;
  const document = {
    documentElement: {lang: 'en'}, title: 'Fixture notes',
    querySelectorAll: () => [card], createElement: () => new Element(),
    addEventListener: (name, callback) => { if (name === 'DOMContentLoaded') ready = callback; },
  };
  vm.runInNewContext(source, {
    window, document, location: {href: 'file:///fixture/OkuFlow/Notes/notes.html'},
    URL, Blob, crypto: {randomUUID: () => 'fixture-id'},
    showDirectoryPicker: window.showDirectoryPicker,
    setInterval: () => 1, clearInterval() {}, setTimeout: () => 2, clearTimeout() {},
    MessageChannel: class {
      constructor() {
        this.port1 = {onmessage: null, postMessage() {}, close() {}};
        this.port2 = {};
        channels.push(this);
      }
    },
  });
  await ready();
  const start = card.querySelector('.transcribe-start');
  await start.handlers.click(); // folder grant is deliberately its own click
  return {
    card, writes, entered, release, get creates() { return creates; },
    async begin() { assert.equal(start.disabled, false); await start.handlers.click(); return channels.at(-1); },
    done(channel, text) { return channel.port1.onmessage({data: {
      type: 'done', model: 'fixture-model', device: 'cpu', language: 'en',
      segments: [{start: 0, end: 1, text}],
    }}); },
    status() { return card.querySelector('.transcribe-status').textContent; },
  };
}

test('completed transcript is immutable while a new run clears the visible segments', async () => {
  const h = await harness();
  const first = await h.begin();
  const saving = h.done(first, 'complete first transcript');
  await h.entered.promise;
  await h.begin();
  const nextStatus = h.status();
  assert.equal(h.card.segments.length, 0);
  h.release.resolve();
  await saving;
  assert.equal(h.writes[0].segments[0]?.text, 'complete first transcript');
  assert.equal(h.status(), nextStatus, 'Older save must not overwrite the active run status.');
});

test('sidecar writes for the same media serialize in completion order', async () => {
  const h = await harness();
  const first = await h.begin();
  const savingFirst = h.done(first, 'first transcript');
  await h.entered.promise;
  const second = await h.begin();
  const savingSecond = h.done(second, 'second transcript');
  const prematureCreateCount = h.creates;
  h.release.resolve();
  await Promise.all([savingFirst, savingSecond]);
  assert.equal(prematureCreateCount, 1, 'The successor must wait for the previous sidecar writer.');
  assert.deepEqual(h.writes.map(write => write.segments[0]?.text), ['first transcript', 'second transcript']);
});
