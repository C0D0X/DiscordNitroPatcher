// entry point + arg handling
#include "config.h"
#include "installer.h"
#include "patcher.h"
#include "ui.h"
#include "updater.h"
#include "util.h"

#include <shellapi.h>
#include <cwchar>
#include <string>
#include <vector>

namespace dnp {

namespace {

enum class Mode { Auto, Launch, Install, Uninstall, Version, UI };

constexpr const wchar_t* SINGLE_INSTANCE_MUTEX = L"Local\\dnp_single_instance";

Mode parse_mode(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--install")   return Mode::Install;
        if (a == L"--uninstall") return Mode::Uninstall;
        if (a == L"--launch")    return Mode::Launch;
        if (a == L"--ui")        return Mode::UI;
        if (a == L"--version" || a == L"-v") return Mode::Version;
    }
    return Mode::Auto;
}

bool running_from_install_dir() {
    std::wstring self = self_exe_path();
    if (self.empty()) return false;
    std::wstring expected = path_join(install_dir(), L"dnp.exe");
    return _wcsicmp(self.c_str(), expected.c_str()) == 0;
}

int do_launch() {
    ensure_payload_files_extracted();
    auto app_dir = find_latest_discord_app_dir();
    if (!app_dir) {
        LOG_WARN("Discord not found.");
        return 1;
    }
    std::wstring asar = asar_path_in_app_dir(*app_dir);
    if (!is_patched(asar)) {
        LOG_INFO("Discord unpatched; killing + patching before launch.");
        kill_discord_processes();
        Sleep(300);
        if (!apply_patch(*app_dir)) {
            LOG_ERR("Patch failed; launching anyway.");
        }
    }
    launch_discord();
    return 0;
}

// Acquire single-instance mutex. Privileged modes kill competing instances.
HANDLE acquire_mutex_for_mode(Mode mode, bool& out_should_continue) {
    out_should_continue = true;
    if (mode == Mode::Version) return nullptr; // no gating

    const bool privileged = (mode == Mode::Install || mode == Mode::Uninstall ||
                             mode == Mode::Auto    || mode == Mode::UI);

    HANDLE h = acquire_single_instance_mutex(SINGLE_INSTANCE_MUTEX);
    if (h) return h;

    if (!privileged) {
        // Another instance is running and we yield silently.
        out_should_continue = false;
        return nullptr;
    }

    kill_processes_by_name_except_self(L"dnp.exe");
    for (int i = 0; i < 50; ++i) {
        h = acquire_single_instance_mutex(SINGLE_INSTANCE_MUTEX);
        if (h) return h;
        Sleep(100);
    }
    return nullptr;
}

} // namespace

} // namespace dnp
int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    using dnp::Mode;
    Mode m = dnp::parse_mode(argc, argv);

    bool should_continue = true;
    HANDLE single_instance = dnp::acquire_mutex_for_mode(m, should_continue);
    if (!should_continue) {
        if (argv) LocalFree(argv);
        return 0;
    }

    dnp::log_init();
    LOG_INFO("dnp %ls mode=%d", dnp::VERSION, (int)m);

    int rc = 0;
    switch (m) {
        case Mode::Version: {
            wchar_t buf[64];
            swprintf(buf, 64, L"dnp %ls\n", dnp::VERSION);
            MessageBoxW(nullptr, buf, L"DiscordNitroPatcher", MB_OK | MB_ICONINFORMATION);
            rc = 0;
            break;
        }
        case Mode::Install:
            rc = dnp::do_install();
            break;
        case Mode::Uninstall:
            rc = dnp::do_uninstall();
            break;
        case Mode::Launch:
            dnp::check_for_update_and_maybe_restart();
            rc = dnp::do_launch();
            break;
        case Mode::UI:
            dnp::check_for_update_and_maybe_restart();
            rc = dnp::run_ui();
            break;
        case Mode::Auto:
        default:
            dnp::check_for_update_and_maybe_restart();
            rc = dnp::run_ui();
            break;
    }

    if (argv) LocalFree(argv);
    if (single_instance) CloseHandle(single_instance);
    dnp::log_shutdown();
    return rc;
}
