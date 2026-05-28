// util.cpp — implementations of shared utilities.
#include "util.h"
#include "config.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <shellapi.h>
#include <bcrypt.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "bcrypt.lib")

namespace dnp {

// ============================================================================
// Path helpers
// ============================================================================

std::wstring local_app_data() {
    PWSTR p = nullptr;
    if (SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p) != S_OK || !p) {
        return L"";
    }
    std::wstring out(p);
    CoTaskMemFree(p);
    return out;
}

std::wstring install_dir() {
    return path_join(local_app_data(), INSTALL_SUBDIR);
}

std::wstring discord_root() {
    return path_join(local_app_data(), DISCORD_SUBDIR);
}

std::wstring path_join(std::wstring a, std::wstring b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    if (a.back() != L'\\' && a.back() != L'/') a.push_back(L'\\');
    if (!b.empty() && (b.front() == L'\\' || b.front() == L'/')) b.erase(0, 1);
    a.append(b);
    return a;
}

bool ensure_directory(const std::wstring& dir) {
    if (dir.empty()) return false;
    // SHCreateDirectoryExW creates intermediate dirs.
    int rc = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return rc == ERROR_SUCCESS || rc == ERROR_ALREADY_EXISTS || rc == ERROR_FILE_EXISTS;
}

bool file_exists(const std::wstring& path) {
    DWORD a = GetFileAttributesW(path.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

bool remove_file(const std::wstring& path) {
    if (!file_exists(path)) return true;
    // Clear read-only just in case.
    DWORD a = GetFileAttributesW(path.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(path.c_str(), a & ~FILE_ATTRIBUTE_READONLY);
    }
    return DeleteFileW(path.c_str()) != 0;
}

bool remove_directory_recursive(const std::wstring& dir) {
    if (dir.empty()) return false;
    // SHFileOperationW with FO_DELETE + FOF_NO_UI is the simplest reliable recursive delete.
    // Buffer must be double-null-terminated.
    std::wstring buf = dir;
    buf.push_back(L'\0');
    buf.push_back(L'\0');
    SHFILEOPSTRUCTW op = {};
    op.wFunc  = FO_DELETE;
    op.pFrom  = buf.c_str();
    op.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    return SHFileOperationW(&op) == 0;
}

// ============================================================================
// File IO
// ============================================================================

std::optional<std::vector<uint8_t>> read_file(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;

    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(h, &sz)) { CloseHandle(h); return std::nullopt; }
    if (sz.QuadPart < 0 || sz.QuadPart > (LONGLONG)0x7FFFFFFF) {
        CloseHandle(h);
        return std::nullopt;
    }

    std::vector<uint8_t> out(static_cast<size_t>(sz.QuadPart));
    DWORD read = 0;
    if (out.size() > 0) {
        if (!ReadFile(h, out.data(), (DWORD)out.size(), &read, nullptr) || read != out.size()) {
            CloseHandle(h);
            return std::nullopt;
        }
    }
    CloseHandle(h);
    return out;
}

bool write_file(const std::wstring& path, const void* data, size_t size) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD wrote = 0;
    bool ok = true;
    if (size > 0) {
        if (!WriteFile(h, data, (DWORD)size, &wrote, nullptr) || wrote != size) ok = false;
    }
    FlushFileBuffers(h);
    CloseHandle(h);
    return ok;
}

bool write_file(const std::wstring& path, const std::vector<uint8_t>& data) {
    return write_file(path, data.data(), data.size());
}

bool write_file(const std::wstring& path, const std::string& data) {
    return write_file(path, data.data(), data.size());
}

bool wait_for_file_unlocked(const std::wstring& path, int max_wait_ms) {
    int waited = 0;
    while (waited <= max_wait_ms) {
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                               nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return true; }
        DWORD e = GetLastError();
        if (e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND) return true;
        Sleep(FILE_LOCK_RETRY_MS);
        waited += FILE_LOCK_RETRY_MS;
    }
    return false;
}

