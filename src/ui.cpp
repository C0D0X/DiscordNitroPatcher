// ui.cpp — native Win32 control panel with GDI+ rendering.
//
// Layout (460 x 440 client area):
//
//   ╔═ 3 px blurple accent strip ═══════════════════════════════════╗
//   │                                                                │
//   │  DiscordNitroPatcher                                  v0.2.0   │
//   │                                                                │
//   │  ╭─ surface card ─────────────────────────────────────────╮   │
//   │  │  ● Installed and patched                                │   │
//   │  │  Discord 1.0.9238                                       │   │
//   │  ╰─────────────────────────────────────────────────────────╯   │
//   │                                                                │
//   │  ╭───────── Reapply patch (accent, taller) ───────────────╮   │
//   │  ╰─────────────────────────────────────────────────────────╯   │
//   │  ╭─ Launch Discord ─╮  ╭─ Open log file ────────────────╮     │
//   │  ╰──────────────────╯  ╰─────────────────────────────────╯     │
//   │  ╭───────── Uninstall (danger) ────────────────────────────╮   │
//   │  ╰─────────────────────────────────────────────────────────╯   │
//   │                                                                │
//   │  github.com/C0D0X/DiscordNitroPatcher                          │
//   ╚════════════════════════════════════════════════════════════════╝
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

#include <objidl.h>      // IStream — required by gdiplus headers
#include <algorithm>     // std::min, used by <gdiplus.h>
#include <gdiplus.h>

#include <cwchar>
#include <string>
#include <unordered_map>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif

using namespace Gdiplus;

namespace dnp {

namespace {

// ============================================================================
// Palette (Discord-inspired dark)
// ============================================================================
struct RGBA { BYTE r, g, b, a; };

constexpr RGBA C_BG          = {0x1E, 0x1F, 0x22, 0xFF};
constexpr RGBA C_SURFACE     = {0x2B, 0x2D, 0x31, 0xFF};
constexpr RGBA C_SURFACE_HOV = {0x35, 0x37, 0x3D, 0xFF};
constexpr RGBA C_SURFACE_PRS = {0x42, 0x45, 0x4C, 0xFF};
constexpr RGBA C_ACCENT      = {0x58, 0x65, 0xF2, 0xFF};
constexpr RGBA C_ACCENT_HOV  = {0x6F, 0x7A, 0xF5, 0xFF};
constexpr RGBA C_ACCENT_PRS  = {0x49, 0x55, 0xD9, 0xFF};
constexpr RGBA C_DANGER      = {0xF2, 0x3F, 0x43, 0xFF};
constexpr RGBA C_DANGER_HOV  = {0xF4, 0x5A, 0x5F, 0xFF};
constexpr RGBA C_DANGER_PRS  = {0xC8, 0x35, 0x39, 0xFF};
constexpr RGBA C_TEXT        = {0xF2, 0xF3, 0xF5, 0xFF};
constexpr RGBA C_TEXT_DIM    = {0xB5, 0xBA, 0xC1, 0xFF};
constexpr RGBA C_TEXT_SUBTLE = {0x80, 0x84, 0x8E, 0xFF};
constexpr RGBA C_GREEN       = {0x23, 0xA5, 0x5A, 0xFF};
constexpr RGBA C_AMBER       = {0xF0, 0xB2, 0x32, 0xFF};
constexpr RGBA C_BORDER      = {0x3F, 0x42, 0x49, 0xFF};

Color gp(RGBA c) { return Color(c.a, c.r, c.g, c.b); }

// ============================================================================
// State
// ============================================================================
enum class Status { NotInstalled, Installed, NeedsRepatch, DiscordMissing };

struct Btn {
    HWND hwnd = nullptr;
    bool hover = false;
    bool tracking = false;
};

struct State {
    Status status = Status::NotInstalled;
    std::wstring discord_version;

    // GDI+
    ULONG_PTR gdiplus_token = 0;

    // Window
    HWND hwnd_root = nullptr;
    HBRUSH brush_bg = nullptr;

    // Buttons
    Btn btn_primary, btn_launch, btn_log, btn_remove;

    // Hit area for footer link
    RECT footer_rc = {};
    bool footer_hover = false;
    bool footer_tracking = false;

