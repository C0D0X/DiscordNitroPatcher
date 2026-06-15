#pragma once
//
// overlay_daemon -- in-dnp.exe radar receiver + compositor.
//
// When extra.cfg is present in the install dir, dnp.exe stays resident
// after launching Discord, runs a UDP listener on the radar port, polls
// for Discord's overlay HWND, attaches a D3D11 + Direct2D swap chain
// to that HWND, and paints the radar inside Discord's window. Exits
// when Discord's main process exits.
//
// Why this lives in dnp.exe rather than as a separate exe:
//   * dnp.exe already has install / wrapper / log infrastructure.
//   * Discord's overlay HWND lookup happens from a process that's a
//     known Discord-side helper, not a generic third-party process.
//   * One install footprint, not two.
//

namespace dnp {

// Blocks the calling thread until Discord exits or the OS stops us.
// Spawns its own internal worker threads as needed (UDP recv, render).
// Returns 0 on a clean shutdown, non-zero on init failure.
int run_overlay_daemon();

} // namespace dnp
