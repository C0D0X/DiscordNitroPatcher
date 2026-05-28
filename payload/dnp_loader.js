// dnp_loader.js — self-contained main-process shim embedded inside Discord's app.asar.
//
// Constructive purpose: client-side override of the locally-reported premium tier and the
// screen-capture quality manager parameters so the local user's stream-quality picker presents
// the full set of presets (1080p60 / 1440p60 / 4K60) and so the local capture pipeline allows
// higher bitrate/resolution/framerate values that Discord's own client would otherwise clamp.
//
// Architecture:
//   1. This file runs as the main-process entry point because dnp's patcher set package.json
//      "main": "./dnp_loader.js" (the original main was renamed to ./app_original_main.js).
//   2. We override electron.BrowserWindow so every renderer window receives our preload script.
//      The preload source is embedded below as a string literal and written to disk on each boot.
//   3. The preload, running in the renderer with nodeIntegration enabled, hooks Discord's
//      Webpack chunk array, walks the module factory map, and applies in-place patches to
//      UserStore / UserProfileStore / video quality / stream-settings validator.
//   4. Finally we require ./app_original_main.js so Discord boots normally.
//
// Reliability: all paths derive from os.homedir() — no reliance on env vars that can be stripped.
// All operations are wrapped so any failure leaves Discord booting unpatched, never broken.

'use strict';