    // Disable interaction during long-running ops.
    bool busy = false;
    std::wstring busy_text;
} g;

constexpr int ID_BTN_PRIMARY = 1001;
constexpr int ID_BTN_LAUNCH  = 1002;
constexpr int ID_BTN_LOG     = 1003;
constexpr int ID_BTN_REMOVE  = 1004;

constexpr const wchar_t* FOOTER_URL = L"github.com/C0D0X/DiscordNitroPatcher";

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
        case Status::NotInstalled:   return L"Not installed";
        case Status::Installed:      return L"Installed and patched";
        case Status::NeedsRepatch:   return L"Installed, Discord needs repatch";
        case Status::DiscordMissing: return L"Discord not found";
    }
    return L"";
}

RGBA status_color() {
    switch (g.status) {
        case Status::NotInstalled:   return C_TEXT_SUBTLE;
        case Status::Installed:      return C_GREEN;
        case Status::NeedsRepatch:   return C_AMBER;
        case Status::DiscordMissing: return C_DANGER;
    }
    return C_TEXT_SUBTLE;
}

const wchar_t* primary_button_text() {
    switch (g.status) {
        case Status::NotInstalled:   return L"Install";
        case Status::Installed:      return L"Reapply patch";
        case Status::NeedsRepatch:   return L"Apply patch";
        case Status::DiscordMissing: return L"Install Discord first";
    }
    return L"";
}

// ============================================================================
// GDI+ helpers
// ============================================================================
void fill_rounded_rect(Graphics& gfx, REAL x, REAL y, REAL w, REAL h, REAL r, Color color) {
    GraphicsPath path;
    path.AddArc(x,         y,         r * 2, r * 2, 180.0f, 90.0f);
    path.AddArc(x + w - r * 2, y,         r * 2, r * 2, 270.0f, 90.0f);
    path.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2,   0.0f, 90.0f);
    path.AddArc(x,         y + h - r * 2, r * 2, r * 2,  90.0f, 90.0f);
    path.CloseFigure();
    SolidBrush br(color);
    gfx.FillPath(&br, &path);
}

void stroke_rounded_rect(Graphics& gfx, REAL x, REAL y, REAL w, REAL h, REAL r, Color color, REAL stroke = 1.0f) {
    GraphicsPath path;
    path.AddArc(x,         y,         r * 2, r * 2, 180.0f, 90.0f);
    path.AddArc(x + w - r * 2, y,         r * 2, r * 2, 270.0f, 90.0f);
    path.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2,   0.0f, 90.0f);
    path.AddArc(x,         y + h - r * 2, r * 2, r * 2,  90.0f, 90.0f);
    path.CloseFigure();
    Pen pen(color, stroke);
    gfx.DrawPath(&pen, &path);
}

void draw_text(Graphics& gfx, const wchar_t* text, const Font& font, REAL x, REAL y, REAL w, REAL h,
               Color color, StringAlignment halign = StringAlignmentNear,
               StringAlignment valign = StringAlignmentCenter) {
    StringFormat fmt;
    fmt.SetAlignment(halign);
    fmt.SetLineAlignment(valign);
    fmt.SetTrimming(StringTrimmingEllipsisCharacter);
    SolidBrush br(color);
    RectF rc(x, y, w, h);
    gfx.DrawString(text, -1, &font, rc, &fmt, &br);
}

REAL measure_text_width(Graphics& gfx, const wchar_t* text, const Font& font) {
    RectF bounds(0, 0, 10000, 1000);
    RectF out;
    gfx.MeasureString(text, -1, &font, bounds, &out);
    return out.Width;
}

// ============================================================================
// Hover tracking — TrackMouseEvent per button so we get proper hover/leave.
// ============================================================================
void install_hover_track(HWND hwnd, bool& tracking) {
    if (tracking) return;
    TRACKMOUSEEVENT t{};
    t.cbSize    = sizeof(t);
    t.dwFlags   = TME_LEAVE;
    t.hwndTrack = hwnd;
    TrackMouseEvent(&t);
    tracking = true;
}

Btn* find_btn(HWND hwnd) {
    if (hwnd == g.btn_primary.hwnd) return &g.btn_primary;
    if (hwnd == g.btn_launch.hwnd)  return &g.btn_launch;
    if (hwnd == g.btn_log.hwnd)     return &g.btn_log;
    if (hwnd == g.btn_remove.hwnd)  return &g.btn_remove;
    return nullptr;
}

