// build constants
// Single source of truth for paths, versions, and tunables.
#pragma once

namespace dnp {

constexpr bool DEV_MODE = false;

constexpr const wchar_t* VERSION = L"0.2.0";
constexpr int VERSION_MAJOR = 0;
constexpr int VERSION_MINOR = 2;
constexpr int VERSION_PATCH = 0;

constexpr const wchar_t* UPDATE_HOST       = L"api.github.com";
constexpr const wchar_t* UPDATE_PATH       = L"/repos/C0D0X/DiscordNitroPatcher/releases/latest";
constexpr const wchar_t* UPDATE_USER_AGENT = L"dnp-updater/0.1";
constexpr const wchar_t* UPDATE_ASSET_NAME = L"dnp.exe";

constexpr const wchar_t* INSTALL_SUBDIR     = L"dnp";
constexpr const wchar_t* DISCORD_SUBDIR     = L"Discord";
constexpr const wchar_t* DISCORD_UPDATE_EXE = L"Update.exe";
constexpr const wchar_t* DISCORD_PROC_NAME  = L"Discord.exe";
constexpr const wchar_t* APP_DIR_PREFIX     = L"app-";
constexpr const wchar_t* ASAR_RELPATH       = L"resources\\app.asar";
constexpr const wchar_t* ASAR_BAK_RELPATH   = L"resources\\app.asar.bak";
constexpr const wchar_t* ASAR_NEW_RELPATH   = L"resources\\app.asar.new";

constexpr const char* SENTINEL_KEY     = "_dnp";
constexpr int         SENTINEL_VERSION = 1;
constexpr const char* LOADER_FILENAME  = "dnp_loader.js";
constexpr const char* ORIG_MAIN_RENAME = "app_original_main.js";
constexpr const char* SHIM_MAIN_FILE   = "shim_main.js";
constexpr const char* SHIM_REND_FILE   = "shim_renderer.js";
constexpr const char* LOG_FILE         = "log.txt";

constexpr const wchar_t* TASK_NAME = L"DiscordNitroPatcherDaemon";

constexpr int DAEMON_POLL_MS        = 2000;
constexpr int KILL_GRACEFUL_WAIT_MS = 1500;
constexpr int KILL_FORCEFUL_WAIT_MS = 2000;
constexpr int FILE_LOCK_RETRY_MS    = 100;
constexpr int FILE_LOCK_MAX_WAIT_MS = 5000;

constexpr size_t LOG_MAX_BYTES = 64 * 1024;

} // namespace dnp
