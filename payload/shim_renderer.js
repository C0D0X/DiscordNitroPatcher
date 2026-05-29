// renderer preload for nitro patch

'use strict';

(function () {
    const PREMIUM_TIER_NITRO = 2;

    // logging
    let fs, path, logPath;
    try {
        fs = require('fs');
        path = require('path');
        const lad = process.env && process.env.LOCALAPPDATA;
        if (lad) logPath = path.join(lad, 'dnp', 'log.txt');
    } catch (_) { }

    function log(msg) {
        if (!fs || !logPath) return;
        try {
            fs.appendFileSync(logPath, `[shim_renderer] ${new Date().toISOString()} ${msg}\n`);
        } catch (_) { }
    }

    // state
    let wpRequire     = null;
    let patchedPremium = false;
    let patchedUser    = false;
    const seenIds = new Set();

    // check module for patch targets
    function probe(id, src) {
        if (typeof src !== 'string') return;

        const wantPremium = !patchedPremium && /getPremiumType/.test(src);
        const wantUser    = !patchedUser    && /getCurrentUser/.test(src) && /premiumType/.test(src);
        if (!wantPremium && !wantUser) return;

        let mod;
        try { mod = wpRequire(id); } catch (e) { return; }
        if (!mod) return;

        // check different export patterns
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

    // hook chunks
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
        } catch (_) { }

        Object.defineProperty(window, 'webpackChunkdiscord_app', {
            configurable: true,
            set(v) {
                try {
                    Object.defineProperty(window, 'webpackChunkdiscord_app', {
                        configurable: true,
                        writable: true,
                        value: v,
                    });
                } catch (_) { }
                attach(v);
            }
        });
    }

    try { install(); } catch (e) { log('install failed: ' + (e && e.message)); }
    log('renderer shim loaded');
})();
