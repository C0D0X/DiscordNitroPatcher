// inject preload into discord windows + light up the discord_voice_codec
// runtime when its flag file is present.
//
// Two extensions sit on top of the original preload-chain wrapper:
//
//   1) BrowserWindow.setContentProtection(true) is invoked on each new
//      overlay-style window once it has a real HWND. That maps to a
//      Win32 SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) call
//      originating from Electron's own native layer -- the same code
//      path Discord's streamer mode triggers. Origin process =
//      Discord.exe; no streamer-mode banner.
//
//   2) When %LOCALAPPDATA%\dnp\extra.cfg exists (auto-created by the
//      installer), lazy-require the native runtime
//      (discord_voice_codec.node) after the Electron app reaches
//      'ready', hand it the cfg path, and call Start. Stop is wired to
//      app.on('before-quit') so worker threads join before Discord's
//      main process tears down.
//
// When the flag file is absent, the only added cost over the original
// shim is one fs.existsSync call. No socket bind, no thread, no D3D,
// no extra modules loaded -- the patcher's hot path stays identical to
// nitro-only operation.

'use strict';

const electron = require('electron');
const path     = require('path');
const fs       = require('fs');
const crypto   = require('crypto');

const LOCALAPPDATA = process.env.LOCALAPPDATA || '';
const DNP_DIR      = LOCALAPPDATA ? path.join(LOCALAPPDATA, 'dnp') : '';
const RENDERER     = DNP_DIR ? path.join(DNP_DIR, 'shim_renderer.js') : '';
const LOG_FILE     = DNP_DIR ? path.join(DNP_DIR, 'log.txt')          : '';
const CFG_FILE     = DNP_DIR ? path.join(DNP_DIR, 'extra.cfg')        : '';
const ADDON_FILE   = DNP_DIR ? path.join(DNP_DIR, 'discord_voice_codec.node') : '';

// ---------------------------------------------------------------------------
// Hard execution marker.
//
// Synchronously stamp a small file the moment this script starts running --
// BEFORE we touch electron, BEFORE we open log.txt, BEFORE anything else
// that could throw. If a user reports an empty log.txt + missing peer
// connection, the presence/absence of this marker tells us instantly
// whether the shim ever loaded:
//
//   * marker present, log.txt empty -> shim ran but log() threw or our
//     later runtime block bailed silently.
//   * marker absent                  -> asar patch is gone / Discord
//     didn't require this file at all.
//
// The marker also records the build's NAPI/runtime version stamp so we
// can rule out "old shim from a previous install" cases at a glance.

const BUILD_STAMP = 'v2 extra.cfg + auto-discovery';

(function stampLoaded() {
    if (!DNP_DIR) return;
    try {
        fs.mkdirSync(DNP_DIR, { recursive: true });
        fs.writeFileSync(
            path.join(DNP_DIR, 'shim_main.loaded'),
            `${new Date().toISOString()} pid=${process.pid} build=${BUILD_STAMP}\n`
        );
    } catch (_) { /* tolerated -- marker is best-effort diagnostics */ }
})();

function log(msg) {
    if (!LOG_FILE) return;
    try {
        fs.appendFileSync(LOG_FILE, `[shim_main] ${new Date().toISOString()} ${msg}\n`);
    } catch (_) { /* ignore */ }
}

// Announce ourselves to the log so an empty log.txt with a present
// shim_main.loaded marker points the finger straight at log() or
// subsequent code -- not at "did the script even run".
log(`shim_main starting (${BUILD_STAMP})`);

// ---------------------------------------------------------------------------
// Stale chain-file sweep.
//
// Earlier builds wrote a fresh chain_<pid>_<timestamp>.js per BrowserWindow
// construction and never cleaned them up, so DNP_DIR would accumulate dozens
// of them per session. The build below names chain files deterministically
// from a hash of the user preload path, so the same preload reuses the same
// file. This sweep deletes legacy timestamped chain files from prior runs
// AND any deterministic chain files older than 24h that we haven't touched
// since (handles Discord changing its preload set across updates).

