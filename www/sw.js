/*
 * sw.js — TransitTag Service Worker
 * Cache-first for static PWA assets, network-first for everything else.
 * WebSocket connections are never intercepted.
 */

var CACHE_NAME = "transittag-v1";

/* Assets to pre-cache on install */
var PRE_CACHE = [
    "/pwa/",
    "/pwa/index.html",
    "/pwa/pwa.css",
    "/pwa/pwa.js"
];

/* ── Install ────────────────────────────────────────────────────── */

self.addEventListener("install", function (evt) {
    console.log("[SW] Installing transittag-v1");
    evt.waitUntil(
        caches.open(CACHE_NAME).then(function (cache) {
            return cache.addAll(PRE_CACHE);
        }).then(function () {
            /* Activate immediately without waiting for old tabs to close */
            return self.skipWaiting();
        })
    );
});

/* ── Activate ───────────────────────────────────────────────────── */

self.addEventListener("activate", function (evt) {
    console.log("[SW] Activating transittag-v1");
    evt.waitUntil(
        caches.keys().then(function (keys) {
            return Promise.all(
                keys.filter(function (k) {
                    /* Delete any old cache versions */
                    return k !== CACHE_NAME;
                }).map(function (k) {
                    console.log("[SW] Deleting old cache:", k);
                    return caches.delete(k);
                })
            );
        }).then(function () {
            /* Take control of all open pages immediately */
            return self.clients.claim();
        })
    );
});

/* ── Fetch ──────────────────────────────────────────────────────── */

self.addEventListener("fetch", function (evt) {
    var req = evt.request;
    var url = new URL(req.url);

    /* Never intercept WebSocket upgrades or non-GET requests */
    if (req.method !== "GET") return;
    if (url.protocol === "ws:" || url.protocol === "wss:") return;

    /* Never cache API / WebSocket endpoint */
    if (url.pathname === "/ws" || url.pathname.startsWith("/ws/")) return;

    /* Cache-first strategy for known PWA assets */
    if (isCacheable(url.pathname)) {
        evt.respondWith(cacheFirst(req));
        return;
    }

    /* Network-first for everything else (dashboard, other pages) */
    evt.respondWith(networkFirst(req));
});

/* ── Strategies ─────────────────────────────────────────────────── */

function cacheFirst(req) {
    return caches.match(req).then(function (cached) {
        if (cached) return cached;
        /* Not in cache — fetch and store */
        return fetch(req).then(function (resp) {
            if (resp && resp.status === 200) {
                var clone = resp.clone();
                caches.open(CACHE_NAME).then(function (c) {
                    c.put(req, clone);
                });
            }
            return resp;
        }).catch(function () {
            /* Offline and not cached — nothing we can do */
            return new Response("Offline", { status: 503 });
        });
    });
}

function networkFirst(req) {
    return fetch(req).then(function (resp) {
        if (resp && resp.status === 200) {
            var clone = resp.clone();
            caches.open(CACHE_NAME).then(function (c) {
                c.put(req, clone);
            });
        }
        return resp;
    }).catch(function () {
        /* Network failed — try cache */
        return caches.match(req).then(function (cached) {
            return cached || new Response("Offline", { status: 503 });
        });
    });
}

/* ── Helpers ────────────────────────────────────────────────────── */

function isCacheable(pathname) {
    /* Only cache PWA shell files */
    return pathname === "/pwa/" ||
           pathname === "/pwa/index.html" ||
           pathname === "/pwa/pwa.css" ||
           pathname === "/pwa/pwa.js" ||
           pathname === "/manifest.json";
}
