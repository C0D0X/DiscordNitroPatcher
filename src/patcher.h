// Discord patch functions
#pragma once

#include <optional>
#include <string>

namespace dnp {

std::optional<std::wstring> find_latest_discord_app_dir();
std::wstring asar_path_in_app_dir(const std::wstring& app_dir);
bool is_patched(const std::wstring& asar_path);
bool kill_discord_processes();
bool apply_patch(const std::wstring& app_dir);
bool restore_backup(const std::wstring& app_dir);
bool launch_discord();
bool ensure_payload_files_extracted();

} // namespace dnp
