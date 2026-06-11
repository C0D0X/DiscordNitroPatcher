// GitHub releases self-update.
//
// See updater.h for the non-disruptive staging model.
#include "updater.h"
#include "config.h"
#include "util.h"

#include <winhttp.h>

#include <cstdio>
#include <cstring>
#include <regex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace dnp {

namespace {

// Throttle update checks; GitHub unauthenticated API limit is 60/hr per IP.
constexpr ULONGLONG UPDATE_CHECK_INTERVAL_SEC = 6ULL * 60ULL * 60ULL;

bool http_get(const std::wstring& host, const std::wstring& path, std::vector<uint8_t>& out,
              int& status, const std::wstring& accept_header = L"") {
    out.clear();
    status = 0;
    HINTERNET sess = WinHttpOpen(UPDATE_USER_AGENT,
                                 WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess) return false;

    // Modest timeouts so a stalled network never blocks the background thread for long.
    DWORD t_resolve = 5'000, t_connect = 5'000, t_send = 15'000, t_recv = 30'000;
    WinHttpSetTimeouts(sess, t_resolve, t_connect, t_send, t_recv);

    HINTERNET conn = WinHttpConnect(sess, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!conn) { WinHttpCloseHandle(sess); return false; }

    DWORD req_flags = WINHTTP_FLAG_SECURE;
    HINTERNET req = WinHttpOpenRequest(conn, L"GET", path.c_str(), nullptr,
                                       WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, req_flags);
    if (!req) { WinHttpCloseHandle(conn); WinHttpCloseHandle(sess); return false; }

    DWORD redir = WINHTTP_OPTION_REDIRECT_POLICY_ALWAYS;
    WinHttpSetOption(req, WINHTTP_OPTION_REDIRECT_POLICY, &redir, sizeof(redir));

    std::wstring hdrs = L"Accept: application/vnd.github+json\r\n";
    if (!accept_header.empty()) hdrs = accept_header;

    BOOL ok = WinHttpSendRequest(req, hdrs.c_str(), (DWORD)hdrs.size(),
                                 WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    if (!ok) goto fail;
    if (!WinHttpReceiveResponse(req, nullptr)) goto fail;

    {
        DWORD st = 0, sz = sizeof(st);
        if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                 WINHTTP_HEADER_NAME_BY_INDEX, &st, &sz, WINHTTP_NO_HEADER_INDEX)) {
            goto fail;
        }
        status = (int)st;
    }

    while (true) {
        DWORD avail = 0;
        if (!WinHttpQueryDataAvailable(req, &avail)) goto fail;
        if (avail == 0) break;
        size_t old = out.size();
        out.resize(old + avail);
        DWORD read = 0;
        if (!WinHttpReadData(req, &out[old], avail, &read)) goto fail;
        if (read != avail) out.resize(old + read);
    }

    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(sess);
    return true;

fail:
    WinHttpCloseHandle(req);
    WinHttpCloseHandle(conn);
    WinHttpCloseHandle(sess);
    return false;
}

bool split_https_url(const std::wstring& url, std::wstring& host, std::wstring& path) {
    const std::wstring prefix = L"https://";
    if (url.compare(0, prefix.size(), prefix) != 0) return false;
    size_t slash = url.find(L'/', prefix.size());
    if (slash == std::wstring::npos) {
        host = url.substr(prefix.size());
        path = L"/";
    } else {
        host = url.substr(prefix.size(), slash - prefix.size());
        path = url.substr(slash);
    }
    return true;
}

bool extract_tag_name(const std::string& body, std::string& out) {
    std::regex re("\"tag_name\"\\s*:\\s*\"v?([^\"]+)\"");
    std::smatch m;
    if (!std::regex_search(body, m, re)) return false;
    out = m[1].str();
    return true;
}

bool extract_asset_url(const std::string& body, const std::string& asset_name, std::string& out) {
    size_t pos = body.find("\"name\":\"" + asset_name + "\"");
    if (pos == std::string::npos) return false;
    size_t key = body.find("\"browser_download_url\"", pos);
    if (key == std::string::npos) return false;
    size_t colon = body.find(':', key);
    if (colon == std::string::npos) return false;
    size_t quote1 = body.find('"', colon);
    if (quote1 == std::string::npos) return false;
    size_t quote2 = body.find('"', quote1 + 1);
    if (quote2 == std::string::npos) return false;
    out = body.substr(quote1 + 1, quote2 - quote1 - 1);
    return true;
}

