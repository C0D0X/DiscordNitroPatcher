// GitHub releases self-update.
//
// Non-disruptive model:
//   - start_background_update_check() spawns a detached thread that polls
//     the GitHub releases API and stages the new exe at
//     <install_dir>\dnp.exe.new. It never restarts the running process.
//   - apply_pending_update_if_any() runs at startup, before the
//     single-instance mutex is acquired. If a staged exe is present, the
//     running exe is renamed to dnp.exe.old, the staged exe is moved into
//     place, and the new exe is spawned with the same arguments. The old
//     copy is cleaned up on the next startup.
#pragma once

namespace dnp {

// Returns true if a staged update was applied and a new process was
// spawned. Caller should exit immediately without taking any locks.
bool apply_pending_update_if_any();

// Fire-and-forget background check. Honors DEV_MODE and a 6-hour
// throttle. Safe to call from any mode that has a meaningful lifetime
// (UI / Auto). Launch mode also calls it; if the process exits before
// the download finishes, the next run picks it up.
void start_background_update_check();

} // namespace dnp
