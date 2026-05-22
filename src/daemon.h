// daemon.h — background poll loop that detects Discord launches and applies patch.
#pragma once

namespace dnp {

// Single-instance daemon. Loops until killed. Returns process exit code.
int run_daemon();

} // namespace dnp
