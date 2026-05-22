// shim_renderer.js — runs in each Discord renderer process as a preload script.
//
// Constructive purpose: client-side premium-tier override for screen-capture quality presets.
// Discord's renderer reads the local user's premiumType to decide which resolution / framerate /
// bitrate options are presented in the stream-quality picker. This shim overrides the locally
// reported tier so the picker presents the full set of presets.
//
// Strategy:
//   1. Install a setter on window.webpackChunkdiscord_app BEFORE Discord populates it.
//   2. When Discord's chunk array materializes, push a synthetic chunk whose entry callback
//      hands us a live reference to Discord's __webpack_require__.
//   3. After each subsequent chunk push, walk the module factory map and look for factories
//      whose source contains the premium-tier accessor identifiers. Instantiate the module
//      to get its live exports and replace the accessor in place.
//   4. Two targets:
//        a. UserPremiumStore.getPremiumType  → returns Nitro tier.
//        b. UserStore.getCurrentUser         → returns user object with premiumType overlayed.
//
// Reliability: matches by factory source substring (not pinned module IDs), so Webpack
// reshuffles between Discord releases don't break the patch. Fail-safe wrapped in try/catch.

'use strict';

(function () {
    const PREMIUM_TIER_NITRO = 2;

    // -------- Logging (best-effort; renderer has nodeIntegration=true via shim_main) --------
    let fs, path, logPath;
    try {
        fs = require('fs');
        path = require('path');
        const lad = process.env && process.env.LOCALAPPDATA;
        if (lad) logPath = path.join(lad, 'dnp', 'log.txt');
    } catch (_) { /* fs unavailable */ }

    function log(msg) {
        if (!fs || !logPath) return;
        try {
            fs.appendFileSync(logPath, `[shim_renderer] ${new Date().toISOString()} ${msg}\n`);
        } catch (_) { /* swallow */ }
    }

    // -------- Webpack handle + patch state --------
    let wpRequire     = null;
    let patchedPremium = false;
    let patchedUser    = false;
    const seenIds = new Set();

    // Probe a module factory for the two target accessors.
    function probe(id, src) {
        if (typeof src !== 'string') return;

        const wantPremium = !patchedPremium && /getPremiumType/.test(src);
        const wantUser    = !patchedUser    && /getCurrentUser/.test(src) && /premiumType/.test(src);
        if (!wantPremium && !wantUser) return;

        let mod;
        try { mod = wpRequire(id); } catch (e) { return; }
        if (!mod) return;

        // Webpack export shapes vary across builds.
        const candidates = [
            mod,
            mod.default,
            mod.Z, mod.ZP,
            mod.exports, mod.exports && mod.exports.default,
        ];

        for (const c of candidates) {
            if (!c) continue;

            if (wantPremium && !patchedPremium && typeof c.getPremiumType === 'function') {
                try {
                    c.getPremiumType = function (_userId) { return PREMIUM_TIER_NITRO; };
                    patchedPremium = true;
                    log('getPremiumType override installed via module ' + id);
                } catch (e) { log('premium install err: ' + (e && e.message)); }
            }

            if (wantUser && !patchedUser && typeof c.getCurrentUser === 'function') {
                try {
                    const orig = c.getCurrentUser.bind(c);
                    c.getCurrentUser = function () {
                        const u = orig.apply(null, arguments);
                        if (!u) return u;
                        if (u.premiumType >= PREMIUM_TIER_NITRO) return u;
                        const proto = Object.getPrototypeOf(u);
                        const clone = Object.assign(Object.create(proto || null), u);
                        clone.premiumType = PREMIUM_TIER_NITRO;
                        return clone;
                    };
                    patchedUser = true;
                    log('getCurrentUser overlay installed via module ' + id);
                } catch (e) { log('user install err: ' + (e && e.message)); }
            }

            if (patchedPremium && patchedUser) return;
        }
    }

    function sweep() {
        if (!wpRequire) return;
        if (patchedPremium && patchedUser) return;
        const factories = wpRequire.m;
        if (!factories) return;
        for (const id in factories) {
            if (seenIds.has(id)) continue;
            const fn = factories[id];
            if (typeof fn !== 'function') continue;
            seenIds.add(id);
            let src;
            try { src = fn.toString(); } catch (_) { continue; }
            probe(id, src);
            if (patchedPremium && patchedUser) return;
        }
    }

    // -------- Hook the Webpack chunk array --------
    function attach(arr) {
        if (!arr || typeof arr.push !== 'function') return;
        try {
            const tag = 'dnp_capture_' + Math.random().toString(36).slice(2);
            arr.push([
                [tag],
                {},
                function (req) {
                    wpRequire = req;
                    try { sweep(); } catch (e) { log('initial sweep err: ' + (e && e.message)); }
                }
            ]);
        } catch (e) {
            log('synthetic chunk push failed: ' + (e && e.message));
        }

        const realPush = arr.push.bind(arr);
        arr.push = function (chunk) {
            const r = realPush(chunk);
            try { sweep(); } catch (e) { log('sweep err: ' + (e && e.message)); }
            return r;
        };
        log('chunk hook attached');
    }

    function install() {
        try {
            const existing = window.webpackChunkdiscord_app;
            if (Array.isArray(existing)) {
                attach(existing);
                return;
            }
        } catch (_) { /* not yet defined */ }

        Object.defineProperty(window, 'webpackChunkdiscord_app', {
            configurable: true,
            set(v) {
                try {
                    Object.defineProperty(window, 'webpackChunkdiscord_app', {
                        configurable: true,
                        writable: true,
                        value: v,
                    });
                } catch (_) { /* fallback */ }
                attach(v);
            }
        });
    }

    try { install(); } catch (e) { log('install failed: ' + (e && e.message)); }
    log('renderer shim loaded');
})();
