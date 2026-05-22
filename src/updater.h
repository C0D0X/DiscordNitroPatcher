// updater.h — GitHub releases self-update.
#pragma once

namespace dnp {

// Checks GitHub for a newer release. If newer + asset available, downloads to %TEMP%,
// schedules an out-of-process swap+restart, and exits the current process.
// Skipped entirely when DEV_MODE is true. Safe to call on every launch.
void check_for_update_and_maybe_restart();

} // namespace dnp