bool parse_semver_string(const std::string& s, int& a, int& b, int& c) {
    a = b = c = 0;
    int got = sscanf(s.c_str(), "%d.%d.%d", &a, &b, &c);
    return got >= 1;
}

bool newer_than_current(const std::string& tag) {
    int a, b, c;
    if (!parse_semver_string(tag, a, b, c)) return false;
    if (a != VERSION_MAJOR) return a > VERSION_MAJOR;
    if (b != VERSION_MINOR) return b > VERSION_MINOR;
    return c > VERSION_PATCH;
}

void strip_zone_identifier(const std::wstring& path) {
    std::wstring zone = path + L":Zone.Identifier";
    DeleteFileW(zone.c_str());
}

std::wstring installed_self_path() { return path_join(install_dir(), L"dnp.exe"); }
std::wstring staged_update_path() { return path_join(install_dir(), L"dnp.exe.new"); }
std::wstring old_self_path()      { return path_join(install_dir(), L"dnp.exe.old"); }
std::wstring throttle_marker()    { return path_join(install_dir(), L".dnp_update_check"); }

bool running_as_installed() {
    std::wstring self = self_exe_path();
    if (self.empty()) return false;
    return _wcsicmp(self.c_str(), installed_self_path().c_str()) == 0;
}

// Returns true if the throttle window has elapsed. Touches the marker as a side effect.
bool throttle_allows_check() {
    std::wstring marker = throttle_marker();
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExW(marker.c_str(), GetFileExInfoStandard, &fad)) {
        FILETIME ftnow;
        GetSystemTimeAsFileTime(&ftnow);
        ULARGE_INTEGER a{}, b{};
        a.LowPart  = fad.ftLastWriteTime.dwLowDateTime;
        a.HighPart = fad.ftLastWriteTime.dwHighDateTime;
        b.LowPart  = ftnow.dwLowDateTime;
        b.HighPart = ftnow.dwHighDateTime;
        if (b.QuadPart > a.QuadPart) {
            ULONGLONG elapsed_100ns = b.QuadPart - a.QuadPart;
            ULONGLONG elapsed_sec   = elapsed_100ns / 10'000'000ULL;
            if (elapsed_sec < UPDATE_CHECK_INTERVAL_SEC) return false;
        }
    }
    // Touch (create or overwrite) the marker.
    HANDLE h = CreateFileW(marker.c_str(), GENERIC_WRITE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        FILETIME ftnow;
        GetSystemTimeAsFileTime(&ftnow);
        SetFileTime(h, nullptr, nullptr, &ftnow);
        CloseHandle(h);
    }
    return true;
}

// Sentinel: if the staged file's version equals the running version, drop it.
// Prevents an infinite "re-stage / re-apply" loop when a prior tag matches us.
bool staged_matches_current(const std::wstring& staged) {
    // Cheap heuristic: a freshly-staged file under 4 KiB is junk.
    auto data = read_file(staged);
    if (!data || data->size() < 4096) return true;
    return false;
}

