// main.cpp — entrypoint and argv dispatch.
//
// Distribution model: a single standalone dnp.exe. On first double-click (run from any
// location other than the install dir), the exe self-installs into %LOCALAPPDATA%\dnp\,
// extracts payload, registers the logon scheduled task, patches Discord, and relaunches it.
// Subsequent runs (from the install dir, or via the scheduled task) skip install and act as
// daemon / launcher.
//
// Single-instance policy: a per-user named mutex Local\dnp_single_instance gates all modes
// except --version. Install/uninstall/auto modes take priority — they terminate any other
// dnp.exe instances and then acquire the mutex. Daemon/launch modes silently exit if the
// mutex is held (another dnp.exe is already running).
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

enum class Mode { Auto, Launch, Install, Uninstall, Daemon, Version };

constexpr const wchar_t* SINGLE_INSTANCE_MUTEX = L"Local\\dnp_single_instance";

Mode parse_mode(int argc, wchar_t** argv) {
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (a == L"--install")   return Mode::Install;
        if (a == L"--uninstall") return Mode::Uninstall;
        if (a == L"--daemon")    return Mode::Daemon;
        if (a == L"--launch")    return Mode::Launch;
        if (a == L"--version" || a == L"-v") return Mode::Version;
    }
    return Mode::Auto;
}

// Is the running binary located at %LOCALAPPDATA%\dnp\dnp.exe?
bool running_from_install_dir() {
    std::wstring self = self_exe_path();
    if (self.empty()) return false;
    std::wstring expected = path_join(install_dir(), L"dnp.exe");
    return _wcsicmp(self.c_str(), expected.c_str()) == 0;
}

int do_launch() {
    // Launcher path: ensure payload extracted, patch if missing, then launch Discord.
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

// First-run install: self-installs into %LOCALAPPDATA%\dnp\, registers task, patches Discord.
// Reports outcome via MessageBox so a friend double-clicking the exe gets feedback.
int auto_first_run_install() {
    int rc = do_install();
    if (DEV_MODE) return rc;  // dev console already shows logs
    if (rc == 0) {
        MessageBoxW(nullptr,
            L"DiscordNitroPatcher installed.\n\n"
            L"Discord has been patched and relaunched. The patch will be re-applied "
            L"automatically after every Discord update.",
            L"DiscordNitroPatcher",
            MB_OK | MB_ICONINFORMATION);
    } else {
        wchar_t buf[256];
        swprintf(buf, 256,
            L"Install failed (code %d).\nSee %%LOCALAPPDATA%%\\dnp\\log.txt for details.",
            rc);
        MessageBoxW(nullptr, buf, L"DiscordNitroPatcher", MB_OK | MB_ICONERROR);
    }
    return rc;
}

// Acquire the global single-instance mutex with a policy appropriate for the given mode.
//   - Install/Uninstall/Auto: privileged — terminate any existing dnp.exe, then acquire.
//   - Daemon/Launch: silent exit if another instance holds the mutex.
//   - Version: no mutex.
// Returns mutex handle to keep alive, or nullptr to indicate the caller should exit (rc=0).
//
// out_should_continue is set to true if execution should proceed, false to exit silently.
HANDLE acquire_mutex_for_mode(Mode mode, bool& out_should_continue) {
    out_should_continue = true;
    if (mode == Mode::Version) return nullptr; // no gating

    const bool privileged = (mode == Mode::Install || mode == Mode::Uninstall || mode == Mode::Auto);

    HANDLE h = acquire_single_instance_mutex(SINGLE_INSTANCE_MUTEX);
    if (h) return h;

    if (!privileged) {
        // Another instance is running and we yield silently.
        out_should_continue = false;
        return nullptr;
    }

    // Privileged path: terminate other instances, then retry mutex acquire (with a short retry loop
    // because the OS releases the mutex asynchronously after the holder process is terminated).
    kill_processes_by_name_except_self(L"dnp.exe");
    for (int i = 0; i < 50; ++i) {
        h = acquire_single_instance_mutex(SINGLE_INSTANCE_MUTEX);
        if (h) return h;
        Sleep(100);
    }
    // Couldn't acquire even after kill — proceed anyway without mutex (best-effort).
    return nullptr;
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

    // Single-instance gate — must happen before any side-effecting work.
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
        case Mode::Daemon:
            dnp::check_for_update_and_maybe_restart();
            rc = dnp::run_daemon();
            break;
        case Mode::Launch:
            dnp::check_for_update_and_maybe_restart();
            rc = dnp::do_launch();
            break;
        case Mode::Auto:
        default:
            // No args: distinguish first-run (run from any path other than install dir)
            // from subsequent runs (run from install dir — act as launcher).
            if (dnp::running_from_install_dir()) {
                dnp::check_for_update_and_maybe_restart();
                rc = dnp::do_launch();
            } else {
                rc = dnp::auto_first_run_install();
            }
            break;
    }

    if (argv) LocalFree(argv);
    if (single_instance) CloseHandle(single_instance);
    dnp::log_shutdown();
    return rc;
}
