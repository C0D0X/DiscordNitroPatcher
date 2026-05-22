// dnp_loader.js — bootstrap embedded into Discord's app.asar.
// Constructive purpose: load external main-process shim, then hand off to original Discord main.
// Fail-safe: any exception leaves Discord booting normally.

'use strict';

(function () {
    const path = require('path');

    function logErr(prefix, err) {
        try {
            const fs = require('fs');
            const localApp = process.env.LOCALAPPDATA;
            if (!localApp) return;
            const logPath = path.join(localApp, 'dnp', 'log.txt');
            const msg = `[${prefix}] ${new Date().toISOString()} ${err && err.stack ? err.stack : err}\n`;
            fs.appendFileSync(logPath, msg);
        } catch (_) { /* swallow */ }
    }

    try {
        const localApp = process.env.LOCALAPPDATA;
        if (localApp) {
            const shimMain = path.join(localApp, 'dnp', 'shim_main.js');
            try {
                require(shimMain);
            } catch (e) {
                logErr('loader.shim_main', e);
            }
        }
    } catch (e) {
        logErr('loader.outer', e);
    }

    // Always hand control to Discord's original main, regardless of shim outcome.
    require('./app_original_main.js');
})();
