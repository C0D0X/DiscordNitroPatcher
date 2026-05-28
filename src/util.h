// util.h — shared utilities: paths, logging, IO, hashing, encoding, resources.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dnp {

// ---- Path helpers ----------------------------------------------------------
std::wstring local_app_data();                       // %LOCALAPPDATA%
std::wstring install_dir();                          // %LOCALAPPDATA%\dnp
std::wstring discord_root();                         // %LOCALAPPDATA%\Discord
std::wstring path_join(std::wstring a, std::wstring b);
bool         ensure_directory(const std::wstring& dir);
bool         file_exists(const std::wstring& path);
bool         remove_file(const std::wstring& path);
bool         remove_directory_recursive(const std::wstring& dir);

// ---- File IO ---------------------------------------------------------------
std::optional<std::vector<uint8_t>> read_file(const std::wstring& path);
bool write_file(const std::wstring& path, const void* data, size_t size);
bool write_file(const std::wstring& path, const std::vector<uint8_t>& data);
bool write_file(const std::wstring& path, const std::string& data);
bool wait_for_file_unlocked(const std::wstring& path, int max_wait_ms);

// ---- Encoding --------------------------------------------------------------
std::wstring utf8_to_wide(const std::string& s);
std::string  wide_to_utf8(const std::wstring& s);

// ---- Hashing ---------------------------------------------------------------
// SHA-256 via bcrypt. Returns lowercase hex string on success, empty on failure.
std::string sha256_hex(const void* data, size_t size);
std::string sha256_hex(const std::vector<uint8_t>& data);

// ---- Embedded resource extraction -----------------------------------------
// Loads a resource embedded as RT_RCDATA. Returns raw bytes, no decoding.
std::optional<std::vector<uint8_t>> load_resource(int resource_id);

// ---- Process / launch ------------------------------------------------------
// Spawn a process and optionally wait for exit. Returns exit code or -1.
int run_command(const std::wstring& cmdline, bool wait, bool show_window);
std::wstring self_exe_path();

// Terminate every process with the given image name (case-insensitive) except the current
// process. Returns number of processes terminated.
int kill_processes_by_name_except_self(const std::wstring& exe_name);

// Acquire a per-user named single-instance mutex. Returns:
//   - non-null handle if we are the first/only instance (caller must keep handle alive)
//   - nullptr if another instance already holds it
// Caller is responsible for CloseHandle on the returned handle at shutdown.
HANDLE acquire_single_instance_mutex(const wchar_t* name);

// ---- Logging ---------------------------------------------------------------
// Levels: 0=error, 1=warn, 2=info, 3=debug.
// In DEV_MODE: stdout + file. Otherwise: file only and only level <= 1.
void log_init();
void log_shutdown();
void log_write(int level, const char* fmt, ...);

#define LOG_ERR(...)  ::dnp::log_write(0, __VA_ARGS__)
#define LOG_WARN(...) ::dnp::log_write(1, __VA_ARGS__)
#define LOG_INFO(...) ::dnp::log_write(2, __VA_ARGS__)
#define LOG_DBG(...)  ::dnp::log_write(3, __VA_ARGS__)

} // namespace dnp
