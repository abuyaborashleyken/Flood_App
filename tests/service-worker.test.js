import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';
import { runInNewContext } from 'node:vm';
import test from 'node:test';

const workerSource = await readFile(new URL('../public/service-worker.js', import.meta.url), 'utf8');

function createWorker(fetchImplementation = async () => ({ ok: true, source: 'network' })) {
  const listeners = new Map();
  const cached = new Map();
  const calls = [];
  const normalizeKey = request => typeof request === 'string'
    ? new URL(request, 'https://secure-app.test').pathname
    : new URL(request.url).pathname;
  const cache = {
    addAll: async paths => {
      paths.forEach(path => cached.set(normalizeKey(path), { ok: true, source: `cached:${path}` }));
    },
    match: async request => cached.get(normalizeKey(request)),
    put: async (request, response) => cached.set(normalizeKey(request), response)
  };
  const context = {
    URL,
    Promise,
    fetch: (...args) => {
      calls.push(args[0]);
      return fetchImplementation(...args);
    },
    caches: {
      open: async () => cache,
      match: request => cache.match(request),
      keys: async () => ['secure-app-shell-old', 'unrelated-cache'],
      delete: async key => key === 'secure-app-shell-old'
    },
    self: {
      location: { origin: 'https://secure-app.test' },
      clients: { claim: async () => {} },
      skipWaiting: async () => {},
      addEventListener: (name, listener) => listeners.set(name, listener)
    }
  };
  runInNewContext(workerSource, context);

  return {
    cached,
    calls,
    listeners,
    async dispatch(name, event) {
      const listener = listeners.get(name);
      assert.ok(listener, `Expected a ${name} listener`);
      listener(event);
      if (event.pending) await event.pending;
    }
  };
}

test('pre-caches the app shell for installation', async () => {
  const worker = createWorker();
  const event = { waitUntil(promise) { this.pending = promise; } };

  await worker.dispatch('install', event);

  for (const asset of ['/', '/manifest.webmanifest', '/pwa.js', '/favicon.png', '/secure-icon-512.png']) {
    assert.ok(worker.cached.has(asset), `${asset} should be cached`);
  }
});

test('serves the cached app shell when a page navigation is offline', async () => {
  const worker = createWorker(async () => { throw new Error('offline'); });
  await worker.dispatch('install', { waitUntil(promise) { this.pending = promise; } });
  const event = {
    request: { url: 'https://secure-app.test/', method: 'GET', mode: 'navigate' },
    respondWith(promise) { this.response = promise; }
  };

  await worker.dispatch('fetch', event);

  assert.equal((await event.response).source, 'cached:/');
});

test('refreshes the cached app shell from a successful navigation', async () => {
  const worker = createWorker(async () => ({ ok: true, source: 'updated shell', clone() { return this; } }));
  const event = {
    request: { url: 'https://secure-app.test/', method: 'GET', mode: 'navigate' },
    respondWith(promise) { this.response = promise; }
  };

  await worker.dispatch('fetch', event);

  assert.equal((await event.response).source, 'updated shell');
  assert.equal(worker.cached.get('/').source, 'updated shell');
});

test('always sends API requests to the network without caching their responses', async () => {
  const worker = createWorker(async () => ({ ok: true, source: 'live API' }));
  const event = {
    request: { url: 'https://secure-app.test/api/status', method: 'GET', mode: 'cors' },
    respondWith() { assert.fail('API requests must not be intercepted'); }
  };

  await worker.dispatch('fetch', event);

  assert.equal(worker.calls.length, 0);
  assert.equal(worker.cached.has('/api/status'), false);
});

test('uses the cached logo before making a network request', async () => {
  const worker = createWorker(async () => { throw new Error('unexpected network request'); });
  await worker.dispatch('install', { waitUntil(promise) { this.pending = promise; } });
  const event = {
    request: { url: 'https://secure-app.test/secure-logo.png', method: 'GET', mode: 'no-cors' },
    respondWith(promise) { this.response = promise; }
  };

  await worker.dispatch('fetch', event);

  assert.equal((await event.response).source, 'cached:/secure-logo.png');
  assert.equal(worker.calls.length, 0);
});