(function () {
    const path = require('path');
    const fs   = require('fs');
    const os   = require('os');

    // Derive %LOCALAPPDATA%\dnp paths from homedir to avoid env-var reliance.
    const LOCAL_APP_DATA = path.join(os.homedir(), 'AppData', 'Local');
    const DNP_DIR        = path.join(LOCAL_APP_DATA, 'dnp');
    const LOG_FILE       = path.join(DNP_DIR, 'log.txt');
    const RENDERER_PATH  = path.join(DNP_DIR, 'shim_renderer.js');

    function diag(prefix, msg) {
        try {
            if (!fs.existsSync(DNP_DIR)) fs.mkdirSync(DNP_DIR, { recursive: true });
            fs.appendFileSync(
                LOG_FILE,
                `${new Date().toISOString()} [${prefix}] ${msg}\n`
            );
        } catch (_) { /* swallow — never let logging break boot */ }
    }

    diag('loader', 'main-process loader executing, pid=' + process.pid);

    // -------- Renderer source (embedded string literal) --------
    // Written to disk on each main-process boot so BrowserWindow.preload can reference it.
    const RENDERER_SOURCE = `
// shim_renderer.js — runs as preload in each Discord renderer.
'use strict';
(function () {
    const PREMIUM_TIER_NITRO = 2;

    let fs, path, os, logPath;
    try {
        fs = require('fs');
        path = require('path');
        os = require('os');
        logPath = path.join(os.homedir(), 'AppData', 'Local', 'dnp', 'log.txt');
    } catch (_) {}

    function log(msg) {
        if (!fs || !logPath) return;
        try {
            fs.appendFileSync(logPath, new Date().toISOString() + ' [renderer] ' + msg + '\\n');
        } catch (_) {}
    }

    log('preload started');

    let wpRequire = null;
    const seen = new Set();
    const state = {
        userStore: null,
        userProfileStore: null,
        patchedVideoQuality: false,
        patchedStreamSettings: false,
        patchedMaxFileSize: false,
    };

    // Generic Webpack module finder.
    // Walks the factory map (wpRequire.m), filters by source-code substrings, then instantiates
    // the matching factory and probes each common export shape for the required own/prototype keys.
    function findExport(codeNeedles, candidatePicker) {
        if (!wpRequire || !wpRequire.m) return null;
        const factories = wpRequire.m;
        for (const id in factories) {
            const fn = factories[id];
            if (typeof fn !== 'function') continue;
            let src;
            try { src = fn.toString(); } catch (_) { continue; }
            if (!codeNeedles.every(n => src.includes(n))) continue;
            try {
                const ex = wpRequire(id);
                const cands = [ex, ex && ex.default, ex && ex.Z, ex && ex.ZP, ex && ex.exports];
                for (const c of cands) {
                    if (!c) continue;
                    const hit = candidatePicker(c);
                    if (hit) return hit;
                }
            } catch (_) { /* instantiation failed; skip */ }
        }
        return null;
    }

    // -------- Patches --------
    function patchUserStore() {
        if (state.userStore) return;
        const store = findExport(
            ['getCurrentUser', 'getUser'],
            c => (typeof c.getCurrentUser === 'function' && typeof c.getUser === 'function') ? c : null
        );
        if (!store) return;
        const origGCU = store.getCurrentUser.bind(store);
        store.getCurrentUser = function () {
            const u = origGCU.apply(null, arguments);
            if (u && u.premiumType !== PREMIUM_TIER_NITRO) {
                try { u.premiumType = PREMIUM_TIER_NITRO; } catch (_) {}
            }
            return u;
        };
        const origGU = store.getUser.bind(store);
        store.getUser = function () {
            const u = origGU.apply(null, arguments);
            if (u && u.id && state.currentUserId === u.id && u.premiumType !== PREMIUM_TIER_NITRO) {
                try { u.premiumType = PREMIUM_TIER_NITRO; } catch (_) {}
            }
            return u;
        };
        // Mutate cached current user immediately so any synchronous reads see Nitro tier.
        try {
            const cur = store.getCurrentUser();
            if (cur) { state.currentUserId = cur.id; cur.premiumType = PREMIUM_TIER_NITRO; }
        } catch (_) {}
        state.userStore = store;
        log('UserStore patched');
    }

    function patchUserProfileStore() {
        if (state.userProfileStore) return;
        const store = findExport(
            ['getUserProfile'],
            c => (typeof c.getUserProfile === 'function') ? c : null
        );
        if (!store) return;
        const origGUP = store.getUserProfile.bind(store);
        store.getUserProfile = function () {
            const p = origGUP.apply(null, arguments);
            if (p && p.premiumType !== PREMIUM_TIER_NITRO) {
                try { p.premiumType = PREMIUM_TIER_NITRO; } catch (_) {}
            }
            return p;
        };
        state.userProfileStore = store;
        log('UserProfileStore patched');
    }

    function patchVideoQuality() {
        if (state.patchedVideoQuality) return;
        // YABDP4Nitro targets the prototype.updateVideoQuality method on the videoOptionFunctions
        // class. We find it by walking factories whose source contains 'updateVideoQuality'.
        const cls = findExport(
            ['updateVideoQuality'],
            c => (c && c.prototype && typeof c.prototype.updateVideoQuality === 'function') ? c : null
        );
        if (!cls) return;
        const orig = cls.prototype.updateVideoQuality;
        cls.prototype.updateVideoQuality = function () {
            try {
                const e = this;
                if (e && e.videoQualityManager && e.videoQualityManager.options) {
                    const opts = e.videoQualityManager.options;
                    // Floor / target / max (kbps -> bps conversion handled by raising directly).
                    const MIN_BPS    = 500000;     // 500 kbps floor
                    const TARGET_BPS = 8000000;    // 8 Mbps target
                    const MAX_BPS    = 25000000;   // 25 Mbps max — matches Nitro Source preset
                    opts.videoBitrateFloor = MIN_BPS;
                    if (opts.videoBitrate)   { opts.videoBitrate.min   = MIN_BPS; opts.videoBitrate.max   = MAX_BPS; }
                    if (opts.desktopBitrate) { opts.desktopBitrate.min = MIN_BPS;
                                               opts.desktopBitrate.target = TARGET_BPS;
                                               opts.desktopBitrate.max = MAX_BPS; }
                    if (e.videoQualityManager.goliveMaxQuality) {
                        e.videoQualityManager.goliveMaxQuality.bitrateMax = MAX_BPS;
                    }
                }
                // Lift framerate cap to whatever stream parameters allow.
                if (e && e.videoStreamParameters && e.videoStreamParameters[0]) {
                    const sp = e.videoStreamParameters[0];
                    if (sp.maxFrameRate && e.videoQualityManager && e.videoQualityManager.options) {
                        const fps = sp.maxFrameRate;
                        if (e.videoQualityManager.options.videoBudget)
                            e.videoQualityManager.options.videoBudget.framerate = fps;
                        if (e.videoQualityManager.options.videoCapture)
                            e.videoQualityManager.options.videoCapture.framerate = fps;
                    }
                    // Resolution: pin to maxResolution from stream params (Nitro tier ceiling).
                    if (sp.maxResolution && e.videoQualityManager) {
                        const w = sp.maxResolution.width  || 2560;
                        const h = sp.maxResolution.height || 1440;
                        const fps = sp.maxFrameRate || 60;
                        const vq = { width: w, height: h, framerate: fps };
                        e.remoteSinkWantsMaxFramerate = fps;
                        e.videoQualityManager.options.videoBudget  = vq;
                        e.videoQualityManager.options.videoCapture = vq;
                        // Quality ladder recomputation requires LadderModule — best-effort.
                        try {
                            const pxBudget = w * h;
                            if (e.videoQualityManager.ladder) {
                                e.videoQualityManager.ladder.pixelBudget = pxBudget;
                            }
                        } catch (_) {}
                    }
                }
            } catch (err) { log('updateVideoQuality patch err: ' + (err && err.message)); }
            return orig.apply(this, arguments);
        };
        state.patchedVideoQuality = true;
        log('updateVideoQuality wrapped');
    }

    function patchStreamSettingsValidator() {
        if (state.patchedStreamSettings) return;
        // Discord's InvalidStreamSettingsModal exports an areStreamSettingsAllowed function
        // (often a mangled name). YABDP4Nitro identifies it by the source needles below.
        const mod = findExport(
            ['preset)&&', 'resolution&&', 'fps&&'],
            c => c
        );
        if (!mod) return;
        // Find any method on the module that takes settings and returns boolean — replace with const true.
        for (const key in mod) {
            if (typeof mod[key] !== 'function') continue;
            try {
                const src = mod[key].toString();
                if (/resolution\s*&&|fps\s*&&|preset\s*&&/.test(src)) {
                    mod[key] = function () { return true; };
                    state.patchedStreamSettings = true;
                    log('areStreamSettingsAllowed bypassed (' + key + ')');
                    break;
                }
            } catch (_) {}
        }
    }

    function patchMaxFileSize() {
        if (state.patchedMaxFileSize) return;
        const mod = findExport(
            ['getUserMaxFileSize'],
            c => c
        );
        if (!mod) return;
        for (const key in mod) {
            if (typeof mod[key] !== 'function') continue;
            try {
                const src = mod[key].toString();
                if (src.includes('getUserMaxFileSize')) {
                    mod[key] = function () { return 500 * 1024 * 1024; };
                    state.patchedMaxFileSize = true;
                    log('getMaxFileSize bumped to 500MB');
                    break;
                }
            } catch (_) {}
        }
    }

    function sweep() {
        try { patchUserStore(); }              catch (e) { log('userstore err: ' + e.message); }
        try { patchUserProfileStore(); }       catch (e) { log('profilestore err: ' + e.message); }
        try { patchVideoQuality(); }           catch (e) { log('videoquality err: ' + e.message); }
        try { patchStreamSettingsValidator(); }catch (e) { log('streamsettings err: ' + e.message); }
        try { patchMaxFileSize(); }            catch (e) { log('maxfilesize err: ' + e.message); }
    }

    // -------- Webpack hook --------
    function attach(arr) {
        if (!arr || typeof arr.push !== 'function') return;
        // Inject a synthetic chunk so we capture a reference to Discord's __webpack_require__.
        try {
            const tag = 'dnp_capture_' + Math.random().toString(36).slice(2);
            arr.push([
                [tag],
                {},
                function (req) {
                    wpRequire = req;
                    log('captured wpRequire, factories=' + (req && req.m ? Object.keys(req.m).length : 0));
                    sweep();
                }
            ]);
        } catch (e) { log('synthetic push err: ' + e.message); }

        const realPush = arr.push.bind(arr);
        arr.push = function (chunk) {
            const r = realPush(chunk);
            try { sweep(); } catch (_) {}
            return r;
        };
        log('chunk array hook attached');
    }

    function install() {
        try {
            const existing = window.webpackChunkdiscord_app;
            if (Array.isArray(existing)) {
                attach(existing);
                return;
            }
        } catch (_) {}

        Object.defineProperty(window, 'webpackChunkdiscord_app', {
            configurable: true,
            set(v) {
                try {
                    Object.defineProperty(window, 'webpackChunkdiscord_app', {
                        configurable: true, writable: true, value: v
                    });
                } catch (_) {}
                attach(v);
            }
        });
        log('webpackChunkdiscord_app setter installed');
    }

    try { install(); } catch (e) { log('install err: ' + (e && e.message)); }

    // Periodic re-sweep — catches modules that load after initial chunk burst.
    let ticks = 0;
    const iv = setInterval(function () {
        ticks++;
        try { sweep(); } catch (_) {}
        if (ticks > 60 || (state.userStore && state.patchedVideoQuality && state.patchedStreamSettings)) {
            clearInterval(iv);
            log('sweep loop done, ticks=' + ticks);
        }
    }, 500);
})();
`;

    // -------- Main process setup --------
    try {
        if (!fs.existsSync(DNP_DIR)) fs.mkdirSync(DNP_DIR, { recursive: true });
        fs.writeFileSync(RENDERER_PATH, RENDERER_SOURCE);
        diag('loader', 'renderer source written to ' + RENDERER_PATH);
    } catch (e) {
        diag('loader', 'failed to write renderer: ' + (e && e.stack || e));
    }

    try {
        const electron = require('electron');
        diag('loader', 'electron require ok, BrowserWindow type=' + typeof electron.BrowserWindow);

        const OrigBrowserWindow = electron.BrowserWindow;

        // Wrap webPreferences to inject our preload + relax isolation enough for the renderer
        // to access window.webpackChunkdiscord_app and use node fs for diagnostics.
        function patchOpts(opts) {
            opts = opts || {};
            opts.webPreferences = opts.webPreferences || {};
            const userPreload = opts.webPreferences.preload;
            if (userPreload && userPreload !== RENDERER_PATH) {
                try {
                    const chain = path.join(DNP_DIR, `chain_${process.pid}.js`);
                    fs.writeFileSync(chain,
                        `try { require(${JSON.stringify(userPreload)}); } catch (e) {}\n` +
                        `try { require(${JSON.stringify(RENDERER_PATH)}); } catch (e) {}\n`);
                    opts.webPreferences.preload = chain;
                } catch (_) {
                    opts.webPreferences.preload = RENDERER_PATH;
                }
            } else {
                opts.webPreferences.preload = RENDERER_PATH;
            }
            opts.webPreferences.sandbox          = false;
            opts.webPreferences.contextIsolation = false;
            opts.webPreferences.nodeIntegration  = true;
            return opts;
        }

        // PatchedBrowserWindow is a constructor wrapper that mutates webPreferences before super().
        // Using `extends` keeps prototype chain identity intact so Discord's instanceof checks pass.
        class PatchedBrowserWindow extends OrigBrowserWindow {
            constructor(opts) {
                diag('loader', 'PatchedBrowserWindow instantiated');
                super(patchOpts(opts));
            }
        }
        // Preserve static members (BrowserWindow.fromWebContents, BrowserWindow.getAllWindows, ...).
        for (const k of Object.getOwnPropertyNames(OrigBrowserWindow)) {
            if (k === 'length' || k === 'name' || k === 'prototype') continue;
            try {
                Object.defineProperty(PatchedBrowserWindow, k,
                    Object.getOwnPropertyDescriptor(OrigBrowserWindow, k));
            } catch (_) {}
        }

        // Electron 37 marks electron.BrowserWindow non-configurable, so Object.defineProperty
        // on the exports object fails. Instead intercept Module._load: when downstream code
        // requires 'electron', return a Proxy that yields PatchedBrowserWindow for the
        // BrowserWindow property and forwards everything else to the real module.
        try {
            const Module = require('module');
            const electronProxy = new Proxy(electron, {
                get(target, prop, receiver) {
                    if (prop === 'BrowserWindow') return PatchedBrowserWindow;
                    return Reflect.get(target, prop, receiver);
                }
            });
            const origLoad = Module._load;
            let inHook = false;
            Module._load = function (request, parent, isMain) {
                const result = origLoad.apply(this, arguments);
                if (inHook) return result;
                if (request === 'electron' && result && result.BrowserWindow === OrigBrowserWindow) {
                    inHook = true;
                    try {
                        return electronProxy;
                    } finally {
                        inHook = false;
                    }
                }
                return result;
            };
            diag('loader', 'Module._load proxy hook installed for electron');
        } catch (e) {
            diag('loader', 'Module._load hook err: ' + (e && e.message));
        }

        // Defense in depth: catch any window that slips past the proxy (e.g. created via direct
        // reference Discord already destructured before our hook ran).
        try {
            electron.app.on('browser-window-created', (_event, win) => {
                diag('loader', 'browser-window-created event fired');
                try {
                    const wc = win.webContents;
                    if (!wc) return;
                    // Best-effort: inject the renderer source via executeJavaScript at the
                    // earliest possible renderer event. The preload path above is the
                    // primary mechanism; this is a fallback for unhooked windows.
                    wc.on('dom-ready', () => {
                        wc.executeJavaScript(RENDERER_SOURCE).catch(err => {
                            diag('loader', 'executeJavaScript fallback err: ' + (err && err.message));
                        });
                    });
                } catch (e) { diag('loader', 'window event setup err: ' + (e && e.message)); }
            });
        } catch (e) {
            diag('loader', 'app event subscribe err: ' + (e && e.message));
        }
    } catch (e) {
        diag('loader', 'electron setup outer err: ' + (e && e.stack || e));
    }

    // Always hand off to Discord's original main, regardless of any failure above.
    try {
        require('./app_original_main.js');
    } catch (e) {
        diag('loader', 'FATAL: app_original_main require failed: ' + (e && e.stack || e));
        throw e;
    }
})();
