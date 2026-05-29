// win32 control panel
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

#include <objidl.h>
#include <algorithm>
#include <gdiplus.h>

#include <cmath>
#include <cwchar>
#include <string>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "gdiplus.lib")

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_SYSTEMBACKDROP_TYPE
#define DWMWA_SYSTEMBACKDROP_TYPE 38
#endif
#ifndef DWMSBT_MAINWINDOW
#define DWMSBT_MAINWINDOW 2  // Mica
#endif
#ifndef DWMSBT_NONE
#define DWMSBT_NONE 1        // no system backdrop - flat opaque
#endif

using namespace Gdiplus;

namespace dnp {

namespace {

// palette
struct RGBA { BYTE r, g, b, a; };

constexpr RGBA C_BG          = {0x0B, 0x0B, 0x0C, 0xFF};
constexpr RGBA C_CARD        = {0x1A, 0x1B, 0x1E, 0xFF};
constexpr RGBA C_CARD_BORDER = {0xFF, 0xFF, 0xFF, 0x1E};
constexpr RGBA C_BTN         = {0x25, 0x25, 0x25, 0xFF};
constexpr RGBA C_BTN_BORDER  = {0x3D, 0x3D, 0x3D, 0xFF};
constexpr RGBA C_BTN_TEXT    = {0xBB, 0xBB, 0xBB, 0xFF};
constexpr RGBA C_HOVER       = {0x30, 0x30, 0x30, 0xFF};
constexpr RGBA C_PRESS       = {0x10, 0x10, 0x12, 0xFF};
constexpr RGBA C_TEXT        = {0xF2, 0xF2, 0xF3, 0xFF};
constexpr RGBA C_TEXT_DIM    = {0x8A, 0x8C, 0x90, 0xFF};
constexpr RGBA C_TEXT_SUBTLE = {0x5A, 0x5C, 0x60, 0xFF};
constexpr RGBA C_ACCENT      = {0x2D, 0x7A, 0x3A, 0xFF};
constexpr RGBA C_ACCENT_HOV  = {0x34, 0x8A, 0x44, 0xFF};
constexpr RGBA C_ACCENT_DEEP = {0x25, 0x63, 0x2F, 0xFF};
constexpr RGBA C_ACCENT_BDR  = {0x3A, 0x94, 0x48, 0xFF};
constexpr RGBA C_ACCENT_INK  = {0xB8, 0xEC, 0xC0, 0xFF};
constexpr RGBA C_GREEN       = {0x3A, 0x94, 0x48, 0xFF};
constexpr RGBA C_AMBER       = {0xD9, 0x9A, 0x2B, 0xFF};
constexpr RGBA C_DANGER      = {0xE0, 0x55, 0x55, 0xFF};
constexpr RGBA C_DANGER_BDR  = {0x5A, 0x2A, 0x2A, 0xFF};
constexpr RGBA C_DANGER_HOV  = {0xF0, 0x6A, 0x6A, 0xFF};

Color gp(RGBA c)              { return Color(c.a, c.r, c.g, c.b); }
Color gpA(RGBA c, BYTE a)     { return Color(a,   c.r, c.g, c.b); }

// icons
constexpr const wchar_t* IC_PLAY      = L"";
constexpr const wchar_t* IC_REFRESH   = L"";
constexpr const wchar_t* IC_DOC       = L"";
constexpr const wchar_t* IC_TRASH     = L"";
constexpr const wchar_t* IC_CHECK     = L"";
constexpr const wchar_t* IC_WARN      = L"";
constexpr const wchar_t* IC_INFO      = L"";
constexpr const wchar_t* IC_ERROR     = L"";
constexpr const wchar_t* IC_DOWNLOAD  = L"";
enum class Status { NotInstalled, Installed, NeedsRepatch, DiscordMissing };

struct Btn {
    int   id         = 0;
    RECT  rc         = {};
    bool  enabled    = true;
    float hover_t    = 0.0f;
    float hover_targ = 0.0f;
    float press_t    = 0.0f;
    float press_targ = 0.0f;
};

struct State {
    Status status = Status::NotInstalled;
    std::wstring discord_version;

