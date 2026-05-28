// config.h — build-time constants and identifiers.
// Single source of truth for paths, versions, and tunables.
#pragma once

namespace dnp {

// === Build-time toggles ===
// DEV_MODE: console window, verbose logging, skips self-update.
// Flip to false for friend distribution builds.
constexpr bool DEV_MODE = false;

// === Versioning ===
constexpr const wchar_t* VERSION = L"0.1.0";
constexpr int VERSION_MAJOR = 0;
constexpr int VERSION_MINOR = 1;
constexpr int VERSION_PATCH = 0;

// === GitHub auto-update ===
constexpr const wchar_t* UPDATE_HOST       = L"api.github.com";
constexpr const wchar_t* UPDATE_PATH       = L"/repos/C0D0X/DiscordNitroPatcher/releases/latest";
constexpr const wchar_t* UPDATE_USER_AGENT = L"dnp-updater/0.1";
constexpr const wchar_t* UPDATE_ASSET_NAME = L"dnp.exe";

// === Paths (resolved at runtime via SHGetKnownFolderPath) ===
constexpr const wchar_t* INSTALL_SUBDIR     = L"dnp";          // under %LOCALAPPDATA%
constexpr const wchar_t* DISCORD_SUBDIR     = L"Discord";      // under %LOCALAPPDATA%
constexpr const wchar_t* DISCORD_UPDATE_EXE = L"Update.exe";
constexpr const wchar_t* DISCORD_PROC_NAME  = L"Discord.exe";
constexpr const wchar_t* APP_DIR_PREFIX     = L"app-";
constexpr const wchar_t* ASAR_RELPATH       = L"resources\\app.asar";
constexpr const wchar_t* ASAR_BAK_RELPATH   = L"resources\\app.asar.bak";
constexpr const wchar_t* ASAR_NEW_RELPATH   = L"resources\\app.asar.new";

// === Patch identifiers (asar header sentinel + injected file names) ===
constexpr const char* SENTINEL_KEY     = "_dnp";
constexpr int         SENTINEL_VERSION = 1;
constexpr const char* LOADER_FILENAME  = "dnp_loader.js";
constexpr const char* ORIG_MAIN_RENAME = "app_original_main.js";
constexpr const char* SHIM_MAIN_FILE   = "shim_main.js";
constexpr const char* SHIM_REND_FILE   = "shim_renderer.js";
constexpr const char* LOG_FILE         = "log.txt";

// === Scheduled task ===
constexpr const wchar_t* TASK_NAME = L"DiscordNitroPatcherDaemon";

// === Timing (milliseconds) ===
constexpr int DAEMON_POLL_MS        = 2000;
constexpr int KILL_GRACEFUL_WAIT_MS = 1500;
constexpr int KILL_FORCEFUL_WAIT_MS = 2000;
constexpr int FILE_LOCK_RETRY_MS    = 100;
constexpr int FILE_LOCK_MAX_WAIT_MS = 5000;

// === Log rotation ===
constexpr size_t LOG_MAX_BYTES = 64 * 1024;

} // namespace dnp
