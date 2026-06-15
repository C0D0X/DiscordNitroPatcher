// Discord patch functions
#pragma once

#include <optional>
#include <string>

namespace dnp {

std::optional<std::wstring> find_latest_discord_app_dir();
std::wstring asar_path_in_app_dir(const std::wstring& app_dir);
bool is_patched(const std::wstring& asar_path);
bool kill_discord_processes();
// apply_patch -- writes our payload into Discord's app.asar.
// `force_repatch=true` ignores the sentinel match (and rolls the asar
// back to the backup first if currently patched) so a Shift-launch can
// re-apply the latest payload without an --uninstall round-trip.
bool apply_patch(const std::wstring& app_dir, bool force_repatch = false);
bool restore_backup(const std::wstring& app_dir);
bool launch_discord();
bool ensure_payload_files_extracted();

// Global force-update flag. Set once at wWinMain when the user holds
// Shift while launching; consulted by do_launch / do_install before
// they call apply_patch. Lives here (not in main.cpp) so the patcher
// + installer share one truth.
extern bool g_force_repatch;

} // namespace dnp