    ULONG_PTR gdiplus_token = 0;
    HWND hwnd_root = nullptr;
    HBRUSH brush_bg = nullptr;
    UINT_PTR anim_timer = 0;
    DWORD start_tick = 0;
    bool icon_font_fluent = true;

    Btn btn_primary, btn_reapply, btn_log, btn_remove;
    Btn* pressed = nullptr;
    bool mouse_tracking = false;

    RECT footer_rc = {};
    float footer_t = 0.0f;
    float footer_targ = 0.0f;

    bool busy = false;
    std::wstring busy_text;
} g;

constexpr int ID_BTN_PRIMARY = 1001;
constexpr int ID_BTN_REAPPLY = 1002;
constexpr int ID_BTN_LOG     = 1003;
constexpr int ID_BTN_REMOVE  = 1004;

constexpr const wchar_t* FOOTER_URL = L"github.com/C0D0X/DiscordNitroPatcher";

void update_status() {
    auto app_dir = find_latest_discord_app_dir();
    g.discord_version.clear();
    if (!app_dir) { g.status = Status::DiscordMissing; return; }
    auto slash = app_dir->find_last_of(L"\\/");
    std::wstring name = (slash == std::wstring::npos) ? *app_dir : app_dir->substr(slash + 1);
    if (name.compare(0, 4, L"app-") == 0) g.discord_version = name.substr(4);

    bool installed = file_exists(path_join(install_dir(), L"dnp.exe"));
    bool patched   = is_patched(asar_path_in_app_dir(*app_dir));
    if (!installed) g.status = Status::NotInstalled;
    else if (patched) g.status = Status::Installed;
    else g.status = Status::NeedsRepatch;
}

const wchar_t* status_title() {
    switch (g.status) {
        case Status::NotInstalled:   return L"Not installed";
        case Status::Installed:      return L"Patched successfully";
        case Status::NeedsRepatch:   return L"Needs repatch";
        case Status::DiscordMissing: return L"Discord not found";
    }
    return L"";
}

const wchar_t* status_icon() {
    switch (g.status) {
        case Status::NotInstalled:   return IC_INFO;
        case Status::Installed:      return IC_CHECK;
        case Status::NeedsRepatch:   return IC_WARN;
        case Status::DiscordMissing: return IC_ERROR;
    }
    return IC_INFO;
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
        case Status::Installed:      return L"Launch Discord";
        case Status::NeedsRepatch:   return L"Apply patch & launch";
        case Status::DiscordMissing: return L"Install Discord first";
    }
    return L"";
}

const wchar_t* primary_button_icon() {
    switch (g.status) {
        case Status::NotInstalled:   return IC_DOWNLOAD;
        case Status::Installed:      return IC_PLAY;
        case Status::NeedsRepatch:   return IC_REFRESH;
        case Status::DiscordMissing: return IC_INFO;
    }
    return IC_PLAY;
}

// gdi+ helpers
void add_round_rect(GraphicsPath& p, REAL x, REAL y, REAL w, REAL h, REAL r) {
    p.AddArc(x,             y,             r * 2, r * 2, 180.0f, 90.0f);
    p.AddArc(x + w - r * 2, y,             r * 2, r * 2, 270.0f, 90.0f);
    p.AddArc(x + w - r * 2, y + h - r * 2, r * 2, r * 2,   0.0f, 90.0f);
    p.AddArc(x,             y + h - r * 2, r * 2, r * 2,  90.0f, 90.0f);
    p.CloseFigure();
}

void fill_round(Graphics& gfx, REAL x, REAL y, REAL w, REAL h, REAL r, Color c) {
    GraphicsPath p; add_round_rect(p, x, y, w, h, r);
    SolidBrush br(c);
    gfx.FillPath(&br, &p);
}

void stroke_round(Graphics& gfx, REAL x, REAL y, REAL w, REAL h, REAL r, Color c, REAL s = 1.0f) {
    GraphicsPath p; add_round_rect(p, x, y, w, h, r);
    Pen pen(c, s);
    gfx.DrawPath(&pen, &p);
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

REAL measure_w(Graphics& gfx, const wchar_t* text, const Font& font) {
    RectF bounds(0, 0, 10000, 1000);
    RectF out;
    gfx.MeasureString(text, -1, &font, bounds, &out);
    return out.Width;
}

float lerp(float a, float b, float t) { return a + (b - a) * t; }
float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
RGBA mix(RGBA a, RGBA b, float t) {
    t = clamp01(t);
    return {
        (BYTE)(a.r + (b.r - a.r) * t),
        (BYTE)(a.g + (b.g - a.g) * t),
        (BYTE)(a.b + (b.b - a.b) * t),
        (BYTE)(a.a + (b.a - a.a) * t),
    };
}

Btn* btn_at(POINT pt) {
    Btn* btns[] = { &g.btn_primary, &g.btn_reapply, &g.btn_log, &g.btn_remove };
    for (Btn* b : btns) {
        if (b->enabled && PtInRect(&b->rc, pt)) return b;
    }
    return nullptr;
}

void ensure_anim_timer() {
    if (g.anim_timer) return;
    g.anim_timer = SetTimer(g.hwnd_root, 1, 16, nullptr);
}

bool advance_animations() {
    bool active = false;
    const float SPEED = 0.22f;
    bool dirty = false;

    Btn* btns[] = { &g.btn_primary, &g.btn_reapply, &g.btn_log, &g.btn_remove };
    for (Btn* b : btns) {
        float old_h = b->hover_t;
        float old_p = b->press_t;
        b->hover_t = lerp(b->hover_t, b->hover_targ, SPEED);
        b->press_t = lerp(b->press_t, b->press_targ, SPEED);
        if (fabsf(b->hover_t - b->hover_targ) < 0.005f) b->hover_t = b->hover_targ;
        if (fabsf(b->press_t - b->press_targ) < 0.005f) b->press_t = b->press_targ;
        if (b->hover_t != b->hover_targ || b->press_t != b->press_targ) active = true;
        if (old_h != b->hover_t || old_p != b->press_t) dirty = true;
    }

    float old_f = g.footer_t;
    g.footer_t = lerp(g.footer_t, g.footer_targ, SPEED);
    if (fabsf(g.footer_t - g.footer_targ) < 0.005f) g.footer_t = g.footer_targ;
    if (g.footer_t != g.footer_targ) active = true;
    if (old_f != g.footer_t) dirty = true;

    if (dirty) InvalidateRect(g.hwnd_root, nullptr, FALSE);
    return active;
}

const wchar_t* button_label(int id) {
    switch (id) {
        case ID_BTN_PRIMARY: return primary_button_text();
        case ID_BTN_REAPPLY: return L"Reapply patch";
        case ID_BTN_LOG:     return L"Open logs";
        case ID_BTN_REMOVE:  return L"Uninstall";
    }
    return L"";
}

void draw_button(Graphics& gfx, const Btn& b) {
    int  id       = b.id;
    bool disabled = !b.enabled;
    float hover   = b.hover_t;
    float press   = b.press_t;

    REAL inset = press * 1.5f;
    REAL bx = b.rc.left + inset;
    REAL by = b.rc.top  + inset;
    REAL bw = (REAL)(b.rc.right - b.rc.left) - inset * 2;
    REAL bh = (REAL)(b.rc.bottom - b.rc.top) - inset * 2;
    REAL radius = 8.0f;

    auto draw_fill = [&](RGBA color, float alpha = 1.0f) {
        Color c(BYTE(color.a * alpha), color.r, color.g, color.b);
        fill_round(gfx, bx, by, bw, bh, radius, c);
    };
    auto draw_border = [&](Color c) {
        stroke_round(gfx, bx + 0.5f, by + 0.5f, bw - 1.0f, bh - 1.0f, radius, c, 1.0f);
    };

    if (id == ID_BTN_PRIMARY) {
        if (disabled) {
            draw_fill(C_BTN);
        } else {
            RGBA fill = mix(C_ACCENT, C_ACCENT_HOV, hover);
            fill = mix(fill, C_ACCENT_DEEP, press);
            draw_fill(fill);
            draw_border(gp(C_ACCENT_BDR));
        }
    } else if (id == ID_BTN_REMOVE) {
        if (disabled) {
            draw_border(gpA(C_DANGER_BDR, 0x80));
        } else {
            if (hover > 0.01f) draw_fill(RGBA{0x2A, 0x18, 0x18, 0xFF}, hover);
            draw_border(gp(C_DANGER_BDR));
        }
    } else {
        if (disabled) {
            draw_fill(C_BTN, 0.5f);
            draw_border(gpA(C_BTN_BORDER, 0x80));
        } else {
            draw_fill(mix(C_BTN, C_HOVER, hover));
            draw_border(gp(C_BTN_BORDER));
        }
    }

    // Icon + label
    FontFamily ffText(L"Segoe UI Variable Display");
    FontFamily ffIcon(g.icon_font_fluent ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets");

    RGBA text_color = C_BTN_TEXT;
    if (disabled) text_color = C_TEXT_SUBTLE;
    else if (id == ID_BTN_PRIMARY) text_color = C_ACCENT_INK;
    else if (id == ID_BTN_REMOVE) text_color = mix(C_DANGER, C_DANGER_HOV, hover);

    REAL text_pt = (id == ID_BTN_PRIMARY) ? 11.5f : 10.5f;
    Font font(&ffText, text_pt, FontStyleRegular, UnitPoint);
    Font icon_font(&ffIcon, text_pt, FontStyleRegular, UnitPoint);

    const wchar_t* label = button_label(id);
    const wchar_t* icon = nullptr;
    if (id == ID_BTN_PRIMARY) icon = primary_button_icon();
    else if (id == ID_BTN_REAPPLY) icon = IC_REFRESH;
    else if (id == ID_BTN_LOG) icon = IC_DOC;
    else if (id == ID_BTN_REMOVE) icon = IC_TRASH;

    REAL icon_w = measure_w(gfx, icon, icon_font);
    REAL label_w = measure_w(gfx, label, font);
    REAL gap = (id == ID_BTN_PRIMARY) ? 7.0f : 6.0f;
    REAL total = icon_w + gap + label_w;
    REAL group_x = (bw - total) / 2.0f + bx;

    draw_text(gfx, icon, icon_font, group_x, by, icon_w + 2.0f, bh, gp(text_color),
              StringAlignmentNear, StringAlignmentCenter);
    draw_text(gfx, label, font, group_x + icon_w + gap, by, label_w + 2.0f, bh, gp(text_color),
              StringAlignmentNear, StringAlignmentCenter);
}

constexpr int PAD        = 28;
constexpr int HEAD_Y     = 32;
constexpr int DIV_Y      = HEAD_Y + 40;
constexpr int CARD_Y     = 88;
constexpr int CARD_H     = 86;
constexpr int PRIMARY_Y  = CARD_Y + CARD_H + 18;
constexpr int PRIMARY_H  = 48;
constexpr int SEC_GAP    = 8;
constexpr int SEC_Y      = PRIMARY_Y + PRIMARY_H + 12;
constexpr int SEC_H      = 42;
constexpr int FOOT_DIV_Y = SEC_Y + SEC_H + 22;
constexpr int FOOT_ROW_Y = FOOT_DIV_Y + 16;
constexpr int FOOT_BTN_H = 42;
constexpr int FOOT_BTN_W = 128;

void paint_status_card(Graphics& gfx, int W) {
    REAL cx = (REAL)PAD;
    REAL cy = (REAL)CARD_Y;
    REAL cw = (REAL)(W - PAD * 2);
    REAL ch = (REAL)CARD_H;
    REAL r  = 10.0f;

    fill_round(gfx, cx, cy, cw, ch, r, gp(C_CARD));
    stroke_round(gfx, cx + 0.5f, cy + 0.5f, cw - 1.0f, ch - 1.0f, r, gp(C_CARD_BORDER), 1.0f);

    RGBA sc = status_color();
    REAL badge = 44.0f;
    REAL badge_x = cx + 18.0f;
    REAL badge_y = cy + (ch - badge) / 2.0f;
    fill_round(gfx, badge_x, badge_y, badge, badge, 11.0f, gpA(sc, 0x22));

    FontFamily ffIcon(g.icon_font_fluent ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets");
    Font fIcon(&ffIcon, 17.0f, FontStyleRegular, UnitPoint);
    draw_text(gfx, status_icon(), fIcon, badge_x, badge_y, badge, badge,
              gp(sc), StringAlignmentCenter, StringAlignmentCenter);

    FontFamily ff(L"Segoe UI Variable Display");
    Font fTitle(&ff, 12.5f, FontStyleBold, UnitPoint);
    Font fSub(&ff, 9.5f, FontStyleRegular, UnitPoint);

    const wchar_t* title = g.busy ? g.busy_text.c_str() : status_title();

    REAL text_x = badge_x + badge + 16.0f;
    draw_text(gfx, title, fTitle,
              text_x, cy + 18.0f, cw - (text_x - cx) - 16.0f, 22.0f,
              gp(C_TEXT), StringAlignmentNear, StringAlignmentNear);

    wchar_t sub[160] = {};
    if (g.status == Status::DiscordMissing) {
        wcscpy_s(sub, 160, L"Install Discord, then reopen this panel.");
    } else if (!g.discord_version.empty()) {
        swprintf(sub, 160, L"Discord Stable  ·  %ls", g.discord_version.c_str());
    }
    draw_text(gfx, sub, fSub,
              text_x, cy + 44.0f, cw - (text_x - cx) - 16.0f, 20.0f,
              gp(C_TEXT_DIM), StringAlignmentNear, StringAlignmentNear);
}

void paint(HWND hwnd, HDC hdc) {
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;

    Graphics gfx(hdc);
    gfx.SetSmoothingMode(SmoothingModeAntiAlias);
    gfx.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // Background
    SolidBrush bg(gp(C_BG));
    gfx.FillRectangle(&bg, 0, 0, W, H);

    FontFamily ff(L"Segoe UI Variable Display");

    // Title
    {
        Font fTitle(&ff, 19.0f, FontStyleBold, UnitPoint);
        draw_text(gfx, L"DiscordNitroPatcher", fTitle,
                  (REAL)PAD, (REAL)HEAD_Y, (REAL)(W - PAD * 2), 36.0f,
                  gp(C_TEXT), StringAlignmentNear, StringAlignmentNear);

            Font fVer(&ff, 9.0f, FontStyleBold, UnitPoint);
        wchar_t vbuf[16];
        swprintf(vbuf, 16, L"v%ls", VERSION);
        REAL vw = measure_w(gfx, vbuf, fVer) + 18.0f;
        REAL vx = (REAL)(W - PAD) - vw;
        REAL vy = (REAL)(HEAD_Y + 8);
        fill_round(gfx, vx, vy, vw, 20.0f, 5.0f, gp(C_BTN));
        stroke_round(gfx, vx + 0.5f, vy + 0.5f, vw - 1.0f, 19.0f, 5.0f, gp(C_BTN_BORDER), 1.0f);
        draw_text(gfx, vbuf, fVer, vx, vy, vw, 20.0f, gp(C_TEXT_DIM),
                  StringAlignmentCenter, StringAlignmentCenter);
    }

    {
        Pen line(gp(C_CARD_BORDER), 1.0f);
        gfx.DrawLine(&line, (REAL)PAD, (REAL)(HEAD_Y + 40), (REAL)(W - PAD), (REAL)(HEAD_Y + 40));
    }

    paint_status_card(gfx, W);

    draw_button(gfx, g.btn_primary);
    draw_button(gfx, g.btn_reapply);
    draw_button(gfx, g.btn_log);
    draw_button(gfx, g.btn_remove);

    {
        Pen line(gp(C_CARD_BORDER), 1.0f);
        gfx.DrawLine(&line, (REAL)PAD, (REAL)FOOT_DIV_Y, (REAL)(W - PAD), (REAL)FOOT_DIV_Y);

        Font fFoot(&ff, 9.0f, FontStyleRegular, UnitPoint);
        REAL tw = measure_w(gfx, FOOTER_URL, fFoot);
        REAL fx = (REAL)PAD;
        REAL fy = (REAL)FOOT_ROW_Y;
        RGBA fc = mix(C_ACCENT_DEEP, C_ACCENT_HOV, g.footer_t);
        draw_text(gfx, FOOTER_URL, fFoot, fx, fy, tw + 4.0f, (REAL)FOOT_BTN_H,
                  gp(fc), StringAlignmentNear, StringAlignmentCenter);
        g.footer_rc = { (LONG)fx, (LONG)(fy + 8), (LONG)(fx + tw), (LONG)(fy + FOOT_BTN_H - 8) };
    }
}

void layout_controls() {
    if (!g.hwnd_root) return;
    RECT rc; GetClientRect(g.hwnd_root, &rc);
    int W = rc.right;

    int bw = W - PAD * 2;
    int half = (bw - SEC_GAP) / 2;
    int log_w = bw - half - SEC_GAP;
    int log_x = PAD + half + SEC_GAP;
    int rem_x = W - PAD - FOOT_BTN_W;

    g.btn_primary.id = ID_BTN_PRIMARY;
    g.btn_reapply.id = ID_BTN_REAPPLY;
    g.btn_log.id     = ID_BTN_LOG;
    g.btn_remove.id  = ID_BTN_REMOVE;

    g.btn_primary.rc = { PAD,    PRIMARY_Y, PAD + bw,        PRIMARY_Y + PRIMARY_H };
    g.btn_reapply.rc = { PAD,    SEC_Y,     PAD + half,      SEC_Y + SEC_H };
    g.btn_log.rc     = { log_x,  SEC_Y,     log_x + log_w,   SEC_Y + SEC_H };
    g.btn_remove.rc  = { rem_x,  FOOT_ROW_Y, rem_x + FOOT_BTN_W, FOOT_ROW_Y + FOOT_BTN_H };
}

void refresh_buttons() {
    g.btn_primary.enabled = !g.busy && g.status != Status::DiscordMissing;
    g.btn_reapply.enabled = !g.busy && g.status != Status::DiscordMissing &&
                                       g.status != Status::NotInstalled;
    g.btn_log.enabled     = !g.busy;
    g.btn_remove.enabled  = !g.busy && g.status != Status::NotInstalled;
    InvalidateRect(g.hwnd_root, nullptr, FALSE);
}

void set_busy(const wchar_t* what) {
    g.busy = true;
    g.busy_text = what;
    refresh_buttons();
    UpdateWindow(g.hwnd_root);
}
void clear_busy() {
    g.busy = false;
    g.busy_text.clear();
    refresh_buttons();
}

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
    } else if (g.status == Status::Installed) {
        launch_discord();
        return;
    } else {
        set_busy(L"Applying patch...");
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

void action_reapply() {
    if (g.status != Status::Installed && g.status != Status::NeedsRepatch) return;
    set_busy(L"Reapplying patch...");
    auto app_dir = find_latest_discord_app_dir();
    if (app_dir) {
        kill_discord_processes();
        Sleep(300);
        apply_patch(*app_dir);
        Sleep(200);
        launch_discord();
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

void action_footer() {
    ShellExecuteW(g.hwnd_root, L"open",
        L"https://github.com/C0D0X/DiscordNitroPatcher",
        nullptr, nullptr, SW_SHOWNORMAL);
}

bool font_family_exists(const wchar_t* name) {
    FontFamily ff(name);
    return ff.GetLastStatus() == Ok;
}

LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_CREATE: {
            g.brush_bg = CreateSolidBrush(RGB(C_BG.r, C_BG.g, C_BG.b));
            g.start_tick = GetTickCount();
            g.icon_font_fluent = font_family_exists(L"Segoe Fluent Icons");

            update_status();
            layout_controls();
            refresh_buttons();
            return 0;
        }
        case WM_TIMER:
            if (wp == 1) {
                if (!advance_animations()) {
                    KillTimer(hwnd, 1);
                    g.anim_timer = 0;
                }
            }
            return 0;
        case WM_SIZE:
            layout_controls();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
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
        case WM_MOUSEMOVE: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            Btn* hot = btn_at(pt);
            Btn* btns[] = { &g.btn_primary, &g.btn_reapply, &g.btn_log, &g.btn_remove };
            for (Btn* b : btns) {
                float targ = (b == hot) ? 1.0f : 0.0f;
                if (b->hover_targ != targ) { b->hover_targ = targ; ensure_anim_timer(); }
            }
            float ftarg = PtInRect(&g.footer_rc, pt) ? 1.0f : 0.0f;
            if (g.footer_targ != ftarg) { g.footer_targ = ftarg; ensure_anim_timer(); }

            if (!g.mouse_tracking) {
                TRACKMOUSEEVENT t{ sizeof(t), TME_LEAVE, hwnd, 0 };
                TrackMouseEvent(&t);
                g.mouse_tracking = true;
            }
            return 0;
        }
        case WM_MOUSELEAVE: {
            Btn* btns[] = { &g.btn_primary, &g.btn_reapply, &g.btn_log, &g.btn_remove };
            for (Btn* b : btns) b->hover_targ = 0.0f;
            g.footer_targ = 0.0f;
            g.mouse_tracking = false;
            ensure_anim_timer();
            return 0;
        }
        case WM_LBUTTONDOWN: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            Btn* hit = btn_at(pt);
            if (hit) {
                g.pressed = hit;
                hit->press_targ = 1.0f;
                SetCapture(hwnd);
                ensure_anim_timer();
            }
            return 0;
        }
        case WM_LBUTTONUP: {
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            Btn* pressed = g.pressed;
            if (pressed) {
                pressed->press_targ = 0.0f;
                g.pressed = nullptr;
                ReleaseCapture();
                ensure_anim_timer();
                if (pressed->enabled && PtInRect(&pressed->rc, pt)) {
                    switch (pressed->id) {
                        case ID_BTN_PRIMARY: action_primary();   break;
                        case ID_BTN_REAPPLY: action_reapply();   break;
                        case ID_BTN_LOG:     action_open_log();  break;
                        case ID_BTN_REMOVE:  action_uninstall(); break;
                    }
                    return 0;
                }
            }
            if (PtInRect(&g.footer_rc, pt)) action_footer();
            return 0;
        }
        case WM_SETCURSOR: {
            if ((HWND)wp == hwnd) {
                POINT pt; GetCursorPos(&pt); ScreenToClient(hwnd, &pt);
                if (btn_at(pt) || PtInRect(&g.footer_rc, pt)) {
                    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32649)));
                    return TRUE;
                }
            }
            break;
        }
        case WM_DESTROY:
            if (g.anim_timer) { KillTimer(hwnd, 1); g.anim_timer = 0; }
            if (g.brush_bg)   { DeleteObject(g.brush_bg); g.brush_bg = nullptr; }
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

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

    const int W = 520, H = 438;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;

    HWND hwnd = CreateWindowExW(0, L"DnpMainWnd", L"DiscordNitroPatcher",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN,
        sx, sy, W, H,
        nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd) { GdiplusShutdown(g.gdiplus_token); return 1; }
    g.hwnd_root = hwnd;

    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    int backdrop = DWMSBT_NONE;
    DwmSetWindowAttribute(hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));

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
