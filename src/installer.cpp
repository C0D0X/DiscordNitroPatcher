// installer.cpp — first-run install + clean uninstall.
#include "installer.h"
#include "config.h"
#include "patcher.h"
#include "util.h"

#include <string>

namespace dnp {

namespace {

bool copy_self_to_install_dir(std::wstring& out_installed_path) {
    std::wstring src = self_exe_path();
    if (src.empty()) return false;
    std::wstring dir = install_dir();
    if (!ensure_directory(dir)) return false;
    std::wstring dst = path_join(dir, L"dnp.exe");

    // If we are already running from the install path, skip copy.
    if (_wcsicmp(src.c_str(), dst.c_str()) == 0) {
        out_installed_path = dst;
        return true;
    }

    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        LOG_ERR("CopyFileW failed: %lu", GetLastError());
        return false;
    }
    // Strip mark-of-the-web on copied binary.
    std::wstring zone = dst + L":Zone.Identifier";
    DeleteFileW(zone.c_str());
    out_installed_path = dst;
    return true;
}

bool register_scheduled_task(const std::wstring& exe_path) {
    // /sc onlogon — runs at user logon. /rl limited — current user privileges, no elevation prompt.
    // /it — interactive (allows showing UI in user session). /f — force overwrite if existing.
    std::wstring cmd = L"schtasks.exe /create /f /tn ";
    cmd += TASK_NAME;
    cmd += L" /sc onlogon /rl limited /it /tr \"\\\"";
    cmd += exe_path;
    cmd += L"\\\" --daemon\"";

    int code = run_command(cmd, true, false);
    if (code != 0) {
        LOG_WARN("schtasks create returned %d", code);
        return false;
    }
    return true;
}

bool unregister_scheduled_task() {
    std::wstring cmd = L"schtasks.exe /delete /f /tn ";
    cmd += TASK_NAME;
    int code = run_command(cmd, true, false);
    if (code != 0) {
        LOG_WARN("schtasks delete returned %d", code);
        return false;
    }
    return true;
}

} // namespace

int do_install() {
    LOG_INFO("Install starting.");

    std::wstring installed_path;
    if (!copy_self_to_install_dir(installed_path)) {
        LOG_ERR("Self-copy failed.");
        return 1;
    }
    if (!ensure_payload_files_extracted()) {
        LOG_ERR("Payload extraction failed.");
        return 2;
    }
    if (!register_scheduled_task(installed_path)) {
        LOG_WARN("Scheduled task registration failed; continuing without daemon autorun.");
    }

    auto app_dir = find_latest_discord_app_dir();
    if (!app_dir) {
        LOG_WARN("Discord install not located. Patch deferred until Discord is installed.");
        return 0;
    }

    // Kill, patch, relaunch.
    kill_discord_processes();
    Sleep(500);
    if (!apply_patch(*app_dir)) {
        LOG_ERR("Initial patch failed.");
        return 3;
    }
    Sleep(200);
    launch_discord();

    // Also kick off daemon now so user gets coverage immediately without waiting for next logon.
    {
        std::wstring cmd = L"\"" + installed_path + L"\" --daemon";
        run_command(cmd, false, false);
    }

    LOG_INFO("Install complete.");
    return 0;
}

int do_uninstall() {
    LOG_INFO("Uninstall starting.");
    unregister_scheduled_task();

    // Stop running daemon: best-effort by name.
    {
        std::wstring cmd = L"taskkill.exe /f /im dnp.exe";
        run_command(cmd, true, false);
    }

    kill_discord_processes();

    auto app_dir = find_latest_discord_app_dir();
    if (app_dir) restore_backup(*app_dir);

    // Recursive delete of install dir.
    remove_directory_recursive(install_dir());

    LOG_INFO("Uninstall complete.");
    return 0;
}

} // namespace dnp
