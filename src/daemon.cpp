// daemon.cpp — 2s poll loop watching for Discord.exe.
#include "daemon.h"
#include "config.h"
#include "patcher.h"
#include "util.h"

#include <psapi.h>
#include <tlhelp32.h>

#include <cwchar>

namespace dnp {

namespace {

bool any_discord_running() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, DISCORD_PROC_NAME) == 0) {
                found = true;
                break;
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

} // namespace

int run_daemon() {
    // Single-instance guard.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\dnp_single_instance_daemon");
    if (!mutex || GetLastError() == ERROR_ALREADY_EXISTS) {
        LOG_INFO("Daemon already running, exiting.");
        if (mutex) CloseHandle(mutex);
        return 0;
    }

    LOG_INFO("Daemon started, polling every %d ms.", DAEMON_POLL_MS);
    ensure_payload_files_extracted();

    bool last_seen = false;
    while (true) {
        bool now = any_discord_running();
        if (now && !last_seen) {
            // Discord just appeared.
            auto app_dir = find_latest_discord_app_dir();
            if (app_dir) {
                std::wstring asar = asar_path_in_app_dir(*app_dir);
                if (!is_patched(asar)) {
                    LOG_INFO("Discord unpatched. Killing + repatching + relaunching.");
                    kill_discord_processes();
                    Sleep(500);
                    ensure_payload_files_extracted();
                    if (apply_patch(*app_dir)) {
                        Sleep(200);
                        launch_discord();
                    } else {
                        LOG_ERR("Patch attempt failed; will retry on next launch.");
                    }
                } else {
                    LOG_DBG("Discord seen, already patched.");
                }
            } else {
                LOG_WARN("Discord running but no app-* dir located.");
            }
        }
        last_seen = now;
        Sleep(DAEMON_POLL_MS);
    }
    // Unreachable.
}

} // namespace dnp
