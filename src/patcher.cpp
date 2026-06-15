// Discord patch - install, kill, launch, patch/restore
#include "patcher.h"
#include "asar.h"
#include "config.h"
#include "util.h"
#include "../res/resource.h"

#include <tlhelp32.h>
#include <psapi.h>
#include <shellapi.h>

#include <algorithm>
#include <cstring>
#include <cwchar>
#include <unordered_set>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace dnp {

namespace {

struct SemVer {
    int parts[4] = {0, 0, 0, 0};
};

bool parse_semver_after_prefix(const std::wstring& name, const std::wstring& prefix, SemVer& out) {
    if (name.size() <= prefix.size()) return false;
    if (name.compare(0, prefix.size(), prefix) != 0) return false;
    const wchar_t* p = name.c_str() + prefix.size();
    int idx = 0;
    while (*p && idx < 4) {
        int v = 0;
        bool any = false;
        while (*p >= L'0' && *p <= L'9') {
            v = v * 10 + (*p - L'0');
            ++p;
            any = true;
        }
        if (!any) return false;
        out.parts[idx++] = v;
        if (*p == L'.') ++p;
        else break;
    }
    return idx > 0;
}

int compare_semver(const SemVer& a, const SemVer& b) {
    for (int i = 0; i < 4; ++i) {
        if (a.parts[i] != b.parts[i]) return a.parts[i] < b.parts[i] ? -1 : 1;
    }
    return 0;
}

} // namespace

std::optional<std::wstring> find_latest_discord_app_dir() {
    std::wstring root = discord_root();
    if (root.empty()) return std::nullopt;

    std::wstring search = path_join(root, std::wstring(APP_DIR_PREFIX) + L"*");

    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(search.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;

    SemVer best{};
    std::wstring best_name;
    bool any = false;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) continue;
        SemVer v{};
        if (!parse_semver_after_prefix(fd.cFileName, APP_DIR_PREFIX, v)) continue;
        if (!any || compare_semver(v, best) > 0) {
            best = v;
            best_name = fd.cFileName;
            any = true;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    if (!any) return std::nullopt;
    return path_join(root, best_name);
}

std::wstring asar_path_in_app_dir(const std::wstring& app_dir) {
    return path_join(app_dir, ASAR_RELPATH);
}

bool is_patched(const std::wstring& asar_path) {
    Asar a;
    if (!a.load(asar_path)) return false;
    return a.has_sentinel(SENTINEL_VERSION);
}

namespace {

struct ProcInfo { DWORD pid; DWORD ppid; std::wstring exe_name; };

std::vector<ProcInfo> snapshot_processes() {
    std::vector<ProcInfo> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;

    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            out.push_back({pe.th32ProcessID, pe.th32ParentProcessID, pe.szExeFile});
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

bool iequals(const std::wstring& a, const std::wstring& b) {
    return a.size() == b.size() && _wcsicmp(a.c_str(), b.c_str()) == 0;
}

struct EnumWinCtx { DWORD pid; std::vector<HWND>* out; };

BOOL CALLBACK enum_window_proc(HWND hwnd, LPARAM lparam) {
    EnumWinCtx* ctx = (EnumWinCtx*)lparam;
    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == ctx->pid) ctx->out->push_back(hwnd);
    return TRUE;
}

void post_close_to_pid(DWORD pid) {
    std::vector<HWND> wins;
    EnumWinCtx c{pid, &wins};
    EnumWindows(enum_window_proc, (LPARAM)&c);
    for (HWND h : wins) {
        PostMessageW(h, WM_CLOSE, 0, 0);
    }
}

bool is_alive(DWORD pid) {
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return false;
    DWORD r = WaitForSingleObject(h, 0);
    CloseHandle(h);
    return r == WAIT_TIMEOUT;
}

void force_kill(DWORD pid) {
    HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid);
    if (!h) return;
    TerminateProcess(h, 0);
    WaitForSingleObject(h, KILL_FORCEFUL_WAIT_MS);
    CloseHandle(h);
}

void collect_descendants(const std::vector<ProcInfo>& procs, DWORD root, std::unordered_set<DWORD>& out) {
    bool changed = true;
    out.insert(root);
    while (changed) {
        changed = false;
        for (const auto& p : procs) {
            if (out.count(p.pid)) continue;
            if (out.count(p.ppid)) { out.insert(p.pid); changed = true; }
        }
    }
}

} // namespace

