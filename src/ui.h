// ui.h — minimal native Win32 dark-themed control panel.
//
// Single-window UI showing patch status with buttons to install/uninstall, view logs, and
// re-launch Discord. No background presence: window closes -> process exits.
#pragma once

namespace dnp {

// Show the main UI window. Blocks until user closes it. Returns process exit code.
int run_ui();

} // namespace dnp
