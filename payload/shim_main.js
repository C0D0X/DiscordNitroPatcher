// Injects the renderer preload into every BrowserWindow Discord constructs
// so the fake-Nitro screenshare patches (bitrate / fps / premium flag) run
// in the renderer even when Discord's own preload takes precedence.

'use strict';

const electron = require('electron');
const path     = require('path');
const fs       = require('fs');
const crypto   = require('crypto');

const LOCALAPPDATA = process.env.LOCALAPPDATA || '';
const DNP_DIR      = LOCALAPPDATA ? path.join(LOCALAPPDATA, 'dnp') : '';
const RENDERER     = DNP_DIR ? path.join(DNP_DIR, 'shim_renderer.js') : '';
const LOG_FILE     = DNP_DIR ? path.join(DNP_DIR, 'log.txt')          : '';

// Presence of this marker separates "shim never ran" from "shim ran but
// log() threw" during triage. Stamped before anything that can raise.
const BUILD_STAMP = 'v3 nitro-only';

(function stampLoaded() {
    if (!DNP_DIR) return;
    try {
        fs.mkdirSync(DNP_DIR, { recursive: true });
        fs.writeFileSync(
            path.join(DNP_DIR, 'shim_main.loaded'),
            `${new Date().toISOString()} pid=${process.pid} build=${BUILD_STAMP}\n`
        );
    } catch (_) { }
})();

function log(msg) {
    if (!LOG_FILE) return;
    try {
        fs.appendFileSync(LOG_FILE, `[shim_main] ${new Date().toISOString()} ${msg}\n`);
    } catch (_) { }
}

log(`shim_main starting (${BUILD_STAMP})`);

// Prior builds wrote a fresh chain_<pid>_<ts>.js per BrowserWindow ctor and
// never cleaned them up. Names are deterministic now (hash of user preload),
// so this pass reclaims legacy files plus deterministic files not touched
// in 24h (covers Discord swapping its preload set across updates).
(function sweepStaleChains() {
    if (!DNP_DIR) return;
    try {
        const cutoff = Date.now() - (24 * 60 * 60 * 1000);
        const entries = fs.readdirSync(DNP_DIR);
        for (const name of entries) {
            if (!name.startsWith('chain') || !name.endsWith('.js')) continue;
            const legacy = /^chain_\d+_\d+\.js$/.test(name);
            const full   = path.join(DNP_DIR, name);
            try {
                if (legacy) {
                    fs.unlinkSync(full);
                } else {
                    const st = fs.statSync(full);
                    if (st.mtimeMs < cutoff) fs.unlinkSync(full);
                }
            } catch (_) { }
        }
    } catch (e) {
        log('chain sweep failed: ' + (e && e.message));
    }
})();

if (!RENDERER || !fs.existsSync(RENDERER)) {
    log('renderer shim missing; aborting injection');
} else {
    const OrigBrowserWindow = electron.BrowserWindow;

    // Deterministic chain-file name keyed on the user preload path. Same
    // preload -> same chain file, so repeated BrowserWindow construction
    // does not produce a flood of files. 12 hex chars is enough collision
    // resistance for the handful of preloads Discord actually ships.
    function buildChainPreload(userPreload) {
        try {
            const tag = crypto.createHash('sha1')
                              .update(String(userPreload || ''))
                              .digest('hex')
                              .slice(0, 12);
            const chainPath = path.join(DNP_DIR, `chain_${tag}.js`);

            const want = (userPreload
                ? `try { require(${JSON.stringify(userPreload)}); } catch (e) {}\n`
                : '')
                + `try { require(${JSON.stringify(RENDERER)}); } catch (e) {}\n`;

            let current = '';
            try { current = fs.readFileSync(chainPath, 'utf8'); } catch (_) { }
            if (current !== want) fs.writeFileSync(chainPath, want);

            return chainPath;
        } catch (e) {
            log('chain build failed: ' + (e && e.message));
            return RENDERER;
        }
    }

    class PatchedBrowserWindow extends OrigBrowserWindow {
        constructor(opts) {
            opts = opts || {};
            opts.webPreferences = opts.webPreferences || {};

            const userPreload = opts.webPreferences.preload;
            const chained = (userPreload && userPreload !== RENDERER)
                ? buildChainPreload(userPreload)
                : RENDERER;

            opts.webPreferences.preload          = chained;
            opts.webPreferences.sandbox          = false;
            opts.webPreferences.contextIsolation = false;
            opts.webPreferences.nodeIntegration  = true;

            super(opts);
        }
    }

    try {
        const desc = Object.getOwnPropertyDescriptor(electron, 'BrowserWindow') || { configurable: true };
        Object.defineProperty(electron, 'BrowserWindow', {
            configurable: true,
            enumerable: desc.enumerable !== false,
            get() { return PatchedBrowserWindow; },
            set() { }
        });
        log('BrowserWindow override installed');
    } catch (e) {
        log('failed to install BrowserWindow override: ' + (e && e.message));
    }
}
