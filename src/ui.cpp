// ui.cpp — native Win32 control panel with Discord-inspired dark theme.
//
// Layout (420 x 360 client area):
//
//   ┌─ padding 24 ─────────────────────────────────────┐
//   │  DiscordNitroPatcher                             │
//   │  v0.2.0                                          │
//   │                                                  │
//   │  ● Installed and patched                         │
//   │  Discord 1.0.9238                                │
//   │                                                  │
//   │  [        Reapply patch        ]                 │
//   │  [        Launch Discord       ]                 │
//   │  [        Open log file        ]                 │
//   │  [        Uninstall            ]                 │
//   └──────────────────────────────────────────────────┘
//
// Colors mirror Discord's own palette so the panel feels like part of the same product.
#include "ui.h"
#include "config.h"
#include "installer.h"
#include "patcher.h"
#include "util.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <shellapi.h>
#include <dwmapi.h>
#include <uxtheme.h>

#include <cwchar>
#include <string>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

namespace dnp {

namespace {

// ============================================================================
// Palette (Discord-inspired dark)
// ============================================================================
constexpr COLORREF C_BG          = RGB(0x1E, 0x1F, 0x22);
constexpr COLORREF C_SURFACE     = RGB(0x2B, 0x2D, 0x31);
constexpr COLORREF C_SURFACE_HOV = RGB(0x3C, 0x3F, 0x45);
constexpr COLORREF C_SURFACE_PRS = RGB(0x4E, 0x52, 0x59);
constexpr COLORREF C_ACCENT      = RGB(0x58, 0x65, 0xF2);
constexpr COLORREF C_ACCENT_HOV  = RGB(0x6F, 0x7A, 0xF5);
constexpr COLORREF C_DANGER      = RGB(0xF2, 0x3F, 0x43);
constexpr COLORREF C_DANGER_HOV  = RGB(0xF4, 0x5A, 0x5F);
constexpr COLORREF C_TEXT        = RGB(0xF2, 0xF3, 0xF5);
constexpr COLORREF C_TEXT_DIM    = RGB(0xB5, 0xBA, 0xC1);
constexpr COLORREF C_GREEN       = RGB(0x23, 0xA5, 0x5A);
constexpr COLORREF C_AMBER       = RGB(0xF0, 0xB2, 0x32);
constexpr COLORREF C_GREY        = RGB(0x80, 0x84, 0x8E);

// ============================================================================
// State
// ============================================================================
enum class Status { NotInstalled, Installed, NeedsRepatch, DiscordMissing };

struct State {
    Status status = Status::NotInstalled;
    std::wstring discord_version;
    HFONT font_title = nullptr;
    HFONT font_status = nullptr;
    HFONT font_body = nullptr;
    HFONT font_button = nullptr;
    HFONT font_subtle = nullptr;
    HBRUSH brush_bg = nullptr;
    HWND  hwnd_root = nullptr;
    HWND  btn_primary = nullptr; // Patch / Install (accent)
    HWND  btn_launch  = nullptr;
    HWND  btn_log     = nullptr;
    HWND  btn_remove  = nullptr; // Uninstall (danger)
} g;

constexpr int ID_BTN_PRIMARY = 1001;
constexpr int ID_BTN_LAUNCH  = 1002;
constexpr int ID_BTN_LOG     = 1003;
constexpr int ID_BTN_REMOVE  = 1004;

// ============================================================================
// Status detection
// ============================================================================
void update_status() {
    auto app_dir = find_latest_discord_app_dir();
    g.discord_version.clear();
    if (!app_dir) {
        g.status = Status::DiscordMissing;
        return;
    }
    // Extract version from app_dir basename "app-X.Y.Z".
    auto slash = app_dir->find_last_of(L"\\/");
    std::wstring name = (slash == std::wstring::npos) ? *app_dir : app_dir->substr(slash + 1);
    if (name.compare(0, 4, L"app-") == 0) g.discord_version = name.substr(4);

    bool installed = file_exists(path_join(install_dir(), L"dnp.exe"));
    bool patched   = is_patched(asar_path_in_app_dir(*app_dir));

    if (!installed) g.status = Status::NotInstalled;
    else if (patched) g.status = Status::Installed;
    else g.status = Status::NeedsRepatch;
}

const wchar_t* status_text() {
    switch (g.status) {
        case Status::NotInstalled:    return L"Not installed";
        case Status::Installed:       return L"Installed and patched";
        case Status::NeedsRepatch:    return L"Installed, Discord needs repatch";
        case Status::DiscordMissing:  return L"Discord not found on this system";
    }
    return L"";
}

COLORREF status_color() {
    switch (g.status) {
        case Status::NotInstalled:    return C_GREY;
        case Status::Installed:       return C_GREEN;
        case Status::NeedsRepatch:    return C_AMBER;
        case Status::DiscordMissing:  return C_DANGER;
    }
    return C_GREY;
}

const wchar_t* primary_button_text() {
    switch (g.status) {
        case Status::NotInstalled:    return L"Install";
        case Status::Installed:       return L"Reapply patch";
        case Status::NeedsRepatch:    return L"Apply patch";
        case Status::DiscordMissing:  return L"Install Discord first";
    }
    return L"";
}

// ============================================================================
// Resource init
// ============================================================================
HFONT make_font(int height, int weight, const wchar_t* face) {
    return CreateFontW(
        -height, 0, 0, 0, weight, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_DONTCARE, face);
}

void init_resources() {
    // Segoe UI Variable Display (Win11) → falls back to Segoe UI on Win10.
    const wchar_t* face = L"Segoe UI Variable Display";
    g.font_title  = make_font(24, FW_BOLD,    face);
    g.font_status = make_font(15, FW_SEMIBOLD,face);
    g.font_body   = make_font(13, FW_NORMAL,  face);
    g.font_button = make_font(14, FW_SEMIBOLD,face);
    g.font_subtle = make_font(12, FW_NORMAL,  face);
    g.brush_bg    = CreateSolidBrush(C_BG);
}

void free_resources() {
    if (g.font_title)  DeleteObject(g.font_title);
    if (g.font_status) DeleteObject(g.font_status);
    if (g.font_body)   DeleteObject(g.font_body);
    if (g.font_button) DeleteObject(g.font_button);
    if (g.font_subtle) DeleteObject(g.font_subtle);
    if (g.brush_bg)    DeleteObject(g.brush_bg);
}

// ============================================================================
// Owner-drawn button — flat with hover/press tints and per-button accent color
// ============================================================================
struct ButtonStyle { COLORREF base, hover, press, text; };

ButtonStyle style_for(HWND btn) {
    int id = GetDlgCtrlID(btn);
    if (id == ID_BTN_PRIMARY) {
        // Accent unless install state would be disabled.
        if (g.status == Status::DiscordMissing) {
            return { C_SURFACE, C_SURFACE, C_SURFACE, C_TEXT_DIM };
        }
        return { C_ACCENT, C_ACCENT_HOV, C_ACCENT, C_TEXT };
    }
    if (id == ID_BTN_REMOVE) {
        if (g.status == Status::NotInstalled) {
            return { C_SURFACE, C_SURFACE, C_SURFACE, C_TEXT_DIM };
        }
        return { C_DANGER, C_DANGER_HOV, C_DANGER, C_TEXT };
    }
    return { C_SURFACE, C_SURFACE_HOV, C_SURFACE_PRS, C_TEXT };
}

void draw_button(LPDRAWITEMSTRUCT di) {
    HDC hdc = di->hDC;
    RECT rc = di->rcItem;

    ButtonStyle s = style_for(di->hwndItem);
    bool pressed = (di->itemState & ODS_SELECTED) != 0;
    bool focused = (di->itemState & ODS_FOCUS)    != 0;
    bool hover   = (di->itemState & 0x0040 /* ODS_HOTLIGHT (XP+) */) != 0;
    // ODS_HOTLIGHT isn't reliable; use mouse-over detection via WM_MOUSEMOVE tracked separately
    // to keep code minimal we use the focus state as a hover proxy.

    COLORREF fill = s.base;
    if (pressed) fill = s.press;
    else if (focused || hover) fill = s.hover;

    HBRUSH br = CreateSolidBrush(fill);
    FillRect(hdc, &rc, br);
    DeleteObject(br);

    // Focus ring — 1 px subtle outline when keyboard-focused.
    if (focused && !pressed) {
        HPEN pen = CreatePen(PS_SOLID, 1, C_ACCENT_HOV);
        HPEN oldpen = (HPEN)SelectObject(hdc, pen);
        HBRUSH oldbr = (HBRUSH)SelectObject(hdc, GetStockObject(NULL_BRUSH));
        Rectangle(hdc, rc.left, rc.top, rc.right, rc.bottom);
        SelectObject(hdc, oldpen);
        SelectObject(hdc, oldbr);
        DeleteObject(pen);
    }

    // Text
    wchar_t text[128] = {};
    GetWindowTextW(di->hwndItem, text, 128);
    SelectObject(hdc, g.font_button);
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, s.text);
    DrawTextW(hdc, text, -1, &rc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

// ============================================================================
// Window painting
// ============================================================================
void paint(HWND hwnd, HDC hdc) {
    RECT rc; GetClientRect(hwnd, &rc);
    FillRect(hdc, &rc, g.brush_bg);
    SetBkMode(hdc, TRANSPARENT);

    const int pad = 24;
    int y = pad;

    // Title
    SelectObject(hdc, g.font_title);
    SetTextColor(hdc, C_TEXT);
    RECT tr = { pad, y, rc.right - pad, y + 32 };
    DrawTextW(hdc, L"DiscordNitroPatcher", -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE);
    y += 30;

    // Version subtitle
    SelectObject(hdc, g.font_subtle);
    SetTextColor(hdc, C_TEXT_DIM);
    RECT vr = { pad, y, rc.right - pad, y + 18 };
    wchar_t vbuf[32];
    swprintf(vbuf, 32, L"v%ls", VERSION);
    DrawTextW(hdc, vbuf, -1, &vr, DT_LEFT | DT_TOP | DT_SINGLELINE);
    y += 26;

    // Status dot + text on same baseline
    int dot_size = 10;
    int dot_x = pad;
    int dot_y = y + 5;
    {
        HBRUSH br = CreateSolidBrush(status_color());
        HBRUSH oldbr = (HBRUSH)SelectObject(hdc, br);
        HPEN oldpen = (HPEN)SelectObject(hdc, GetStockObject(NULL_PEN));
        Ellipse(hdc, dot_x, dot_y, dot_x + dot_size, dot_y + dot_size);
        SelectObject(hdc, oldbr);
        SelectObject(hdc, oldpen);
        DeleteObject(br);
    }
    SelectObject(hdc, g.font_status);
    SetTextColor(hdc, C_TEXT);
    RECT sr = { dot_x + dot_size + 10, y, rc.right - pad, y + 22 };
    DrawTextW(hdc, status_text(), -1, &sr, DT_LEFT | DT_TOP | DT_SINGLELINE);
    y += 24;

    // Discord version line
    SelectObject(hdc, g.font_subtle);
    SetTextColor(hdc, C_TEXT_DIM);
    wchar_t dv[128];
    if (!g.discord_version.empty()) {
        swprintf(dv, 128, L"Discord %ls", g.discord_version.c_str());
    } else {
        wcscpy_s(dv, 128, L"");
    }
    RECT dr = { dot_x + dot_size + 10, y, rc.right - pad, y + 18 };
    DrawTextW(hdc, dv, -1, &dr, DT_LEFT | DT_TOP | DT_SINGLELINE);
}

// ============================================================================
// Layout
// ============================================================================
void layout_buttons() {
    RECT rc; GetClientRect(g.hwnd_root, &rc);
    const int pad     = 24;
    const int btn_h   = 36;
    const int btn_gap = 8;
    int y = 124;  // below status block
    int w = rc.right - pad * 2;

    HWND order[] = { g.btn_primary, g.btn_launch, g.btn_log, g.btn_remove };
    for (HWND b : order) {
        SetWindowPos(b, nullptr, pad, y, w, btn_h, SWP_NOZORDER);
        y += btn_h + btn_gap;
    }
}

// ============================================================================
// Action handlers
// ============================================================================
void do_open_log() {
    std::wstring lp = path_join(install_dir(), L"log.txt");
    if (!file_exists(lp)) {
        MessageBoxW(g.hwnd_root, L"No log file yet.\nIt is created on first install or first Discord launch.",
                    L"DiscordNitroPatcher", MB_OK | MB_ICONINFORMATION);
        return;
    }
    ShellExecuteW(g.hwnd_root, L"open", lp.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void do_primary_action() {
    if (g.status == Status::DiscordMissing) return;

    EnableWindow(g.btn_primary, FALSE);
    EnableWindow(g.btn_launch,  FALSE);
    EnableWindow(g.btn_log,     FALSE);
    EnableWindow(g.btn_remove,  FALSE);

    if (g.status == Status::NotInstalled) {
        do_install();
    } else {
        // Reapply patch: kill discord + apply_patch + launch.
        auto app_dir = find_latest_discord_app_dir();
        if (app_dir) {
            kill_discord_processes();
            Sleep(300);
            apply_patch(*app_dir);
            Sleep(200);
            launch_discord();
        }
    }

    update_status();
    SetWindowTextW(g.btn_primary, primary_button_text());
    EnableWindow(g.btn_primary, g.status != Status::DiscordMissing);
    EnableWindow(g.btn_launch,  TRUE);
    EnableWindow(g.btn_log,     TRUE);
    EnableWindow(g.btn_remove,  g.status != Status::NotInstalled);
    InvalidateRect(g.hwnd_root, nullptr, TRUE);
}

void do_uninstall_action() {
    if (g.status == Status::NotInstalled) return;
    int r = MessageBoxW(g.hwnd_root,
                       L"Uninstall DiscordNitroPatcher?\n\n"
                       L"This will close Discord, restore the original app.asar, "
                       L"and undo the shortcut + registry changes.",
                       L"DiscordNitroPatcher",
                       MB_YESNO | MB_ICONQUESTION);
    if (r != IDYES) return;

    EnableWindow(g.btn_primary, FALSE);
    EnableWindow(g.btn_launch,  FALSE);
    EnableWindow(g.btn_log,     FALSE);
    EnableWindow(g.btn_remove,  FALSE);

    do_uninstall();

    MessageBoxW(g.hwnd_root, L"Uninstalled. Discord is back to factory default.",
                L"DiscordNitroPatcher", MB_OK | MB_ICONINFORMATION);

    // The install dir was deleted, including the exe we're running from. Exit.
    PostMessageW(g.hwnd_root, WM_CLOSE, 0, 0);
}

// ============================================================================
// Window proc
// ============================================================================
LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            init_resources();
            // Standard buttons with owner-draw flag for our custom painting.
            DWORD bs = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;
            HINSTANCE hi = GetModuleHandleW(nullptr);
            g.btn_primary = CreateWindowW(L"BUTTON", L"Install",    bs,
                0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_BTN_PRIMARY, hi, nullptr);
            g.btn_launch  = CreateWindowW(L"BUTTON", L"Launch Discord", bs,
                0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_BTN_LAUNCH,  hi, nullptr);
            g.btn_log     = CreateWindowW(L"BUTTON", L"Open log file",  bs,
                0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_BTN_LOG,     hi, nullptr);
            g.btn_remove  = CreateWindowW(L"BUTTON", L"Uninstall",      bs,
                0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_BTN_REMOVE,  hi, nullptr);
            update_status();
            SetWindowTextW(g.btn_primary, primary_button_text());
            EnableWindow(g.btn_primary, g.status != Status::DiscordMissing);
            EnableWindow(g.btn_remove,  g.status != Status::NotInstalled);
            layout_buttons();
            return 0;
        }
        case WM_SIZE:
            layout_buttons();
            return 0;
        case WM_ERASEBKGND:
            return 1;  // we paint everything in WM_PAINT
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            paint(hwnd, hdc);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DRAWITEM:
            draw_button((LPDRAWITEMSTRUCT)lp);
            return TRUE;
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == ID_BTN_PRIMARY) do_primary_action();
            else if (id == ID_BTN_LAUNCH) launch_discord();
            else if (id == ID_BTN_LOG)    do_open_log();
            else if (id == ID_BTN_REMOVE) do_uninstall_action();
            return 0;
        }
        case WM_DESTROY:
            free_resources();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

// ============================================================================
// Public entry
// ============================================================================
int run_ui() {
    WNDCLASSW wc = {};
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DnpMainWnd";
    // IDC_ARROW resolves to the ANSI macro without UNICODE defined; use MAKEINTRESOURCEW form.
    wc.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);

    const int W = 440, H = 380;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;

    HWND hwnd = CreateWindowExW(0, L"DnpMainWnd", L"DiscordNitroPatcher",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        sx, sy, W, H,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) return 1;
    g.hwnd_root = hwnd;

    // Dark immersive title bar (Win10 build 19041+ / Win11).
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(hwnd, &msg)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    return 0;
}

} // namespace dnp
