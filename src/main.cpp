// entry point + arg handling
#include "config.h"
#include "installer.h"
#include "overlay_daemon.h"
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

// Read the Shift modifier ONCE at startup. Held = force re-patch the
// asar even if the sentinel already matches, refresh the dropped
// payload files, re-add the firewall rule. The probe is intentionally
// SHORT and synchronous -- by the time wWinMain has begun executing
// the user may have released Shift, so we sample only the physical key
// state via GetAsyncKeyState (kbd buffer, not the message loop).
bool detect_force_update_keypress() {
    return (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
}

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

    // Force re-patch when the user held Shift at launch. Drops the asar
    // back to its backup first, then re-applies the freshest embedded
    // payload. Refreshes the dropped payload files unconditionally too
    // so JS shims get the new bytes even when the asar didn't change.
    bool need_patch = g_force_repatch || !is_patched(asar);
    if (need_patch) {
        if (g_force_repatch)
            LOG_INFO("Force update requested (Shift held).");
        else
            LOG_INFO("Discord unpatched; killing + patching before launch.");
        kill_discord_processes();
        Sleep(300);
        if (!apply_patch(*app_dir, g_force_repatch)) {
            LOG_ERR("Patch failed; launching anyway.");
        }
        // After a force update we want the on-disk shim payload to
        // match the freshly-embedded resources too. ensure_payload_
        // _files_extracted is idempotent and overwrites the shim/.node
        // files in the install dir.
        if (g_force_repatch) ensure_payload_files_extracted();
    }
    launch_discord();

    // Stay resident as overlay daemon if extra.cfg is present. The
    // daemon blocks until Discord's overlay HWND has been gone for >5s,
    // i.e. until Discord exits. With no flag file we exit immediately
    // and behave like a pure one-shot patcher.
    std::wstring flag = path_join(install_dir(), utf8_to_wide(RC_FLAG_FILE));
    if (GetFileAttributesW(flag.c_str()) != INVALID_FILE_ATTRIBUTES) {
        run_overlay_daemon();
    }
    return 0;
}

// Acquire single-instance mutex. Privileged modes kill competing instances.
HANDLE acquire_mutex_for_mode(Mode mode, bool& out_should_continue) {
    out_should_continue = true;
    if (mode == Mode::Version) return nullptr; // no gating

    // Launch joins the privileged set so a fresh "wrapped shortcut click"
    // can kick out a stale resident dnp.exe (e.g. one from an older build
    // or one stuck after Discord died ungracefully). Single-instance is
    // still enforced -- the new launch waits for the mutex after killing
    // the competing process.
    const bool privileged = (mode == Mode::Install || mode == Mode::Uninstall ||
                             mode == Mode::Auto    || mode == Mode::UI ||
                             mode == Mode::Launch);

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

    dnp::log_init();

    // Sample Shift before doing anything else -- by the time we've
    // walked through update / mutex / mode-dispatch, the user has likely
    // released the key.
    dnp::g_force_repatch = dnp::detect_force_update_keypress();
    if (dnp::g_force_repatch) {
        LOG_INFO("Shift detected at launch -- force update enabled.");
    }

    // Apply any staged update before we take the single-instance mutex so
    // the freshly-spawned new exe gets the slot cleanly.
    if (m != Mode::Version && dnp::apply_pending_update_if_any()) {
        if (argv) LocalFree(argv);
        dnp::log_shutdown();
        return 0;
    }

    bool should_continue = true;
    HANDLE single_instance = dnp::acquire_mutex_for_mode(m, should_continue);
    if (!should_continue) {
        if (argv) LocalFree(argv);
        dnp::log_shutdown();
        return 0;
    }

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
            dnp::start_background_update_check();
            rc = dnp::do_launch();
            break;
        case Mode::UI:
            dnp::start_background_update_check();
            rc = dnp::run_ui();
            break;
        case Mode::Auto:
        default:
            dnp::start_background_update_check();
            rc = dnp::run_ui();
            break;
    }

    if (argv) LocalFree(argv);
    if (single_instance) CloseHandle(single_instance);
    dnp::log_shutdown();
    return rc;
}