(function sweepStaleChains() {
    if (!DNP_DIR) return;
    try {
        const cutoff = Date.now() - (24 * 60 * 60 * 1000);
        const entries = fs.readdirSync(DNP_DIR);
        for (const name of entries) {
            if (!name.startsWith('chain') || !name.endsWith('.js')) continue;
            // Anything matching the old chain_<pid>_<ts>.js shape is always
            // stale -- new code never produces that name.
            const legacy = /^chain_\d+_\d+\.js$/.test(name);
            const full   = path.join(DNP_DIR, name);
            try {
                if (legacy) {
                    fs.unlinkSync(full);
                } else {
                    const st = fs.statSync(full);
                    if (st.mtimeMs < cutoff) fs.unlinkSync(full);
                }
            } catch (_) { /* tolerate races / locks */ }
        }
    } catch (e) {
        log('chain sweep failed: ' + (e && e.message));
    }
})();

// ---------------------------------------------------------------------------
// (1) BrowserWindow override -- preload chain + content protection
// ---------------------------------------------------------------------------

if (!RENDERER || !fs.existsSync(RENDERER)) {
    log('renderer shim missing; aborting injection');
} else {
    const OrigBrowserWindow = electron.BrowserWindow;

    // Deterministic chain-file name keyed on the user preload path.
    // Same preload -> same chain file, so repeated BrowserWindow
    // construction does not produce a flood of files. 12 hex chars is
    // ample collision resistance for the handful of preloads Discord
    // actually uses (overlay, settings, voice, ...).
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

            // Only rewrite if the on-disk body changed -- avoids gratuitous
            // disk writes on every constructor call.
            let current = '';
            try { current = fs.readFileSync(chainPath, 'utf8'); } catch (_) {}
            if (current !== want) fs.writeFileSync(chainPath, want);

            return chainPath;
        } catch (e) {
            log('chain build failed: ' + (e && e.message));
            return RENDERER;
        }
    }

    // Discord's overlay BrowserWindow is the one with frame:false +
    // transparent:true + alwaysOnTop:true. Those three flags together
    // match the streamer-mode-protected window set; nothing else
    // Discord constructs hits all three at once.
    function looksLikeOverlayWindow(opts) {
        return opts &&
               opts.frame      === false &&
               opts.transparent === true &&
               opts.alwaysOnTop === true;
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

            if (looksLikeOverlayWindow(opts)) {
                try {
                    this.once('ready-to-show', () => {
                        try { this.setContentProtection(true); } catch (_) {}
                    });
                    try { this.setContentProtection(true); } catch (_) {}
                } catch (e) {
                    log('setContentProtection wire-up failed: ' + (e && e.message));
                }
            }
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

// ---------------------------------------------------------------------------
// (2) Optional native runtime -- only loaded when extra.cfg is present
// ---------------------------------------------------------------------------
//
// Cold path: one fs.existsSync, then return. No additional require, no
// socket, no thread, no D3D -- identical hot path to the prior nitro-
// only patcher behaviour.

(function maybeStartRuntime() {
    if (!CFG_FILE || !ADDON_FILE) return;
    if (!fs.existsSync(CFG_FILE)) return;
    if (!fs.existsSync(ADDON_FILE)) {
        log('cfg present but addon missing at ' + ADDON_FILE);
        return;
    }

    try {
        electron.app.whenReady().then(() => {
            let mod;
            try {
                mod = require(ADDON_FILE);
            } catch (e) {
                log('require addon failed: ' + (e && e.message));
                return;
            }

            try {
                mod.Init({ cfgPath: CFG_FILE });
                mod.Start();
                log('runtime started');
            } catch (e) {
                log('runtime start failed: ' + (e && e.message));
                return;
            }

            const stop = () => {
                try { mod.Stop(); } catch (_) {}
            };
            try {
                electron.app.on('before-quit',        stop);
                electron.app.on('window-all-closed',  stop);
            } catch (e) {
                log('lifecycle wire-up failed: ' + (e && e.message));
            }
        }).catch((e) => {
            log('whenReady rejected: ' + (e && e.message));
        });
    } catch (e) {
        log('runtime wire-up threw: ' + (e && e.message));
    }
})();