bool kill_discord_processes() {
    auto procs = snapshot_processes();
    std::vector<DWORD> roots;
    for (const auto& p : procs) {
        if (iequals(p.exe_name, DISCORD_PROC_NAME)) roots.push_back(p.pid);
    }
    if (roots.empty()) return true;

    for (DWORD pid : roots) post_close_to_pid(pid);
    Sleep(KILL_GRACEFUL_WAIT_MS);

    procs = snapshot_processes();

    std::unordered_set<DWORD> tree;
    for (DWORD r : roots) {
        for (const auto& p : procs) {
            if (p.pid == r && iequals(p.exe_name, DISCORD_PROC_NAME)) {
                collect_descendants(procs, r, tree);
                break;
            }
        }
    }
    for (const auto& p : procs) {
        if (iequals(p.exe_name, DISCORD_PROC_NAME)) tree.insert(p.pid);
    }

    for (DWORD pid : tree) {
        if (is_alive(pid)) force_kill(pid);
    }
    return true;
}

bool launch_discord() {
    std::wstring updater = path_join(discord_root(), DISCORD_UPDATE_EXE);
    if (!file_exists(updater)) {
        LOG_ERR("Update.exe not found: %ls", updater.c_str());
        return false;
    }
    HINSTANCE r = ShellExecuteW(nullptr, L"open", updater.c_str(),
                                L"--processStart Discord.exe", nullptr, SW_SHOWNORMAL);
    return (INT_PTR)r > 32;
}

bool ensure_payload_files_extracted() {
    std::wstring dir = install_dir();
    if (!ensure_directory(dir)) return false;

    // Required shims -- the nitro patcher does not work without these.
    struct Item { int id; const char* fname; };
    Item required[] = {
        {IDR_SHIM_MAIN,     SHIM_MAIN_FILE},
        {IDR_SHIM_RENDERER, SHIM_REND_FILE},
    };
    for (const auto& it : required) {
        auto data = load_resource(it.id);
        if (!data) {
            LOG_ERR("Embedded resource %d missing", it.id);
            return false;
        }
        std::wstring out = path_join(dir, utf8_to_wide(it.fname));
        if (!write_file(out, *data)) {
            LOG_ERR("Failed writing %ls", out.c_str());
            return false;
        }
    }

    // Optional native runtime. The resource is only present in the binary
    // when addon/build_addon.bat staged res/embedded/discord_voice_codec.node
    // before rc.exe ran. Absence is non-fatal -- shim_main.js notices the
    // missing file and continues in nitro-only mode. A zero-byte resource
    // (rc.exe placeholder) is treated the same as missing.
    auto addon = load_resource(IDR_RC_ADDON);
    bool addon_present = (addon && !addon->empty());
    if (addon_present) {
        std::wstring out = path_join(dir, utf8_to_wide(RC_ADDON_FILE));
        if (!write_file(out, *addon)) {
            // Log + keep going. The user can still get a fully functional
            // nitro patch without the runtime.
            LOG_ERR("Failed writing %ls (continuing nitro-only)", out.c_str());
            addon_present = false;
        }
    }

    // Auto-create the runtime flag file. shim_main.js gates loading on the
    // file's existence; auto-creating it on install means a fresh patch
    // lights up the runtime out of the box. We only drop the file if it
    // isn't already there so a user who removed it to disable the runtime
    // doesn't get overridden on every relaunch.
    if (addon_present) {
        std::wstring flag = path_join(dir, utf8_to_wide(RC_FLAG_FILE));
        if (GetFileAttributesW(flag.c_str()) == INVALID_FILE_ATTRIBUTES) {
            std::vector<uint8_t> body(
                RC_FLAG_DEFAULT_BODY,
                RC_FLAG_DEFAULT_BODY + strlen(RC_FLAG_DEFAULT_BODY));
            if (!write_file(flag, body)) {
                LOG_ERR("Failed writing %ls", flag.c_str());
            }
        }
    }
    return true;
}

namespace {

std::string strip_dot_slash(const std::string& s) {
    if (s.size() >= 2 && s[0] == '.' && (s[1] == '/' || s[1] == '\\')) return s.substr(2);
    return s;
}

} // namespace

// Definition of the global force flag. Declared in patcher.h.
bool g_force_repatch = false;