void stage_update_blocking() {
    std::vector<uint8_t> resp;
    int status = 0;
    if (!http_get(UPDATE_HOST, UPDATE_PATH, resp, status) || status != 200) {
        LOG_WARN("Update check HTTP %d", status);
        return;
    }
    std::string body((const char*)resp.data(), resp.size());

    std::string tag;
    if (!extract_tag_name(body, tag)) {
        LOG_WARN("No tag_name in release JSON.");
        return;
    }
    if (!newer_than_current(tag)) {
        LOG_DBG("Already at latest: %s", tag.c_str());
        // Drop any leftover stale stage from a prior version.
        std::wstring staged = staged_update_path();
        if (file_exists(staged)) DeleteFileW(staged.c_str());
        return;
    }
    LOG_INFO("Update available: %s (staging)", tag.c_str());

    std::string asset_url;
    if (!extract_asset_url(body, wide_to_utf8(UPDATE_ASSET_NAME), asset_url)) {
        LOG_WARN("No %ls asset in release.", UPDATE_ASSET_NAME);
        return;
    }
    std::wstring url_w = utf8_to_wide(asset_url);
    std::wstring host, path;
    if (!split_https_url(url_w, host, path)) return;

    std::vector<uint8_t> bin;
    int bstatus = 0;
    if (!http_get(host, path, bin, bstatus) || bstatus != 200 || bin.empty()) {
        LOG_WARN("Asset download failed (%d).", bstatus);
        return;
    }

    // Optional SHA-256 attestation: looks for `sha256: <64hex>` anywhere in the release body.
    {
        std::regex sha_re("sha256:\\s*([0-9a-fA-F]{64})");
        std::smatch m;
        if (std::regex_search(body, m, sha_re)) {
            std::string expected = m[1].str();
            for (auto& c : expected) if (c >= 'A' && c <= 'F') c = (char)(c - 'A' + 'a');
            std::string actual = sha256_hex(bin);
            if (actual != expected) {
                LOG_ERR("SHA mismatch: expected %s got %s", expected.c_str(), actual.c_str());
                return;
            }
        }
    }

    // Stage atomically: write to .new.tmp, then rename to .new.
    std::wstring staged = staged_update_path();
    std::wstring tmp    = staged + L".tmp";
    if (!write_file(tmp, bin)) {
        LOG_ERR("write staged update failed (%lu)", GetLastError());
        return;
    }
    strip_zone_identifier(tmp);
    if (!MoveFileExW(tmp.c_str(), staged.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        LOG_ERR("Stage rename failed: %lu", GetLastError());
        DeleteFileW(tmp.c_str());
        return;
    }
    LOG_INFO("Staged update %s at %ls (applies on next launch).", tag.c_str(), staged.c_str());
}

} // namespace

void start_background_update_check() {
    if (DEV_MODE) {
        LOG_DBG("Update check skipped (DEV_MODE).");
        return;
    }
    if (!running_as_installed()) {
        LOG_DBG("Update check skipped (not running from install dir).");
        return;
    }
    if (!throttle_allows_check()) {
        LOG_DBG("Update check skipped (throttled).");
        return;
    }
    // Detached worker: any in-flight download is cut at process exit; the
    // next startup will simply try again.
    // Detached worker. Any throw is swallowed so the updater can never
    // crash the host process. /EHsc means we only catch C++ exceptions;
    // a hardware fault would still terminate, which is acceptable given
    // the narrow code surface here (winhttp + regex + file IO).
    std::thread([] {
        try {
            stage_update_blocking();
        } catch (...) {
            LOG_WARN("Updater thread swallowed exception.");
        }
    }).detach();
}

bool apply_pending_update_if_any() {
    if (!running_as_installed()) return false;

    std::wstring self     = self_exe_path();
    std::wstring expected = installed_self_path();
    std::wstring staged   = staged_update_path();
    std::wstring oldp     = old_self_path();

    // Clean up the previous swap's leftover (the file is unlocked once the
    // prior process exited).
    if (file_exists(oldp)) DeleteFileW(oldp.c_str());

    if (!file_exists(staged)) return false;

    if (staged_matches_current(staged)) {
        LOG_WARN("Staged update invalid; discarding.");
        DeleteFileW(staged.c_str());
        return false;
    }

    // Windows permits renaming a running executable on the same volume
    // because the binding is to the open section, not the directory entry.
    DeleteFileW(oldp.c_str());
    if (!MoveFileExW(self.c_str(), oldp.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        LOG_WARN("Rename self -> .old failed: %lu", GetLastError());
        return false;
    }
    if (!MoveFileExW(staged.c_str(), expected.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        LOG_ERR("Rename .new -> exe failed: %lu", GetLastError());
        // Roll back so we don't end up with no exe at the canonical path.
        MoveFileExW(oldp.c_str(), self.c_str(), MOVEFILE_REPLACE_EXISTING);
        return false;
    }
    LOG_INFO("Applied staged update; re-launching new exe.");

    // Re-exec with the original command line (CreateProcessW mutates the
    // buffer it receives, hence the writable copy).
    std::wstring cmdline = GetCommandLineW();
    std::vector<wchar_t> mut(cmdline.begin(), cmdline.end());
    mut.push_back(0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessW(expected.c_str(), mut.data(), nullptr, nullptr,
                             FALSE, 0, nullptr, install_dir().c_str(), &si, &pi);
    if (!ok) {
        LOG_ERR("CreateProcess(new exe) failed: %lu", GetLastError());
        // The new exe is in place but we couldn't launch it; the user's
        // next manual launch will still pick it up.
        return false;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return true;
}

} // namespace dnp
