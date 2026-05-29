// helpers: paths, logging, io, etc
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace dnp {

std::wstring local_app_data();
std::wstring install_dir();
std::wstring discord_root();
std::wstring path_join(std::wstring a, std::wstring b);
bool         ensure_directory(const std::wstring& dir);
bool         file_exists(const std::wstring& path);
bool         remove_file(const std::wstring& path);
bool         remove_directory_recursive(const std::wstring& dir);

std::optional<std::vector<uint8_t>> read_file(const std::wstring& path);
bool write_file(const std::wstring& path, const void* data, size_t size);
bool write_file(const std::wstring& path, const std::vector<uint8_t>& data);
bool write_file(const std::wstring& path, const std::string& data);
bool wait_for_file_unlocked(const std::wstring& path, int max_wait_ms);

std::wstring utf8_to_wide(const std::string& s);
std::string  wide_to_utf8(const std::wstring& s);

std::string sha256_hex(const void* data, size_t size);
std::string sha256_hex(const std::vector<uint8_t>& data);

std::optional<std::vector<uint8_t>> load_resource(int resource_id);

int run_command(const std::wstring& cmdline, bool wait, bool show_window);
std::wstring self_exe_path();
int kill_processes_by_name_except_self(const std::wstring& exe_name);
HANDLE acquire_single_instance_mutex(const wchar_t* name);
void log_init();
void log_shutdown();
void log_write(int level, const char* fmt, ...);

#define LOG_ERR(...)  ::dnp::log_write(0, __VA_ARGS__)
#define LOG_WARN(...) ::dnp::log_write(1, __VA_ARGS__)
#define LOG_INFO(...) ::dnp::log_write(2, __VA_ARGS__)
#define LOG_DBG(...)  ::dnp::log_write(3, __VA_ARGS__)

} // namespace dnp
