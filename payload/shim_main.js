// shim_main.js — Discord main-process injector.
//
// Constructive purpose: configure each Electron BrowserWindow with a renderer-side preload script
// that performs the premium-tier presentation override for screen-capture quality presets.
//
// Approach: wrap electron.BrowserWindow so every window receives our preload, chaining any
// preload set by Discord. nodeIntegration / contextIsolation are tuned so the renderer
// shim can access window.webpackChunkdiscord_app and the node `fs` module for diagnostics.

'use strict';

const electron = require('electron');
const path     = require('path');
const fs       = require('fs');

const LOCALAPPDATA = process.env.LOCALAPPDATA || '';
const DNP_DIR      = LOCALAPPDATA ? path.join(LOCALAPPDATA, 'dnp') : '';
const RENDERER     = DNP_DIR ? path.join(DNP_DIR, 'shim_renderer.js') : '';
const LOG_FILE     = DNP_DIR ? path.join(DNP_DIR, 'log.txt')          : '';

function log(msg) {
    if (!LOG_FILE) return;
    try {
        fs.appendFileSync(LOG_FILE, `[shim_main] ${new Date().toISOString()} ${msg}\n`);
    } catch (_) { /* ignore */ }
}

if (!RENDERER || !fs.existsSync(RENDERER)) {
    log('renderer shim missing; aborting injection');
} else {
    const OrigBrowserWindow = electron.BrowserWindow;

    function buildChainPreload(userPreload) {
        // Generate a per-process preload chain file that requires the user's preload (if any),
        // then requires our renderer shim. Lives in DNP_DIR so we control cleanup.
        try {
            const chainPath = path.join(DNP_DIR, `chain_${process.pid}_${Date.now()}.js`);
            const lines = [];
            if (userPreload) {
                lines.push(`try { require(${JSON.stringify(userPreload)}); } catch (e) {}`);
            }
            lines.push(`try { require(${JSON.stringify(RENDERER)}); } catch (e) {}`);
            fs.writeFileSync(chainPath, lines.join('\n') + '\n');
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

    // Preserve static members and prototype linkage by replacing the export with our subclass.
    try {
        const desc = Object.getOwnPropertyDescriptor(electron, 'BrowserWindow') || { configurable: true };
        Object.defineProperty(electron, 'BrowserWindow', {
            configurable: true,
            enumerable: desc.enumerable !== false,
            get() { return PatchedBrowserWindow; },
            set() { /* no-op: lock our override in */ }
        });
        log('BrowserWindow override installed');
    } catch (e) {
        log('failed to install BrowserWindow override: ' + (e && e.message));
    }
}
