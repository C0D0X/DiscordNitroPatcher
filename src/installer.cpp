// Install and uninstall
#include "installer.h"
#include "config.h"
#include "patcher.h"
#include "shortcuts.h"
#include "util.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>
#include <string>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

namespace dnp {

namespace {

bool copy_self_to_install_dir(std::wstring& out_installed_path) {
    std::wstring src = self_exe_path();
    if (src.empty()) return false;
    std::wstring dir = install_dir();
    if (!ensure_directory(dir)) return false;
    std::wstring dst = path_join(dir, L"dnp.exe");

    if (_wcsicmp(src.c_str(), dst.c_str()) == 0) {
        out_installed_path = dst;
        return true;
    }

    if (!CopyFileW(src.c_str(), dst.c_str(), FALSE)) {
        LOG_ERR("CopyFileW failed: %lu", GetLastError());
        return false;
    }
    std::wstring zone = dst + L":Zone.Identifier";
    DeleteFileW(zone.c_str());
    out_installed_path = dst;
    return true;
}

std::wstring start_menu_shortcut_path() {
    PWSTR p = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &p) != S_OK || !p) return L"";
    std::wstring out(p);
    CoTaskMemFree(p);
    return path_join(out, L"DiscordNitroPatcher.lnk");
}

bool create_start_menu_shortcut(const std::wstring& dnp_exe) {
    if (CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) != S_OK &&
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED) != S_FALSE) {
    }
    bool ok = false;
    IShellLinkW* psl = nullptr;
    if (CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                         IID_IShellLinkW, (void**)&psl) == S_OK && psl) {
        psl->SetPath(dnp_exe.c_str());
        psl->SetArguments(L"--ui");
        psl->SetDescription(L"DiscordNitroPatcher control panel");
        psl->SetWorkingDirectory(install_dir().c_str());

        IPersistFile* ppf = nullptr;
        if (psl->QueryInterface(IID_IPersistFile, (void**)&ppf) == S_OK && ppf) {
            std::wstring p = start_menu_shortcut_path();
            if (!p.empty() && ppf->Save(p.c_str(), TRUE) == S_OK) ok = true;
            ppf->Release();
        }
        psl->Release();
    }
    CoUninitialize();
    return ok;
}

bool remove_start_menu_shortcut() {
    std::wstring p = start_menu_shortcut_path();
    if (p.empty()) return false;
    return remove_file(p);
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

    int wrapped = wrap_all_discord_launchers(installed_path);
    LOG_INFO("Wrapped %d Discord launcher entr%s.", wrapped, wrapped == 1 ? "y" : "ies");

    if (create_start_menu_shortcut(installed_path)) {
        LOG_INFO("Start Menu shortcut created.");
    } else {
        LOG_WARN("Failed to create Start Menu shortcut.");
    }

    auto app_dir = find_latest_discord_app_dir();
    if (!app_dir) {
        LOG_WARN("Discord install not located. Patch deferred until first Discord launch.");
        return 0;
    }

    kill_discord_processes();
    Sleep(500);
    if (!apply_patch(*app_dir, g_force_repatch)) {
        LOG_ERR("Initial patch failed.");
        return 3;
    }

    Sleep(200);
    launch_discord();

    LOG_INFO("Install complete.");
    return 0;
}

int do_uninstall() {
    LOG_INFO("Uninstall starting.");

    int killed = kill_processes_by_name_except_self(L"dnp.exe");
    if (killed > 0) LOG_INFO("Terminated %d other dnp.exe instance(s).", killed);

    int restored = unwrap_all_discord_launchers();
    LOG_INFO("Restored %d Discord launcher entr%s.", restored, restored == 1 ? "y" : "ies");

    remove_start_menu_shortcut();

    kill_discord_processes();

    auto app_dir = find_latest_discord_app_dir();
    if (app_dir) restore_backup(*app_dir);

    remove_directory_recursive(install_dir());

    LOG_INFO("Uninstall complete.");
    return 0;
}

} // namespace dnp
