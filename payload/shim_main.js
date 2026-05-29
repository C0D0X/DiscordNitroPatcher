// inject preload into discord windows

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
        // chain user preload with ours
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

    // replace BrowserWindow export
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