// ============================================================================
// Encoding
// ============================================================================

std::wstring utf8_to_wide(const std::string& s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    if (n <= 0) return L"";
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::string wide_to_utf8(const std::wstring& s) {
    if (s.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

// ============================================================================
// SHA-256 via bcrypt
// ============================================================================

std::string sha256_hex(const void* data, size_t size) {
    BCRYPT_ALG_HANDLE alg = nullptr;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return "";

    DWORD obj_size = 0, cb = 0, hash_len = 0;
    BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&obj_size, sizeof(obj_size), &cb, 0);
    BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&hash_len, sizeof(hash_len), &cb, 0);

    std::vector<uint8_t> obj(obj_size);
    std::vector<uint8_t> digest(hash_len);

    BCRYPT_HASH_HANDLE hh = nullptr;
    if (BCryptCreateHash(alg, &hh, obj.data(), obj_size, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(alg, 0); return "";
    }
    if (size > 0) {
        if (BCryptHashData(hh, (PUCHAR)data, (ULONG)size, 0) < 0) {
            BCryptDestroyHash(hh);
            BCryptCloseAlgorithmProvider(alg, 0);
            return "";
        }
    }
    if (BCryptFinishHash(hh, digest.data(), hash_len, 0) < 0) {
        BCryptDestroyHash(hh);
        BCryptCloseAlgorithmProvider(alg, 0);
        return "";
    }
    BCryptDestroyHash(hh);
    BCryptCloseAlgorithmProvider(alg, 0);

    static const char hex[] = "0123456789abcdef";
    std::string out(hash_len * 2, '0');
    for (DWORD i = 0; i < hash_len; ++i) {
        out[i * 2 + 0] = hex[digest[i] >> 4];
        out[i * 2 + 1] = hex[digest[i] & 0xF];
    }
    return out;
}

std::string sha256_hex(const std::vector<uint8_t>& data) {
    return sha256_hex(data.data(), data.size());
}

// ============================================================================
// Embedded resource extraction
// ============================================================================

std::optional<std::vector<uint8_t>> load_resource(int resource_id) {
    HMODULE mod = GetModuleHandleW(nullptr);
    // RT_RCDATA expands to MAKEINTRESOURCE(10) which is ANSI when UNICODE isn't defined.
    // FindResourceW requires LPCWSTR for the type parameter, so spell out MAKEINTRESOURCEW(10).
    HRSRC res = FindResourceW(mod, MAKEINTRESOURCEW(resource_id), MAKEINTRESOURCEW(10));
    if (!res) return std::nullopt;
    DWORD size = SizeofResource(mod, res);
    HGLOBAL hg = LoadResource(mod, res);
    if (!hg || size == 0) return std::nullopt;
    void* p = LockResource(hg);
    if (!p) return std::nullopt;
    return std::vector<uint8_t>((uint8_t*)p, (uint8_t*)p + size);
}

// ============================================================================
// Process spawn
// ============================================================================

int run_command(const std::wstring& cmdline, bool wait, bool show_window) {
    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    si.dwFlags    = STARTF_USESHOWWINDOW;
    si.wShowWindow = show_window ? SW_SHOWNORMAL : SW_HIDE;

    PROCESS_INFORMATION pi = {};
    std::wstring buf = cmdline; // CreateProcessW may modify
    DWORD flags = CREATE_UNICODE_ENVIRONMENT;
    if (!show_window) flags |= CREATE_NO_WINDOW;

    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        flags, nullptr, nullptr, &si, &pi)) {
        return -1;
    }
    int code = 0;
    if (wait) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD ec = 0;
        GetExitCodeProcess(pi.hProcess, &ec);
        code = (int)ec;
    }
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return code;
}

