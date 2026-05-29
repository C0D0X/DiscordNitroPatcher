// Discord launcher wrapper
#include "shortcuts.h"
#include "config.h"
#include "json_lite.h"
#include "util.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <objbase.h>

#include <cwchar>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

namespace dnp {

namespace {

constexpr const wchar_t* MANIFEST_FILE = L"launchers.json";

struct ComInit {
    bool ok = false;
    ComInit()  { HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); ok = SUCCEEDED(hr); }
    ~ComInit() { if (ok) CoUninitialize(); }
};

std::wstring manifest_path() {
    return path_join(install_dir(), MANIFEST_FILE);
}

bool load_manifest(Json& out) {
    auto buf = read_file(manifest_path());
    if (!buf) { out = Json::make_obj(); return false; }
    std::string text((const char*)buf->data(), buf->size());
    if (!out.parse(text)) { out = Json::make_obj(); return false; }
    return true;
}

bool save_manifest(const Json& m) {
    ensure_directory(install_dir());
    std::string text = m.dump();
    return write_file(manifest_path(), text);
}

std::wstring known_folder(REFKNOWNFOLDERID rfid) {
    PWSTR p = nullptr;
    if (SHGetKnownFolderPath(rfid, 0, nullptr, &p) != S_OK || !p) return L"";
    std::wstring out(p);
    CoTaskMemFree(p);
    return out;
}

bool iends_with(const std::wstring& s, const std::wstring& suffix) {
    if (s.size() < suffix.size()) return false;
    return _wcsicmp(s.c_str() + (s.size() - suffix.size()), suffix.c_str()) == 0;
}

bool icontains(const std::wstring& s, const std::wstring& needle) {
    if (s.empty() || needle.empty()) return false;
    std::wstring a = s, b = needle;
    for (auto& c : a) c = (wchar_t)towlower(c);
    for (auto& c : b) c = (wchar_t)towlower(c);
    return a.find(b) != std::wstring::npos;
}

struct LnkInfo {
    std::wstring target;
    std::wstring args;
    std::wstring workdir;
    bool ok = false;
};

LnkInfo read_lnk(const std::wstring& lnk_path) {
    LnkInfo info{};
    IShellLinkW* psl = nullptr;
    if (CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                         IID_IShellLinkW, (void**)&psl) != S_OK || !psl) return info;
    IPersistFile* ppf = nullptr;
    if (psl->QueryInterface(IID_IPersistFile, (void**)&ppf) != S_OK || !ppf) {
        psl->Release();
        return info;
    }
    if (ppf->Load(lnk_path.c_str(), STGM_READ) == S_OK) {
        wchar_t buf[MAX_PATH * 2] = {};
        WIN32_FIND_DATAW wfd{};
        if (psl->GetPath(buf, MAX_PATH * 2, &wfd, SLGP_RAWPATH) == S_OK) info.target = buf;
        wchar_t args[2048] = {};
        if (psl->GetArguments(args, 2048) == S_OK) info.args = args;
        wchar_t wd[MAX_PATH * 2] = {};
        if (psl->GetWorkingDirectory(wd, MAX_PATH * 2) == S_OK) info.workdir = wd;
        info.ok = true;
    }
    ppf->Release();
    psl->Release();
    return info;
}

bool write_lnk(const std::wstring& lnk_path,
               const std::wstring& target,
               const std::wstring& args,
               const std::wstring& workdir) {
    IShellLinkW* psl = nullptr;
    if (CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                         IID_IShellLinkW, (void**)&psl) != S_OK || !psl) return false;
    IPersistFile* ppf = nullptr;
    if (psl->QueryInterface(IID_IPersistFile, (void**)&ppf) != S_OK || !ppf) {
        psl->Release();
        return false;
    }
    bool ok = false;
    if (ppf->Load(lnk_path.c_str(), STGM_READWRITE) == S_OK) {
        if (psl->SetPath(target.c_str()) == S_OK &&
            psl->SetArguments(args.c_str()) == S_OK &&
            psl->SetWorkingDirectory(workdir.c_str()) == S_OK) {
            if (ppf->Save(lnk_path.c_str(), TRUE) == S_OK) ok = true;
        }
    }
    ppf->Release();
    psl->Release();
    return ok;
}

bool is_discord_launcher_target(const std::wstring& target) {
    if (target.empty()) return false;
    return iends_with(target, L"\\Discord\\Update.exe") && icontains(target, L"\\Discord\\");
}

std::vector<std::wstring> candidate_shortcut_paths() {
    std::vector<std::wstring> out;
    for (REFKNOWNFOLDERID f : {FOLDERID_Desktop, FOLDERID_PublicDesktop,
                               FOLDERID_StartMenu, FOLDERID_CommonStartMenu,
                               FOLDERID_Programs, FOLDERID_CommonPrograms,
                               FOLDERID_Startup, FOLDERID_CommonStartup}) {
        std::wstring base = known_folder(f);
        if (base.empty()) continue;
        out.push_back(path_join(base, L"Discord.lnk"));
        out.push_back(path_join(base, L"Discord Inc\\Discord.lnk"));
    }
    return out;
}

constexpr const wchar_t* RUNKEY_SUBKEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const wchar_t* RUNKEY_VALUE  = L"Discord";

bool read_runkey(std::wstring& out_value) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUNKEY_SUBKEY, 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    DWORD type = 0, size = 0;
    LSTATUS rc = RegQueryValueExW(h, RUNKEY_VALUE, nullptr, &type, nullptr, &size);
    if (rc != ERROR_SUCCESS || type != REG_SZ || size == 0) { RegCloseKey(h); return false; }
    std::vector<wchar_t> buf(size / sizeof(wchar_t) + 1, 0);
    if (RegQueryValueExW(h, RUNKEY_VALUE, nullptr, &type,
                         (BYTE*)buf.data(), &size) != ERROR_SUCCESS) {
        RegCloseKey(h);
        return false;
    }
    RegCloseKey(h);
    out_value = buf.data();
    return true;
}

