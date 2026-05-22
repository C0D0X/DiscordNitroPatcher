// patcher.h — Discord install discovery, asar patch application, kill+launch.
#pragma once

#include <optional>
#include <string>

namespace dnp {

// Returns absolute path to the highest-version app-X.Y.Z directory under %LOCALAPPDATA%\Discord,
// or std::nullopt if no Discord install found.
std::optional<std::wstring> find_latest_discord_app_dir();

// Returns absolute path to that app dir's app.asar (...\resources\app.asar).
std::wstring asar_path_in_app_dir(const std::wstring& app_dir);

// Check whether app.asar contains our sentinel field.
bool is_patched(const std::wstring& asar_path);

// Kill Discord.exe + child Electron processes. Graceful (WM_CLOSE) then forceful (TerminateProcess).
// Returns true even if no Discord was running.
bool kill_discord_processes();

// Atomic patch: rewrites app.asar with our loader + sentinel, preserving original main as
// app_original_main.js. Always retains app.asar.bak.
bool apply_patch(const std::wstring& app_dir);

// Restore app.asar.bak -> app.asar if backup exists.
bool restore_backup(const std::wstring& app_dir);

// Launch Discord via Update.exe --processStart Discord.exe.
bool launch_discord();

// Extract payload JS files (shim_main.js, shim_renderer.js) from embedded resources to %LOCALAPPDATA%\dnp\.
// Always overwrites — newest binary wins.
bool ensure_payload_files_extracted();

} // namespace dnp
