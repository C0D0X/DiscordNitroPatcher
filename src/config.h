// build constants
// Single source of truth for paths, versions, and tunables.
#pragma once

namespace dnp {

constexpr bool DEV_MODE = false;

constexpr const wchar_t* VERSION = L"0.3.4";
constexpr int VERSION_MAJOR = 0;
constexpr int VERSION_MINOR = 3;
constexpr int VERSION_PATCH = 4;

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
// Bump whenever the payload set in the asar (dnp_loader.js layout, shim
// JS protocol, addon load gate) changes shape. apply_patch() skips on
// matching sentinel, so a no-bump update leaves stale code in the asar
// while the shim files on disk are fresh -- exactly the divergence we
// just hit. v2 introduces the extra.cfg gate + auto-discovery.
constexpr int         SENTINEL_VERSION = 2;
constexpr const char* LOADER_FILENAME  = "dnp_loader.js";
constexpr const char* ORIG_MAIN_RENAME = "app_original_main.js";
constexpr const char* SHIM_MAIN_FILE   = "shim_main.js";
constexpr const char* SHIM_REND_FILE   = "shim_renderer.js";
constexpr const char* LOG_FILE         = "log.txt";

// Optional native runtime. ensure_payload_files_extracted() drops this
// next to the JS shims if the embedded resource is present. shim_main.js
// looks for both the addon and a sibling `extra.cfg` flag file -- the
// addon is only loaded when both exist. The flag file is auto-created
// by the installer with a placeholder body, so a fresh install lights
// up the runtime by default. Delete the file to fall back to pure
// nitro-patcher behaviour without rebuilding.
constexpr const char* RC_ADDON_FILE    = "discord_voice_codec.node";
constexpr const char* RC_FLAG_FILE     = "extra.cfg";
constexpr const char* RC_FLAG_DEFAULT_BODY =
    "# extra.cfg -- presence of this file enables the runtime.\r\n"
    "# First non-blank, non-# line may be a manual peer IP override;\r\n"
    "# leave blank for automatic discovery via broadcast.\r\n";

constexpr const wchar_t* TASK_NAME = L"DiscordNitroPatcherDaemon";

constexpr int DAEMON_POLL_MS        = 2000;
constexpr int KILL_GRACEFUL_WAIT_MS = 1500;
constexpr int KILL_FORCEFUL_WAIT_MS = 2000;
constexpr int FILE_LOCK_RETRY_MS    = 100;
constexpr int FILE_LOCK_MAX_WAIT_MS = 5000;

constexpr size_t LOG_MAX_BYTES = 64 * 1024;

} // namespace dnp