// Subclass proc for buttons — intercept WM_MOUSEMOVE and WM_MOUSELEAVE for hover state.
LRESULT CALLBACK btn_subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR /*id*/, DWORD_PTR /*ref*/) {
    Btn* b = find_btn(hwnd);
    if (b) {
        if (msg == WM_MOUSEMOVE) {
            install_hover_track(hwnd, b->tracking);
            if (!b->hover) {
                b->hover = true;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        } else if (msg == WM_MOUSELEAVE) {
            b->hover = false;
            b->tracking = false;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}

// ============================================================================
// Button painting (owner-draw)
// ============================================================================
struct ButtonPalette { RGBA base, hover, press, text; };

ButtonPalette palette_for(HWND btn) {
    int id = GetDlgCtrlID(btn);
    bool disabled = !IsWindowEnabled(btn);
    if (id == ID_BTN_PRIMARY) {
        if (disabled) return { C_SURFACE, C_SURFACE, C_SURFACE, C_TEXT_SUBTLE };
        return { C_ACCENT, C_ACCENT_HOV, C_ACCENT_PRS, C_TEXT };
    }
    if (id == ID_BTN_REMOVE) {
        if (disabled) return { C_SURFACE, C_SURFACE, C_SURFACE, C_TEXT_SUBTLE };
        return { C_DANGER, C_DANGER_HOV, C_DANGER_PRS, C_TEXT };
    }
    if (disabled) return { C_SURFACE, C_SURFACE, C_SURFACE, C_TEXT_SUBTLE };
    return { C_SURFACE, C_SURFACE_HOV, C_SURFACE_PRS, C_TEXT };
}

void draw_button(LPDRAWITEMSTRUCT di) {
    HDC hdc = di->hDC;
    RECT rc = di->rcItem;
    int w = rc.right - rc.left;
    int h = rc.bottom - rc.top;

    Btn* b = find_btn(di->hwndItem);
    ButtonPalette pal = palette_for(di->hwndItem);
    bool pressed = (di->itemState & ODS_SELECTED) != 0;
    bool hover   = b && b->hover;

    RGBA fill = pal.base;
    if (pressed)      fill = pal.press;
    else if (hover)   fill = pal.hover;

    Graphics gfx(hdc);
    gfx.SetSmoothingMode(SmoothingModeAntiAlias);
    gfx.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // Clear behind the rounded shape with the window bg so corners alpha-blend cleanly.
    SolidBrush bg(gp(C_BG));
    gfx.FillRectangle(&bg, 0, 0, w, h);

    // Rounded button body.
    fill_rounded_rect(gfx, 0.5f, 0.5f, (REAL)w - 1.0f, (REAL)h - 1.0f, 8.0f, gp(fill));

    // Focus ring — subtle outline when keyboard focus.
    if (di->itemState & ODS_FOCUS) {
        stroke_rounded_rect(gfx, 0.5f, 0.5f, (REAL)w - 1.0f, (REAL)h - 1.0f, 8.0f,
                            gp(C_ACCENT_HOV), 1.5f);
    }

    // Label.
    wchar_t text[128] = {};
    GetWindowTextW(di->hwndItem, text, 128);
    FontFamily ff(L"Segoe UI Variable Display");
    int id = GetDlgCtrlID(di->hwndItem);
    REAL pt = (id == ID_BTN_PRIMARY) ? 11.0f : 10.0f;
    Font font(&ff, pt, FontStyleBold, UnitPoint);
    draw_text(gfx, text, font, 0, 0, (REAL)w, (REAL)h, gp(pal.text),
              StringAlignmentCenter, StringAlignmentCenter);
}

// ============================================================================
// Main window painting (header + status card + footer)
// ============================================================================
constexpr int PAD       = 24;
constexpr int ACCENT_H  = 3;
constexpr int HEADER_H  = 64;        // title row
constexpr int CARD_H    = 88;        // status card height

void paint(HWND hwnd, HDC hdc) {
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    Graphics gfx(hdc);
    gfx.SetSmoothingMode(SmoothingModeAntiAlias);
    gfx.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // Background
    SolidBrush bg(gp(C_BG));
    gfx.FillRectangle(&bg, 0, 0, W, H);

    // Top accent strip
    SolidBrush accent(gp(C_ACCENT));
    gfx.FillRectangle(&accent, 0, 0, W, ACCENT_H);

    // Header — product name left, version right
    {
        FontFamily ff(L"Segoe UI Variable Display");
        Font fTitle(&ff, 16.0f, FontStyleBold,   UnitPoint);
        Font fVer  (&ff,  9.0f, FontStyleRegular, UnitPoint);

        REAL ty = (REAL)(ACCENT_H + 16);
        draw_text(gfx, L"DiscordNitroPatcher", fTitle,
                  (REAL)PAD, ty, (REAL)(W - PAD * 2), 28.0f,
                  gp(C_TEXT), StringAlignmentNear, StringAlignmentNear);

        wchar_t vbuf[16];
        swprintf(vbuf, 16, L"v%ls", VERSION);
        draw_text(gfx, vbuf, fVer,
                  (REAL)PAD, ty + 4.0f, (REAL)(W - PAD * 2), 24.0f,
                  gp(C_TEXT_SUBTLE), StringAlignmentFar, StringAlignmentNear);
    }

    // Status card
    int card_y = ACCENT_H + HEADER_H;
    {
        REAL cx = (REAL)PAD;
        REAL cy = (REAL)card_y;
        REAL cw = (REAL)(W - PAD * 2);
        REAL ch = (REAL)CARD_H;
        fill_rounded_rect(gfx, cx, cy, cw, ch, 10.0f, gp(C_SURFACE));
        stroke_rounded_rect(gfx, cx + 0.5f, cy + 0.5f, cw - 1.0f, ch - 1.0f, 10.0f, gp(C_BORDER), 1.0f);

        // Status dot
        int dot_d = 12;
        SolidBrush dot(gp(status_color()));
        gfx.FillEllipse(&dot, (int)cx + 20, (int)cy + 24, dot_d, dot_d);

        FontFamily ff(L"Segoe UI Variable Display");
        Font fStatus(&ff, 12.0f, FontStyleBold,    UnitPoint);
        Font fSub   (&ff,  9.5f, FontStyleRegular, UnitPoint);

        // Show "Working..." text in place of status when an operation is in-flight.
        const wchar_t* primary = g.busy ? g.busy_text.c_str() : status_text();
        draw_text(gfx, primary, fStatus,
                  cx + 40.0f, cy + 18.0f, cw - 56.0f, 22.0f,
                  gp(C_TEXT), StringAlignmentNear, StringAlignmentNear);

        // Subtitle: Discord version (or empty)
        wchar_t sub[128] = {};
        if (g.status == Status::DiscordMissing) {
            wcscpy_s(sub, 128, L"Install Discord and reopen this panel.");
        } else if (!g.discord_version.empty()) {
            swprintf(sub, 128, L"Discord %ls", g.discord_version.c_str());
        } else {
            wcscpy_s(sub, 128, L"");
        }
        draw_text(gfx, sub, fSub,
                  cx + 40.0f, cy + 46.0f, cw - 56.0f, 20.0f,
                  gp(C_TEXT_DIM), StringAlignmentNear, StringAlignmentNear);
    }

    // Footer link — measure to set hit area for click + hover.
    {
        FontFamily ff(L"Segoe UI Variable Display");
        Font fFoot(&ff, 8.5f, FontStyleRegular, UnitPoint);

        REAL tw = measure_text_width(gfx, FOOTER_URL, fFoot);
        REAL fx = (REAL)PAD;
        REAL fy = (REAL)(H - 24);
        RGBA fc = g.footer_hover ? C_ACCENT_HOV : C_TEXT_SUBTLE;
        draw_text(gfx, FOOTER_URL, fFoot, fx, fy, (REAL)(W - PAD * 2), 20.0f,
                  gp(fc), StringAlignmentNear, StringAlignmentNear);
        g.footer_rc = { (LONG)fx, (LONG)fy, (LONG)(fx + tw), (LONG)(fy + 18) };
    }
}

// ============================================================================
// Layout
// ============================================================================
void layout_controls() {
    if (!g.hwnd_root) return;
    RECT rc; GetClientRect(g.hwnd_root, &rc);
    int W = rc.right;
    int H = rc.bottom;
    (void)H;
    int top = ACCENT_H + HEADER_H + CARD_H + 20;
    int bw  = W - PAD * 2;

    const int gap = 10;
    const int H_PRIMARY   = 44;
    const int H_SECONDARY = 38;
    const int H_DANGER    = 38;

    // Primary
    SetWindowPos(g.btn_primary.hwnd, nullptr, PAD, top, bw, H_PRIMARY, SWP_NOZORDER);
    top += H_PRIMARY + gap;

    // Side-by-side: Launch | Open log
    int side_w = (bw - gap) / 2;
    SetWindowPos(g.btn_launch.hwnd, nullptr, PAD,             top, side_w,            H_SECONDARY, SWP_NOZORDER);
    SetWindowPos(g.btn_log.hwnd,    nullptr, PAD + side_w + gap, top, bw - side_w - gap, H_SECONDARY, SWP_NOZORDER);
    top += H_SECONDARY + gap;

    // Danger
    SetWindowPos(g.btn_remove.hwnd, nullptr, PAD, top, bw, H_DANGER, SWP_NOZORDER);
}

// ============================================================================
// Refresh helpers
// ============================================================================
void refresh_buttons() {
    SetWindowTextW(g.btn_primary.hwnd, primary_button_text());
    EnableWindow(g.btn_primary.hwnd, !g.busy && g.status != Status::DiscordMissing);
    EnableWindow(g.btn_launch.hwnd,  !g.busy && g.status != Status::DiscordMissing);
    EnableWindow(g.btn_log.hwnd,     !g.busy);
    EnableWindow(g.btn_remove.hwnd,  !g.busy && g.status != Status::NotInstalled);
    InvalidateRect(g.hwnd_root, nullptr, TRUE);
}

void set_busy(const wchar_t* what) {
    g.busy = true;
    g.busy_text = what;
    refresh_buttons();
    // Force immediate paint so the user sees the busy state.
    UpdateWindow(g.hwnd_root);
}

void clear_busy() {
    g.busy = false;
    g.busy_text.clear();
    refresh_buttons();
}

// ============================================================================
// Action handlers
// ============================================================================
void action_open_log() {
    std::wstring lp = path_join(install_dir(), L"log.txt");
    if (!file_exists(lp)) {
        MessageBoxW(g.hwnd_root,
                    L"No log file yet.\n\nIt is created on first install or first Discord launch.",
                    L"DiscordNitroPatcher", MB_OK | MB_ICONINFORMATION);
        return;
    }
    ShellExecuteW(g.hwnd_root, L"open", lp.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void action_primary() {
    if (g.status == Status::DiscordMissing) return;

    if (g.status == Status::NotInstalled) {
        set_busy(L"Installing...");
        do_install();
    } else {
        set_busy(L"Reapplying patch...");
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
    clear_busy();
}

void action_uninstall() {
    if (g.status == Status::NotInstalled) return;
    int r = MessageBoxW(g.hwnd_root,
                       L"Uninstall DiscordNitroPatcher?\n\n"
                       L"This closes Discord, restores the original app.asar, "
                       L"and undoes the shortcut / registry changes.",
                       L"DiscordNitroPatcher",
                       MB_YESNO | MB_ICONQUESTION);
    if (r != IDYES) return;

    set_busy(L"Uninstalling...");
    do_uninstall();
    MessageBoxW(g.hwnd_root, L"Uninstalled. Discord is back to factory default.",
                L"DiscordNitroPatcher", MB_OK | MB_ICONINFORMATION);
    PostMessageW(g.hwnd_root, WM_CLOSE, 0, 0);
}

void action_open_footer_link() {
    ShellExecuteW(g.hwnd_root, L"open",
        L"https://github.com/C0D0X/DiscordNitroPatcher",
        nullptr, nullptr, SW_SHOWNORMAL);
}

// ============================================================================
// Window procedure
// ============================================================================
LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g.brush_bg = CreateSolidBrush(RGB(C_BG.r, C_BG.g, C_BG.b));

            DWORD bs = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW;
            HINSTANCE hi = GetModuleHandleW(nullptr);
            g.btn_primary.hwnd = CreateWindowW(L"BUTTON", L"Install",
                bs, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_BTN_PRIMARY, hi, nullptr);
            g.btn_launch.hwnd  = CreateWindowW(L"BUTTON", L"Launch Discord",
                bs, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_BTN_LAUNCH,  hi, nullptr);
            g.btn_log.hwnd     = CreateWindowW(L"BUTTON", L"Open log file",
                bs, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_BTN_LOG,     hi, nullptr);
            g.btn_remove.hwnd  = CreateWindowW(L"BUTTON", L"Uninstall",
                bs, 0, 0, 0, 0, hwnd, (HMENU)(INT_PTR)ID_BTN_REMOVE,  hi, nullptr);

            SetWindowSubclass(g.btn_primary.hwnd, btn_subclass, 1, 0);
            SetWindowSubclass(g.btn_launch.hwnd,  btn_subclass, 2, 0);
            SetWindowSubclass(g.btn_log.hwnd,     btn_subclass, 3, 0);
            SetWindowSubclass(g.btn_remove.hwnd,  btn_subclass, 4, 0);

            update_status();
            refresh_buttons();
            layout_controls();
            return 0;
        }
        case WM_SIZE:
            layout_controls();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            // Double-buffer to eliminate flicker from GDI+ over the buttons' edges.
            RECT rc; GetClientRect(hwnd, &rc);
            int W = rc.right, H = rc.bottom;
            HDC mem = CreateCompatibleDC(hdc);
            HBITMAP bmp = CreateCompatibleBitmap(hdc, W, H);
            HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
            paint(hwnd, mem);
            BitBlt(hdc, 0, 0, W, H, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteObject(bmp);
            DeleteDC(mem);
            EndPaint(hwnd, &ps);
            return 0;
        }
        case WM_DRAWITEM:
            draw_button((LPDRAWITEMSTRUCT)lp);
            return TRUE;
        case WM_COMMAND: {
            int id = LOWORD(wp);
            if (id == ID_BTN_PRIMARY) action_primary();
            else if (id == ID_BTN_LAUNCH) launch_discord();
            else if (id == ID_BTN_LOG)    action_open_log();
            else if (id == ID_BTN_REMOVE) action_uninstall();
            return 0;
        }
        case WM_MOUSEMOVE: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            bool over_footer = PtInRect(&g.footer_rc, pt);
            if (over_footer != g.footer_hover) {
                g.footer_hover = over_footer;
                SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(over_footer ? 32649 /*IDC_HAND*/ : 32512 /*IDC_ARROW*/)));
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            if (over_footer && !g.footer_tracking) {
                TRACKMOUSEEVENT t{};
                t.cbSize = sizeof(t);
                t.dwFlags = TME_LEAVE;
                t.hwndTrack = hwnd;
                TrackMouseEvent(&t);
                g.footer_tracking = true;
            }
            return 0;
        }
        case WM_MOUSELEAVE:
            if (g.footer_hover) {
                g.footer_hover = false;
                g.footer_tracking = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_LBUTTONUP: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            if (PtInRect(&g.footer_rc, pt)) action_open_footer_link();
            return 0;
        }
        case WM_SETCURSOR: {
            if ((HWND)wp == hwnd) {
                POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
                if (PtInRect(&g.footer_rc, pt)) {
                    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649))); // IDC_HAND
                    return TRUE;
                }
            }
            break;
        }
        case WM_DESTROY:
            if (g.brush_bg) { DeleteObject(g.brush_bg); g.brush_bg = nullptr; }
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
    GdiplusStartupInput gsi;
    if (GdiplusStartup(&g.gdiplus_token, &gsi, nullptr) != Ok) return 1;

    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC  = ICC_STANDARD_CLASSES;
    InitCommonControlsEx(&icc);

    WNDCLASSW wc = {};
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"DnpMainWnd";
    wc.hCursor       = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hbrBackground = nullptr;
    RegisterClassW(&wc);

    const int W = 480, H = 460;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;

    HWND hwnd = CreateWindowExW(0, L"DnpMainWnd", L"DiscordNitroPatcher",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
        sx, sy, W, H,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { GdiplusShutdown(g.gdiplus_token); return 1; }
    g.hwnd_root = hwnd;

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

    GdiplusShutdown(g.gdiplus_token);
    return 0;
}

} // namespace dnp