bool write_runkey(const std::wstring& value) {
    HKEY h = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, RUNKEY_SUBKEY, 0, nullptr, 0,
                        KEY_WRITE, nullptr, &h, nullptr) != ERROR_SUCCESS) return false;
    DWORD bytes = (DWORD)((value.size() + 1) * sizeof(wchar_t));
    LSTATUS rc = RegSetValueExW(h, RUNKEY_VALUE, 0, REG_SZ, (const BYTE*)value.c_str(), bytes);
    RegCloseKey(h);
    return rc == ERROR_SUCCESS;
}

bool delete_runkey() {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUNKEY_SUBKEY, 0, KEY_SET_VALUE, &h) != ERROR_SUCCESS) return false;
    LSTATUS rc = RegDeleteValueW(h, RUNKEY_VALUE);
    RegCloseKey(h);
    return rc == ERROR_SUCCESS;
}

Json make_lnk_entry(const std::wstring& path, const LnkInfo& orig) {
    Json e = Json::make_obj();
    e.set("kind", Json::make_str("lnk"));
    e.set("path",    Json::make_str(wide_to_utf8(path)));
    e.set("target",  Json::make_str(wide_to_utf8(orig.target)));
    e.set("args",    Json::make_str(wide_to_utf8(orig.args)));
    e.set("workdir", Json::make_str(wide_to_utf8(orig.workdir)));
    return e;
}

Json make_runkey_entry(const std::wstring& orig_value) {
    Json e = Json::make_obj();
    e.set("kind",  Json::make_str("runkey"));
    e.set("value", Json::make_str(wide_to_utf8(orig_value)));
    return e;
}

} // namespace

int wrap_all_discord_launchers(const std::wstring& dnp_exe) {
    ComInit com;
    if (!com.ok) {
        LOG_ERR("CoInitialize failed; cannot rewrite shortcuts.");
        return 0;
    }

    Json manifest = Json::make_obj();
    Json entries  = Json::make_arr();

    int count = 0;

    for (const auto& path : candidate_shortcut_paths()) {
        if (!file_exists(path)) continue;
        LnkInfo orig = read_lnk(path);
        if (!orig.ok) {
            LOG_WARN("Failed to read shortcut %ls", path.c_str());
            continue;
        }
        if (!is_discord_launcher_target(orig.target)) {
            continue;
        }
        if (_wcsicmp(orig.target.c_str(), dnp_exe.c_str()) == 0) {
            LOG_DBG("Already wrapped: %ls", path.c_str());
            continue;
        }
        entries.as_arr().push_back(make_lnk_entry(path, orig));

        std::wstring wd = install_dir();
        if (write_lnk(path, dnp_exe, L"--launch", wd)) {
            LOG_INFO("Wrapped shortcut: %ls", path.c_str());
            ++count;
        } else {
            LOG_WARN("Wrap failed: %ls", path.c_str());
        }
    }

    {
        std::wstring orig_value;
        if (read_runkey(orig_value)) {
            if (icontains(orig_value, L"\\Discord\\Update.exe")) {
                entries.as_arr().push_back(make_runkey_entry(orig_value));
                std::wstring new_value = L"\"" + dnp_exe + L"\" --launch";
                if (write_runkey(new_value)) {
                    LOG_INFO("Wrapped RunKey HKCU\\...\\Run\\Discord");
                    ++count;
                } else {
                    LOG_WARN("Failed to write RunKey");
                }
            } else if (_wcsicmp(orig_value.c_str(),
                                (L"\"" + dnp_exe + L"\" --launch").c_str()) == 0) {
                LOG_DBG("RunKey already wrapped.");
            }
        }
    }

    manifest.set("entries", std::move(entries));
    save_manifest(manifest);
    return count;
}

int unwrap_all_discord_launchers() {
    ComInit com;
    if (!com.ok) return 0;

    Json manifest;
    if (!load_manifest(manifest)) {
        LOG_DBG("No manifest, nothing to unwrap.");
        return 0;
    }
    const Json* entries = manifest.find("entries");
    if (!entries || !entries->is_arr()) return 0;

    int restored = 0;
    for (const auto& e : entries->as_arr()) {
        if (!e.is_obj()) continue;
        const Json* kind = e.find("kind");
        if (!kind || !kind->is_str()) continue;

        if (kind->as_str() == "lnk") {
            const Json* jp = e.find("path");
            const Json* jt = e.find("target");
            const Json* ja = e.find("args");
            const Json* jw = e.find("workdir");
            if (!jp || !jt || !ja || !jw) continue;
            std::wstring lnk_path = utf8_to_wide(jp->as_str());
            std::wstring target   = utf8_to_wide(jt->as_str());
            std::wstring args     = utf8_to_wide(ja->as_str());
            std::wstring workdir  = utf8_to_wide(jw->as_str());
            if (!file_exists(lnk_path)) continue;
            if (write_lnk(lnk_path, target, args, workdir)) {
                LOG_INFO("Restored shortcut: %ls", lnk_path.c_str());
                ++restored;
            }
        } else if (kind->as_str() == "runkey") {
            const Json* jv = e.find("value");
            if (!jv) continue;
            std::wstring v = utf8_to_wide(jv->as_str());
            if (write_runkey(v)) {
                LOG_INFO("Restored RunKey to original");
                ++restored;
            }
        }
    }

    remove_file(manifest_path());
    return restored;
}

} // namespace dnp
