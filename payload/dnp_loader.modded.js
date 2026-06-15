// main loader for dnp - patch discord nitro features

'use strict';

(function () {
    const path   = require('path');
    const fs     = require('fs');
    const os     = require('os');
    const crypto = require('crypto');

    // use homedir for paths, not env vars
    const LOCAL_APP_DATA = path.join(os.homedir(), 'AppData', 'Local');
    const DNP_DIR        = path.join(LOCAL_APP_DATA, 'dnp');
    const LOG_FILE       = path.join(DNP_DIR, 'log.txt');
    const RENDERER_PATH  = path.join(DNP_DIR, 'shim_renderer.js');
    const CFG_FILE       = path.join(DNP_DIR, 'extra.cfg');
    const ADDON_FILE     = path.join(DNP_DIR, 'discord_voice_codec.node');
    const MARKER_FILE    = path.join(DNP_DIR, 'shim_main.loaded');
    const BUILD_STAMP    = 'v2 extra.cfg + auto-discovery';

    function diag(prefix, msg) {
        try {
            if (!fs.existsSync(DNP_DIR)) fs.mkdirSync(DNP_DIR, { recursive: true });
            fs.appendFileSync(
                LOG_FILE,
                `${new Date().toISOString()} [${prefix}] ${msg}\n`
            );
        } catch (_) { /* dont break boot */ }
    }

    diag('loader', 'main-process loader executing, pid=' + process.pid +
                   ' build=' + BUILD_STAMP);

    // Hard execution marker for ground-truth debugging. Drops a tiny file
    // BEFORE we touch electron / asar / anything that can throw. If a user
    // reports "nothing in log folder", presence of this file proves the
    // asar patch is loading us; absence proves it isn't.
    try {
        fs.writeFileSync(MARKER_FILE,
            `${new Date().toISOString()} pid=${process.pid} build=${BUILD_STAMP}\n`);
    } catch (_) { /* tolerated */ }

    // ----------------------------------------------------------------------
    // Stale chain-file sweep.
    //
    // The earlier build wrote chain_<pid>.js on every BrowserWindow
    // construction and never cleaned them up; a long Discord session left
    // dozens of files behind. New code names chain files by a hash of
    // the user preload path so the same preload reuses one file. This
    // sweep wipes everything matching the legacy `chain_<digits>.js`
    // shape plus any new-format file untouched in 24h.
    // ----------------------------------------------------------------------
    try {
        const cutoff = Date.now() - (24 * 60 * 60 * 1000);
        const entries = fs.readdirSync(DNP_DIR);
        for (const name of entries) {
            if (!name.startsWith('chain') || !name.endsWith('.js')) continue;
            const legacy = /^chain_\d+(_\d+)?\.js$/.test(name);
            const full   = path.join(DNP_DIR, name);
            try {
                if (legacy) {
                    fs.unlinkSync(full);
                } else {
                    const st = fs.statSync(full);
                    if (st.mtimeMs < cutoff) fs.unlinkSync(full);
                }
            } catch (_) { /* tolerate locks / races */ }
        }
    } catch (e) {
        diag('loader', 'chain sweep failed: ' + (e && e.message));
    }

    // renderer code embedded here
    const RENDERER_SOURCE = `
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

    // public status object for debugging
    try {
        if (!window.__dnp_status) {
            window.__dnp_status = {
                version: 'dnp',
                loaded: true,
                chunkArrayName: null,
                wpRequireCaptured: false,
                factoryCount: 0,
                patches: {
                    userStore: false,
                    userProfileStore: false,
                    videoQuality: false,
                    streamSettings: false,
                    maxFileSize: false,
                },
                lastError: null,
                ticks: 0,
            };
        }
    } catch (_) {}
    function setStatus(field, val) {
        try { if (window.__dnp_status) window.__dnp_status[field] = val; } catch (_) {}
    }
    function setPatch(name, val) {
        try { if (window.__dnp_status && window.__dnp_status.patches) window.__dnp_status.patches[name] = val; } catch (_) {}
    }
    function pushErr(e) {
        try {
            if (!window.__dnp_status) return;
            window.__dnp_status.lastError = String(e && e.message || e);
        } catch (_) {}
    }

    // find webpack modules by code search
    function findExport(codeNeedles, candidatePicker) {
        if (!wpRequire || !wpRequire.m) return null;
        const factories = wpRequire.m;
        for (const id in factories) {
            const fn = factories[id];
            if (typeof fn !== 'function') continue;
            let src;
            try { src = fn.toString(); } catch (_) {}
            if (!codeNeedles.every(n => src.includes(n))) continue;
            try {
                const ex = wpRequire(id);
                const cands = [ex, ex && ex.default, ex && ex.Z, ex && ex.ZP, ex && ex.exports];
                for (const c of cands) {
                    if (!c) continue;
                    const hit = candidatePicker(c);
                    if (hit) return hit;
                }
            } catch (_) {}
        }
        return null;
    }

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
        // patch current user in place
        try {
            const cur = store.getCurrentUser();
            if (cur) { state.currentUserId = cur.id; cur.premiumType = PREMIUM_TIER_NITRO; }
        } catch (_) {}
        state.userStore = store;
        setPatch('userStore', true);
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
        setPatch('userProfileStore', true);
        log('UserProfileStore patched');
    }

    function patchVideoQuality() {
        if (state.patchedVideoQuality) return;
        // find and patch updateVideoQuality
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
                    // set min/target/max bitrate
                    const MIN_BPS    = 500000;     // 500 kbps floor
                    const TARGET_BPS = 8000000;    // 8 Mbps target
                    const MAX_BPS    = 25000000;   // 25 Mbps cap
                    opts.videoBitrateFloor = MIN_BPS;
                    if (opts.videoBitrate)   { opts.videoBitrate.min   = MIN_BPS; opts.videoBitrate.max   = MAX_BPS; }
                    if (opts.desktopBitrate) { opts.desktopBitrate.min = MIN_BPS;
                                               opts.desktopBitrate.target = TARGET_BPS;
                                               opts.desktopBitrate.max = MAX_BPS; }
                    if (e.videoQualityManager.goliveMaxQuality) {
                        e.videoQualityManager.goliveMaxQuality.bitrateMax = MAX_BPS;
                    }
                }
                // uncap framerate
                if (e && e.videoStreamParameters && e.videoStreamParameters[0]) {
                    const sp = e.videoStreamParameters[0];
                    if (sp.maxFrameRate && e.videoQualityManager && e.videoQualityManager.options) {
                        const fps = sp.maxFrameRate;
                        if (e.videoQualityManager.options.videoBudget)
                            e.videoQualityManager.options.videoBudget.framerate = fps;
                        if (e.videoQualityManager.options.videoCapture)
                            e.videoQualityManager.options.videoCapture.framerate = fps;
                    }
                    // set max resolution
                    if (sp.maxResolution && e.videoQualityManager) {
                        const w = sp.maxResolution.width  || 2560;
                        const h = sp.maxResolution.height || 1440;
                        const fps = sp.maxFrameRate || 60;
                        const vq = { width: w, height: h, framerate: fps };
                        e.remoteSinkWantsMaxFramerate = fps;
                        e.videoQualityManager.options.videoBudget  = vq;
                        e.videoQualityManager.options.videoCapture = vq;
                        // update pixel budget if possible
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
        setPatch('videoQuality', true);
        log('updateVideoQuality wrapped');
    }

    function patchStreamSettingsValidator() {
        if (state.patchedStreamSettings) return;
        // find and bypass stream settings validation
        const mod = findExport(
            ['preset)&&', 'resolution&&', 'fps&&'],
            c => c
        );
        if (!mod) return;
        // replace with function that always returns true
        for (const key in mod) {
            if (typeof mod[key] !== 'function') continue;
            try {
                const src = mod[key].toString();
                if (/resolution\s*&&|fps\s*&&|preset\s*&&/.test(src)) {
                    mod[key] = function () { return true; };
                    state.patchedStreamSettings = true;
                    setPatch('streamSettings', true);
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
                    setPatch('maxFileSize', true);
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

    function attach(arr, name) {
        if (!arr || typeof arr.push !== 'function') return;
        if (arr.__dnp_attached) return;
        arr.__dnp_attached = true;
        setStatus('chunkArrayName', name || 'unknown');

        // get webpack require reference
        try {
            const tag = 'dnp_capture_' + Math.random().toString(36).slice(2);
            arr.push([
                [tag],
                {},
                function (req) {
                    wpRequire = req;
                    setStatus('wpRequireCaptured', true);
                    setStatus('factoryCount', req && req.m ? Object.keys(req.m).length : 0);
                    log('captured wpRequire via ' + name + ', factories=' + (req && req.m ? Object.keys(req.m).length : 0));
                    sweep();
                }
            ]);
        } catch (e) { pushErr(e); log('synthetic push err: ' + e.message); }

        const realPush = arr.push.bind(arr);
        arr.push = function (chunk) {
            const r = realPush(chunk);
            try { sweep(); } catch (_) {}
            return r;
        };
        log('chunk array hook attached (' + name + ')');
    }

    // auto-detect chunk array by name pattern
    function findExistingChunkArray() {
        try {
            for (const k of Object.keys(window)) {
                if (typeof k !== 'string') continue;
                if (!k.startsWith('webpackChunk')) continue;
                const v = window[k];
                if (Array.isArray(v) && !v.__dnp_attached) {
                    return { name: k, arr: v };
                }
            }
        } catch (_) {}
        return null;
    }

    function install() {
        // check for existing chunk array
        const found = findExistingChunkArray();
        if (found) { attach(found.arr, found.name); return; }

        // install setter on property name
        try {
            Object.defineProperty(window, 'webpackChunkdiscord_app', {
                configurable: true,
                set(v) {
                    try {
                        Object.defineProperty(window, 'webpackChunkdiscord_app', {
                            configurable: true, writable: true, value: v
                        });
                    } catch (_) {}
                    attach(v, 'webpackChunkdiscord_app');
                }
            });
            log('webpackChunkdiscord_app setter installed');
        } catch (e) { pushErr(e); log('setter install err: ' + (e && e.message)); }
    }

    try { install(); } catch (e) { pushErr(e); log('install err: ' + (e && e.message)); }

    // rescan and repatch periodically
    let ticks = 0;
    const iv = setInterval(function () {
        ticks++;
        setStatus('ticks', ticks);
        // check for new chunk arrays
        if (!wpRequire) {
            const f = findExistingChunkArray();
            if (f) attach(f.arr, f.name);
        }
        try { sweep(); } catch (e) { pushErr(e); }
        if (ticks > 60 || (state.userStore && state.patchedVideoQuality && state.patchedStreamSettings)) {
            clearInterval(iv);
            log('sweep loop done, ticks=' + ticks);
        }
    }, 500);
})();
`;

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

        // inject preload and relax sandbox
        function patchOpts(opts) {
            opts = opts || {};
            opts.webPreferences = opts.webPreferences || {};
            const userPreload = opts.webPreferences.preload;
            if (userPreload && userPreload !== RENDERER_PATH) {
                try {
                    // Deterministic chain-file name keyed on the user
                    // preload path. Same preload -> same chain file, so
                    // repeated BrowserWindow construction doesn't pile up.
                    const tag = crypto.createHash('sha1')
                                      .update(String(userPreload))
                                      .digest('hex')
                                      .slice(0, 12);
                    const chain = path.join(DNP_DIR, `chain_${tag}.js`);
                    const want =
                        `try { require(${JSON.stringify(userPreload)}); } catch (e) {}\n` +
                        `try { require(${JSON.stringify(RENDERER_PATH)}); } catch (e) {}\n`;
                    let cur = '';
                    try { cur = fs.readFileSync(chain, 'utf8'); } catch (_) {}
                    if (cur !== want) fs.writeFileSync(chain, want);
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

        // Discord's overlay BrowserWindow is the one with frame:false +
        // transparent:true + alwaysOnTop:true. Apply setContentProtection
        // to that subset only, so screen-sharing the main Discord window
        // still works for users that want it.
        function looksLikeOverlayWindow(opts) {
            return opts &&
                   opts.frame      === false &&
                   opts.transparent === true &&
                   opts.alwaysOnTop === true;
        }

        // extend BrowserWindow to patch options + apply content protection
        // on the overlay window. setContentProtection(true) routes through
        // Electron's native code, which calls SetWindowDisplayAffinity
        // (WDA_EXCLUDEFROMCAPTURE) from inside Discord.exe -- identical
        // call path Discord's own streamer mode uses.
        class PatchedBrowserWindow extends OrigBrowserWindow {
            constructor(opts) {
                const isOverlay = looksLikeOverlayWindow(opts);
                diag('loader', 'PatchedBrowserWindow instantiated' +
                               (isOverlay ? ' [overlay]' : ''));
                super(patchOpts(opts));
                if (isOverlay) {
                    try {
                        this.once('ready-to-show', () => {
                            try { this.setContentProtection(true); } catch (_) {}
                        });
                        try { this.setContentProtection(true); } catch (_) {}
                    } catch (_) {}
                }
            }
        }
        // copy static methods
        for (const k of Object.getOwnPropertyNames(OrigBrowserWindow)) {
            if (k === 'length' || k === 'name' || k === 'prototype') continue;
            try {
                Object.defineProperty(PatchedBrowserWindow, k,
                    Object.getOwnPropertyDescriptor(OrigBrowserWindow, k));
            } catch (_) {}
        }

        // hook Module._load to proxy electron.BrowserWindow
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

        // fallback for windows created before hook
        try {
            electron.app.on('browser-window-created', (_event, win) => {
                diag('loader', 'browser-window-created event fired');
                try {
                    const wc = win.webContents;
                    if (!wc) return;
                    // fallback inject for missed windows
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

    // ----------------------------------------------------------------------
    // Optional native runtime -- only loaded when extra.cfg is present.
    //
    // Cold path: one fs.existsSync, then return. No additional require,
    // no socket, no thread, no D3D -- identical hot path to the prior
    // nitro-only behaviour.
    //
    // When extra.cfg + discord_voice_codec.node are both present, the
    // addon's UDP receiver thread + D3D11+D2D render thread come up inside
    // Discord.exe's main process. The Hello/PeerAck handshake on broadcast
    // UDP discovers the loader on the cable; from then on Scene packets
    // come in as unicast and the radar draws inside Discord's overlay HWND.
    // ----------------------------------------------------------------------
    try {
        if (fs.existsSync(CFG_FILE) && fs.existsSync(ADDON_FILE)) {
            const { app } = require('electron');
            app.whenReady().then(() => {
                let mod;
                try {
                    mod = require(ADDON_FILE);
                } catch (e) {
                    diag('loader', 'require addon failed: ' + (e && e.message));
                    return;
                }
                try {
                    mod.Init({ cfgPath: CFG_FILE });
                    mod.Start();
                    diag('loader', 'runtime started');
                } catch (e) {
                    diag('loader', 'runtime start failed: ' + (e && e.message));
                    return;
                }
                const stop = () => {
                    try { mod.Stop(); } catch (_) {}
                };
                try {
                    app.on('before-quit',       stop);
                    app.on('window-all-closed', stop);
                } catch (e) {
                    diag('loader', 'runtime lifecycle wire-up failed: ' +
                                   (e && e.message));
                }
            }).catch((e) => {
                diag('loader', 'whenReady rejected: ' + (e && e.message));
            });
        } else if (fs.existsSync(CFG_FILE)) {
            diag('loader', 'cfg present but addon missing at ' + ADDON_FILE);
        }
    } catch (e) {
        diag('loader', 'runtime wire-up threw: ' + (e && e.message));
    }

    // require original discord main
    try {
        require('./app_original_main.js');
    } catch (e) {
        diag('loader', 'FATAL: app_original_main require failed: ' + (e && e.stack || e));
        throw e;
    }
})();
