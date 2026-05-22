// main.cpp — entrypoint and argv dispatch.
#include "config.h"
#include "daemon.h"
#include "installer.h"
#include "patcher.h"
#include "updater.h"
#include "util.h"

#include <shellapi.h>
#include <cwchar>
#include <string>
#include <vector>

namespace dnp {

namespace {

enum class Mode { Launch, Install, Uninstall, Daemon, Version };

Mode parse_mode(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--install")   return Mode::Install;
        if (a == L"--uninstall") return Mode::Uninstall;
        if (a == L"--daemon")    return Mode::Daemon;
        if (a == L"--launch")    return Mode::Launch;
        if (a == L"--version" || a == L"-v") return Mode::Version;
    }
    return Mode::Launch;
}

int do_launch() {
    // Wrapper path: ensure payload extracted, check Discord, patch if missing, then launch Discord.
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

} // namespace

} // namespace dnp

// ============================================================================
// WinMain
// ============================================================================
int APIENTRY wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);

    using dnp::Mode;
    Mode m = dnp::parse_mode(argc, argv);

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
        case Mode::Daemon:
            dnp::check_for_update_and_maybe_restart();
            rc = dnp::run_daemon();
            break;
        case Mode::Launch:
        default:
            dnp::check_for_update_and_maybe_restart();
            rc = dnp::do_launch();
            break;
    }

    if (argv) LocalFree(argv);
    dnp::log_shutdown();
    return rc;
}
