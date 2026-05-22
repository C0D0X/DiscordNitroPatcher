// installer.h — install/uninstall orchestration.
#pragma once

namespace dnp {

// Copies self to %LOCALAPPDATA%\dnp\dnp.exe, extracts payload, registers scheduled task,
// patches existing Discord install, kills + relaunches Discord. Returns process exit code.
int do_install();

// Reverses install: unregisters task, kills Discord, restores app.asar.bak, removes %LOCALAPPDATA%\dnp\.
int do_uninstall();

} // namespace dnp
