const CACHE_NAME = 'secure-app-shell-v1';
const APP_SHELL = [
  '/',
  '/manifest.webmanifest',
  '/pwa.js',
  '/favicon.png',
  '/apple-touch-icon.png',
  '/secure-logo.png',
  '/secure-icon-512.png'
];
const STATIC_ASSETS = new Set(APP_SHELL.slice(1));

self.addEventListener('install', event => {
  event.waitUntil(
    caches.open(CACHE_NAME)
      .then(cache => cache.addAll(APP_SHELL))
      .then(() => self.skipWaiting())
  );
});

self.addEventListener('activate', event => {
  event.waitUntil(
    caches.keys()
      .then(keys => Promise.all(keys
        .filter(key => key.startsWith('secure-app-shell-') && key !== CACHE_NAME)
        .map(key => caches.delete(key))))
      .then(() => self.clients.claim())
  );
});

self.addEventListener('fetch', event => {
  const request = event.request;
  const url = new URL(request.url);

  if (request.method !== 'GET' || url.origin !== self.location.origin || url.pathname.startsWith('/api/')) {
    return;
  }

  if (request.mode === 'navigate') {
    event.respondWith(
      fetch(request)
        .then(response => {
          if (response.ok) return response;
          return caches.match(request).then(cached => cached || caches.match('/')).then(cached => cached || response);
        })
        .then(response => {
          if (!response.ok) return response;
          return caches.open(CACHE_NAME)
            .then(cache => cache.put('/', response.clone()))
            .then(() => response)
            .catch(error => {
              console.error('Could not update the offline app shell.', error);
              return response;
            });
        })
        .catch(() => caches.match(request).then(cached => cached || caches.match('/')))
    );
    return;
  }

  if (STATIC_ASSETS.has(url.pathname)) {
    event.respondWith(
      caches.match(url.pathname).then(cached => cached || fetch(request).then(response => {
        if (response.ok) {
          return caches.open(CACHE_NAME).then(cache => {
            cache.put(url.pathname, response.clone());
            return response;
          });
        }
        return response;
      }))
    );
  }
});
