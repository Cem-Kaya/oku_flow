// Desktop-free contracts for the actual script compiled into local notes.
// Only browser primitives and file handles are simulated; no model or media runs.
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {fileURLToPath} from 'node:url';
import {dirname, resolve} from 'node:path';
import vm from 'node:vm';
import test from 'node:test';
import {createHash, webcrypto} from 'node:crypto';

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
function fileFingerprint(bytes) {
  let digest = Buffer.alloc(32);
  for (let offset = 0; offset < bytes.length; offset += 1024 * 1024)
    digest = createHash('sha256').update(digest).update(bytes.subarray(offset, offset + 1024 * 1024)).digest();
  return digest.toString('hex');
}
const transcriptName = identity => '.okuflow-transcript-' + identity.mediaId + '.json';
async function harness({wrongBytes = false, wrongId = false, saved = null, legacySaved = null,
  bytes = Buffer.from('fixture recording'), holdWrite = true, writeBarrier = 'handle'} = {}) {
  const nodes = new Map();
  const card = new Element();
  card.dataset.transcribeSrc = '../Recordings/video.mp4';
  const identity = {version: 1, mediaId: '12345678-1234-1234-1234-123456789abc', name: 'video.mp4',
    size: bytes.length, algorithm: 'sha256-chain-1m-v1', fingerprint: fileFingerprint(bytes)};
  const files = new Map(), reads = [];
  if (saved) files.set(transcriptName(identity), saved);
  if (legacySaved) files.set('video.mp4.okuflow-transcript.json', legacySaved);
  card.dataset.recordingIdentity = JSON.stringify(identity);
  card.querySelector = selector => {
    if (!nodes.has(selector)) nodes.set(selector, new Element());
    return nodes.get(selector);
  };
  const listeners = new Map();
  const channels = [];
  const writes = [];
  const sent = [];
  const entered = deferred(), release = deferred();
  let creates = 0;
  let content = wrongBytes ? Buffer.alloc(bytes.length, 7) : bytes;
  let marker = {...identity, ...(wrongId ? {mediaId: '87654321-1234-1234-1234-123456789abc'} : {})};
  let maxRead = 0;
  const media = () => Object.assign(new Blob([content]), {name: 'video.mp4', lastModified: 1234,
    slice(start, end) { maxRead = Math.max(maxRead, end - start); return new Blob([content.subarray(start, end)]); }});
  const directory = {
    async getDirectoryHandle() { return directory; },
    async getFileHandle(name, options) {
      if (!options?.create) {
        reads.push(name);
        if (name.includes('okuflow-transcript')) {
          if (!files.has(name)) throw new Error('NotFound');
          return {async getFile() { return new Blob([JSON.stringify(files.get(name))]); }};
        }
        if (name.endsWith('.okuflow-media.json')) return {async getFile() { return new Blob([JSON.stringify(marker)]); }};
        return {async getFile() { return media(); }};
      }
      ++creates;
      const firstWrite = creates === 1 && holdWrite;
      if (firstWrite && writeBarrier === 'handle') { entered.resolve(); await release.promise; }
      return {async createWritable() {
        if (firstWrite && writeBarrier === 'writable') { entered.resolve(); await release.promise; }
        let bytes;
        return {
          async write(value) { bytes = value; },
          async close() { const payload = JSON.parse(bytes); files.set(name, payload); writes.push(payload); },
          async abort() {},
        };
      }};
    },
  };
  const folder = {...directory, name: 'OkuFlow', async queryPermission() { return 'granted'; }};
  const popup = {closed: false, focus() {}, postMessage(message) { sent.push(message); }};
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
    URL, Blob, Uint8Array, crypto: {randomUUID: () => 'fixture-id', subtle: webcrypto.subtle},
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
    card, writes, entered, release, identity, sent, files, reads, get maxRead() { return maxRead; }, get creates() { return creates; },
    replaceBytes() { content = Buffer.alloc(bytes.length, 8); },
    installSuccessor() {
      content = Buffer.alloc(bytes.length, 8);
      marker = {...identity, mediaId: '87654321-1234-1234-1234-123456789abc', fingerprint: fileFingerprint(content)};
      const payload = {version: 2, recording: marker, segments: [{start: 2, end: 3, text: 'successor saved transcript'}]};
      files.set(transcriptName(marker), payload);
      card.dataset.recordingIdentity = JSON.stringify(marker);
      ++card.transcribeJobGeneration;
      return {name: transcriptName(marker), payload};
    },
    async begin() { assert.equal(start.disabled, false); await start.handlers.click(); return channels.at(-1); },
    handoff() { listeners.get('message')?.({source: popup, origin: 'https://okuflow.com', data: {type: 'okuflow-transcribe-ready', version: 1}}); },
    done(channel, text, recording = identity) { return channel.port1.onmessage({data: {
      type: 'done', model: 'fixture-model', device: 'cpu', language: 'en',
      recording,
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

test('same-named folders and media cannot substitute different recording bytes', async () => {
  const h = await harness({wrongBytes: true});
  assert.equal(await h.begin(), undefined);
  assert.equal(h.sent.some(message => message.type === 'okuflow-transcribe-request'), false);
  assert.match(h.status(), /Choose the OkuFlow folder/);
});

test('an independent same-byte recording must also have the exact media UUID', async () => {
  const h = await harness({wrongId: true});
  assert.equal(await h.begin(), undefined);
  assert.equal(h.creates, 0);
});

test('handoff and sidecar retain the exact native recording identity', async () => {
  const h = await harness({holdWrite: false});
  const channel = await h.begin(); h.handoff();
  assert.deepEqual(JSON.parse(JSON.stringify(h.sent.find(message => message.type === 'okuflow-transcribe-request').recording)), h.identity);
  await h.done(channel, 'correct text');
  assert.equal(h.writes[0].version, 2);
  assert.deepEqual(h.writes[0].recording, h.identity);
  const restored = await harness({saved: h.writes[0]});
  assert.equal(restored.card.segments[0]?.text, 'correct text');
  const wrong = await harness({saved: {...h.writes[0], recording: {...h.identity, mediaId: '87654321-1234-1234-1234-123456789abc'}}});
  assert.equal(wrong.card.segments.length, 0);
});

test('a result for another media identity cannot publish or save text', async () => {
  const h = await harness({holdWrite: false});
  const channel = await h.begin();
  await h.done(channel, 'wrong text', {...h.identity, fingerprint: 'a'.repeat(64)});
  assert.equal(h.card.segments.length, 0);
  assert.equal(h.creates, 0);
});

test('metadata-preserving replacement before sidecar save is refused', async () => {
  const h = await harness({holdWrite: false});
  const channel = await h.begin(); h.replaceBytes();
  await h.done(channel, 'completed text');
  assert.equal(h.creates, 0);
  assert.equal(h.card.segments[0].text, 'completed text');
  assert.match(h.status(), /Choose the OkuFlow folder/);
});

test('large recording verification reads at most one MiB per slice', async () => {
  const h = await harness({bytes: Buffer.alloc(3 * 1024 * 1024 + 19, 21)});
  assert(await h.begin());
  assert.equal(h.maxRead, 1024 * 1024);
});

test('legacy size/time sidecars and wrong-identity sidecars do not restore', async () => {
  for (const recording of [{name: 'video.mp4', size: 17, lastModified: 1234},
    {version: 1, mediaId: '12345678-1234-1234-1234-123456789abc', name: 'video.mp4', size: 17,
      algorithm: 'sha256-chain-1m-v1', fingerprint: 'a'.repeat(64)}]) {
    const h = await harness({saved: {version: 2, recording, segments: [{start: 0, end: 1, text: 'wrong restore'}]}});
    assert.equal(h.card.segments.length, 0);
  }
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

test('an old writable cannot overwrite a successor recording transcript after an await', async () => {
  const h = await harness({writeBarrier: 'writable'});
  const channel = await h.begin();
  const saving = h.done(channel, 'old completed transcript');
  await h.entered.promise; // Verification passed and the old createWritable is now held.
  const successor = h.installSuccessor(); // Same path/name; new bytes, marker UUID and saved transcript.
  h.release.resolve();
  await saving;
  assert.deepEqual(h.files.get(successor.name), successor.payload);
  assert.equal(h.files.get(transcriptName(h.identity)).segments[0].text, 'old completed transcript');
  assert.notEqual(transcriptName(h.identity), successor.name);
  assert.equal(successor.name.length, 61, 'Sidecar filename stays bounded independently of the media basename.');
});

test('legacy basename-only sidecars are never trusted or migrated automatically', async () => {
  const h = await harness({legacySaved: {version: 1, recording: {name: 'video.mp4', size: 17, lastModified: 1234},
    segments: [{start: 0, end: 1, text: 'legacy text'}]}});
  assert.equal(h.card.segments.length, 0);
  assert.equal(h.reads.includes('video.mp4.okuflow-transcript.json'), false);
  assert.equal(h.files.get('video.mp4.okuflow-transcript.json').segments[0].text, 'legacy text');
});