bool apply_patch(const std::wstring& app_dir, bool force_repatch) {
    std::wstring asar = asar_path_in_app_dir(app_dir);
    std::wstring asar_new = path_join(app_dir, ASAR_NEW_RELPATH);
    std::wstring asar_bak = path_join(app_dir, ASAR_BAK_RELPATH);

    if (!wait_for_file_unlocked(asar, FILE_LOCK_MAX_WAIT_MS)) {
        LOG_ERR("app.asar locked, giving up");
        return false;
    }

    Asar a;
    if (!a.load(asar)) {
        LOG_ERR("Failed to parse app.asar at %ls", asar.c_str());
        return false;
    }

    // Sentinel match means we'd skip without re-touching anything. Force
    // mode rolls back to the backup so we re-patch on a clean asar with
    // the latest embedded payload.
    if (a.has_sentinel(SENTINEL_VERSION)) {
        if (!force_repatch) {
            LOG_INFO("Already patched (sentinel present), skipping.");
            return true;
        }
        LOG_INFO("Force re-patch -- restoring backup and re-applying.");
        if (!restore_backup(app_dir)) {
            LOG_ERR("force_repatch: restore_backup failed");
            return false;
        }
        // Re-load the freshly-restored asar.
        a = Asar();
        if (!a.load(asar)) {
            LOG_ERR("force_repatch: reload after restore failed");
            return false;
        }
    }

    auto pkg_opt = a.read_file("package.json");
    if (!pkg_opt) { LOG_ERR("package.json missing in asar"); return false; }
    Json pkg;
    std::string pkg_text((const char*)pkg_opt->data(), pkg_opt->size());
    if (!pkg.parse(pkg_text)) { LOG_ERR("package.json parse failed"); return false; }
    const Json* main_field = pkg.find("main");
    std::string main_rel = main_field && main_field->is_str() ? main_field->as_str() : "index.js";
    std::string main_rel_clean = strip_dot_slash(main_rel);

    if (!a.has_file(main_rel_clean)) {
        LOG_ERR("Original main file %s not found in asar", main_rel_clean.c_str());
        return false;
    }

    if (!a.rename_file(main_rel_clean, ORIG_MAIN_RENAME)) {
        LOG_ERR("Rename failed");
        return false;
    }

    pkg.set("main", Json::make_str(std::string("./") + LOADER_FILENAME));
    std::string new_pkg = pkg.dump();
    std::vector<uint8_t> new_pkg_bytes(new_pkg.begin(), new_pkg.end());
    if (!a.write_file("package.json", std::move(new_pkg_bytes))) {
        LOG_ERR("write package.json failed");
        return false;
    }

    auto loader_res = load_resource(IDR_DNP_LOADER);
    if (!loader_res) { LOG_ERR("Loader resource missing"); return false; }
    if (!a.write_file(LOADER_FILENAME, std::move(*loader_res))) {
        LOG_ERR("write loader failed");
        return false;
    }

    a.write_sentinel(SENTINEL_VERSION);

    if (!a.save(asar_new)) {
        LOG_ERR("save .new failed");
        remove_file(asar_new);
        return false;
    }
    {
        Asar verify;
        if (!verify.load(asar_new) || !verify.has_sentinel(SENTINEL_VERSION) ||
            !verify.has_file(LOADER_FILENAME) || !verify.has_file(ORIG_MAIN_RENAME)) {
            LOG_ERR(".new failed re-verify");
            remove_file(asar_new);
            return false;
        }
        auto pkg_v = verify.read_file("package.json");
        if (!pkg_v) { remove_file(asar_new); LOG_ERR("verify pkg read"); return false; }
        Json pj;
        std::string pjt((const char*)pkg_v->data(), pkg_v->size());
        if (!pj.parse(pjt)) { remove_file(asar_new); LOG_ERR("verify pkg parse"); return false; }
        const Json* mf = pj.find("main");
        if (!mf || !mf->is_str() || mf->as_str().find(LOADER_FILENAME) == std::string::npos) {
            remove_file(asar_new);
            LOG_ERR("verify main rewrite missing");
            return false;
        }
    }

    BOOL ok = ReplaceFileW(asar.c_str(), asar_new.c_str(), asar_bak.c_str(),
                           REPLACEFILE_WRITE_THROUGH, nullptr, nullptr);
    if (!ok) {
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND) {
            if (!MoveFileExW(asar_new.c_str(), asar.c_str(),
                             MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                LOG_ERR("MoveFileEx fallback failed: %lu", GetLastError());
                remove_file(asar_new);
                return false;
            }
        } else {
            LOG_ERR("ReplaceFileW failed: %lu", e);
            remove_file(asar_new);
            return false;
        }
    }

    {
        Asar final_check;
        if (!final_check.load(asar) || !final_check.has_sentinel(SENTINEL_VERSION)) {
            LOG_ERR("Final verify failed; restoring backup");
            restore_backup(app_dir);
            return false;
        }
    }

    LOG_INFO("Patch applied: %ls", asar.c_str());
    return true;
}

bool restore_backup(const std::wstring& app_dir) {
    std::wstring asar = asar_path_in_app_dir(app_dir);
    std::wstring asar_bak = path_join(app_dir, ASAR_BAK_RELPATH);
    if (!file_exists(asar_bak)) return false;
    BOOL ok = MoveFileExW(asar_bak.c_str(), asar.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    if (!ok) {
        LOG_ERR("Restore failed: %lu", GetLastError());
        return false;
    }
    LOG_INFO("Restored backup at %ls", asar.c_str());
    return true;
}

} // namespace dnp
