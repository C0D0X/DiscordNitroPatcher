// GitHub releases self-update
#include "updater.h"
#include "config.h"
#include "util.h"

#include <winhttp.h>

#include <cstdio>
#include <cstring>
#include <regex>
#include <string>
#include <vector>

#pragma comment(lib, "winhttp.lib")

namespace dnp {

namespace {

bool http_get(const std::wstring& host, const std::wstring& path, std::vector<uint8_t>& out,
              int& status, const std::wstring& accept_header = L"") {
    out.clear();
    status = 0;
    HINTERNET sess = WinHttpOpen(UPDATE_USER_AGENT,
                                 WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                 WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!sess) return false;

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

} // namespace

void check_for_update_and_maybe_restart() {
    if (DEV_MODE) {
        LOG_DBG("Update check skipped (DEV_MODE).");
        return;
    }

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
        return;
    }
    LOG_INFO("Update available: %s", tag.c_str());

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

    wchar_t tmpdir[MAX_PATH];
    DWORD n = GetTempPathW(MAX_PATH, tmpdir);
    if (n == 0 || n >= MAX_PATH) return;
    std::wstring new_exe = path_join(std::wstring(tmpdir, n), L"dnp_new.exe");
    if (!write_file(new_exe, bin)) {
        LOG_ERR("write new exe failed");
        return;
    }
    strip_zone_identifier(new_exe);

    std::wstring self = self_exe_path();
    if (self.empty()) return;

    std::wstring cmd = L"cmd.exe /c timeout /t 2 /nobreak >nul & move /y \"";
    cmd += new_exe;
    cmd += L"\" \"";
    cmd += self;
    cmd += L"\" & start \"\" \"";
    cmd += self;
    cmd += L"\"";

    run_command(cmd, false, false);
    LOG_INFO("Scheduled update swap. Exiting.");
    ExitProcess(0);
}

} // namespace dnp