std::wstring self_exe_path() {
    wchar_t buf[MAX_PATH * 2] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)(sizeof(buf) / sizeof(buf[0])));
    if (n == 0 || n >= sizeof(buf) / sizeof(buf[0])) return L"";
    return std::wstring(buf, n);
}

// ============================================================================
// Logging
// ============================================================================

namespace {
HANDLE g_log_file = INVALID_HANDLE_VALUE;
HANDLE g_log_console = INVALID_HANDLE_VALUE;
CRITICAL_SECTION g_log_cs;
bool g_log_inited = false;

void rotate_if_needed() {
    if (g_log_file == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz{};
    if (!GetFileSizeEx(g_log_file, &sz)) return;
    if ((size_t)sz.QuadPart < LOG_MAX_BYTES) return;
    // Truncate to keep only second half — preserve recent.
    LONG zero = 0;
    LARGE_INTEGER half;
    half.QuadPart = sz.QuadPart / 2;
    std::vector<char> buf((size_t)(sz.QuadPart - half.QuadPart));
    SetFilePointerEx(g_log_file, half, nullptr, FILE_BEGIN);
    DWORD read = 0;
    ReadFile(g_log_file, buf.data(), (DWORD)buf.size(), &read, nullptr);
    LARGE_INTEGER z{};
    SetFilePointerEx(g_log_file, z, nullptr, FILE_BEGIN);
    SetEndOfFile(g_log_file);
    DWORD wrote = 0;
    WriteFile(g_log_file, buf.data(), read, &wrote, nullptr);
}
} // namespace

void log_init() {
    if (g_log_inited) return;
    InitializeCriticalSection(&g_log_cs);
    g_log_inited = true;

    std::wstring dir = install_dir();
    ensure_directory(dir);
    std::wstring path = path_join(dir, utf8_to_wide(LOG_FILE));
    g_log_file = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

    if (DEV_MODE) {
        if (AllocConsole()) {
            g_log_console = GetStdHandle(STD_OUTPUT_HANDLE);
            SetConsoleOutputCP(CP_UTF8);
            // Tag the console for sanity.
            const char* hdr = "DiscordNitroPatcher (DEV_MODE)\n";
            DWORD w = 0;
            WriteFile(g_log_console, hdr, (DWORD)strlen(hdr), &w, nullptr);
        }
    }
}

void log_shutdown() {
    if (!g_log_inited) return;
    if (g_log_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_log_file);
        g_log_file = INVALID_HANDLE_VALUE;
    }
    DeleteCriticalSection(&g_log_cs);
    g_log_inited = false;
}

void log_write(int level, const char* fmt, ...) {
    if (!DEV_MODE && level > 1) return; // release: errors + warnings only
    if (!g_log_inited) log_init();

    // Format message.
    char msg[2048];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(msg, sizeof(msg) - 2, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(msg) - 2) n = (int)sizeof(msg) - 3;

    // Prefix with timestamp + level.
    SYSTEMTIME st;
    GetLocalTime(&st);
    static const char* tags[] = {"ERR ", "WARN", "INFO", "DBG "};
    const char* tag = tags[level >= 0 && level <= 3 ? level : 2];

    char line[2200];
    int m = snprintf(line, sizeof(line),
        "%04d-%02d-%02d %02d:%02d:%02d [%s] %s\n",
        st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, tag, msg);
    if (m < 0) return;
    if ((size_t)m >= sizeof(line)) m = (int)sizeof(line) - 1;

    EnterCriticalSection(&g_log_cs);
    if (g_log_file != INVALID_HANDLE_VALUE) {
        rotate_if_needed();
        DWORD wrote = 0;
        WriteFile(g_log_file, line, (DWORD)m, &wrote, nullptr);
    }
    if (DEV_MODE && g_log_console != INVALID_HANDLE_VALUE) {
        DWORD wrote = 0;
        WriteFile(g_log_console, line, (DWORD)m, &wrote, nullptr);
    }
    LeaveCriticalSection(&g_log_cs);
}

} // namespace dnp
