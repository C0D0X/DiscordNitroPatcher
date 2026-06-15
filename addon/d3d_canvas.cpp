//
// d3d_canvas.cpp -- D3D11 swap chain + Direct2D compositor on top of
// Discord's overlay HWND, plus the radar render layer.
//
// One Canvas owns:
//
//     HWND -> IDXGISwapChain -> ID3D11Device/Context
//                            -> back-buffer ID3D11Texture2D
//                                  |
//                                  v
//                    IDXGISurface -> ID2D1Bitmap1 (D2D target)
//                                  ^
//     ID2D1Factory1 -> ID2D1Device -> ID2D1DeviceContext (draw API)
//
//     IDWriteFactory -> IDWriteTextFormat (one shared format)
//
// Transparency
// ------------
// The swap chain is cleared to (0,0,0,0). Discord's overlay window is
// already transparent + topmost, so the cleared pixels stay see-through
// and only the D2D primitives we draw appear on screen.
//
// Capture protection
// ------------------
// We deliberately do NOT call SetWindowDisplayAffinity here. That call
// lives in shim_main.js via Electron's BrowserWindow.setContentProtection,
// so the Win32 invocation originates from Discord's own native code path
// (indistinguishable from streamer-mode-driven calls). Keeping it out of
// the addon eliminates a suspicious import from `dumpbin /imports`.
//

#include "d3d_canvas.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cwchar>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite.h>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")

namespace rc {

namespace {

// Small helper: release a COM pointer and null it out so a re-entrant
// teardown is safe.
template<typename T>
void safe_release(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

// Colour palette. Kept compact and adjustable in one place.
D2D1_COLOR_F COL_ENEMY   = D2D1::ColorF(0xff4040, 1.0f);
D2D1_COLOR_F COL_FRIEND  = D2D1::ColorF(0x40ff60, 1.0f);
D2D1_COLOR_F COL_AI      = D2D1::ColorF(0xffd040, 1.0f);
D2D1_COLOR_F COL_DOWNED  = D2D1::ColorF(0xc060ff, 1.0f);
D2D1_COLOR_F COL_TEXT    = D2D1::ColorF(0xffffff, 1.0f);
D2D1_COLOR_F COL_MINIMAP = D2D1::ColorF(0x80c0ff, 0.85f);

// Convenience for casting our opaque HWND_ back to the real Win32 HWND.
inline HWND from_opaque(HWND_ h) { return reinterpret_cast<HWND>(h); }

// World-space -> screen projection via the row-major viewproj matrix
// shipped on the wire. Returns (sx, sy, visible) -- visible is false if
// the point is behind the camera or off-screen.
struct ProjPt { float x, y; bool visible; };

ProjPt project(const float vp[16],
               float wx, float wy, float wz,
               float screen_w, float screen_h)
{
    // Row-major: each row is dotted with (wx, wy, wz, 1).
    float xc = vp[0]*wx + vp[1]*wy + vp[2]*wz + vp[3];
    float yc = vp[4]*wx + vp[5]*wy + vp[6]*wz + vp[7];
    // vp row 2 (z) -- we don't need depth for ESP, skip the multiply.
    float wc = vp[12]*wx + vp[13]*wy + vp[14]*wz + vp[15];

    ProjPt p{ 0.0f, 0.0f, false };
    if (wc <= 0.0001f) return p;

    float nx = xc / wc;
    float ny = yc / wc;

    p.x       = (nx * 0.5f + 0.5f) * screen_w;
    p.y       = (1.0f - (ny * 0.5f + 0.5f)) * screen_h;
    p.visible = (p.x >= 0 && p.x < screen_w && p.y >= 0 && p.y < screen_h);
    return p;
}

// Pick the brush that matches the entity's role flags.
ID2D1SolidColorBrush* pick_brush(uint16_t flags,
                                 ID2D1SolidColorBrush* enemy,
                                 ID2D1SolidColorBrush* friendly,
                                 ID2D1SolidColorBrush* ai,
                                 ID2D1SolidColorBrush* downed)
{
    if (flags & proto::F_DOWNED) return downed;
    if (flags & proto::F_AI)     return ai;
    if (flags & proto::F_ENEMY)  return enemy;
    return friendly;
}

} // namespace

// ---- Lifecycle -------------------------------------------------------------

Canvas::Canvas()  = default;
Canvas::~Canvas() { teardown(); }

bool Canvas::initialise(HWND_ hwnd) {
    if (m_ready) return true;
    if (!create_device_and_swapchain(hwnd)) { teardown(); return false; }
    if (!create_d2d_target())               { teardown(); return false; }
    if (!create_dwrite())                   { teardown(); return false; }
    m_ready = true;
    return true;
}

void Canvas::teardown() {
    // Reverse order of acquisition. safe_release is a no-op for nulls so
    // double-teardown is fine.
    safe_release(m_text_fmt);
    safe_release(m_dwrite);

    safe_release(m_brush_enemy);
    safe_release(m_brush_friend);
    safe_release(m_brush_ai);
    safe_release(m_brush_downed);
    safe_release(m_brush_text);
    safe_release(m_brush_minimap);

    safe_release(m_d2d_target);
    safe_release(m_d2d_ctx);
    safe_release(m_d2d_device);
    safe_release(m_d2d_factory);

    safe_release(m_rtv);
    safe_release(m_swap);
    safe_release(m_ctx);
    safe_release(m_device);

    m_width  = 0;
    m_height = 0;
    m_ready  = false;
}

// ---- Bring-up helpers ------------------------------------------------------

bool Canvas::create_device_and_swapchain(HWND_ hwnd) {
    RECT rc{};
    GetClientRect(from_opaque(hwnd), &rc);
    m_width  = (UINT)(rc.right  - rc.left);
    m_height = (UINT)(rc.bottom - rc.top);
    if (m_width == 0 || m_height == 0) return false;

    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount        = 2;
    sd.BufferDesc.Format  = DXGI_FORMAT_B8G8R8A8_UNORM;  // BGRA for D2D
    sd.BufferDesc.Width   = m_width;
    sd.BufferDesc.Height  = m_height;
    sd.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow       = from_opaque(hwnd);
    sd.SampleDesc.Count   = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed           = TRUE;
    sd.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags              = 0;

    // D2D interop requires BGRA support on the underlying D3D device.
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;

    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
    };

    D3D_FEATURE_LEVEL got{};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr,
        D3D_DRIVER_TYPE_HARDWARE,
        nullptr,
        flags,
        levels, (UINT)std::size(levels),
        D3D11_SDK_VERSION,
        &sd,
        &m_swap,
        &m_device,
        &got,
        &m_ctx);
    if (FAILED(hr)) return false;

    return true;
}

bool Canvas::create_d2d_target() {
    // 1) D2D factory + device on top of our D3D device's DXGI device.
    HRESULT hr = D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1),
        reinterpret_cast<void**>(&m_d2d_factory));
    if (FAILED(hr)) return false;

    IDXGIDevice* dxgi_device = nullptr;
    hr = m_device->QueryInterface(__uuidof(IDXGIDevice),
                                  reinterpret_cast<void**>(&dxgi_device));
    if (FAILED(hr) || !dxgi_device) return false;

    hr = m_d2d_factory->CreateDevice(dxgi_device, &m_d2d_device);
    safe_release(dxgi_device);
    if (FAILED(hr)) return false;

    hr = m_d2d_device->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
        &m_d2d_ctx);
    if (FAILED(hr)) return false;

    // 2) Bind the swap chain's back buffer as a D2D bitmap target.
    IDXGISurface* surface = nullptr;
    hr = m_swap->GetBuffer(0, __uuidof(IDXGISurface),
                           reinterpret_cast<void**>(&surface));
    if (FAILED(hr) || !surface) return false;

    D2D1_BITMAP_PROPERTIES1 bp{};
    bp.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                       D2D1_ALPHA_MODE_PREMULTIPLIED);
    bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET |
                       D2D1_BITMAP_OPTIONS_CANNOT_DRAW;

    hr = m_d2d_ctx->CreateBitmapFromDxgiSurface(surface, &bp, &m_d2d_target);
    safe_release(surface);
    if (FAILED(hr)) return false;

    m_d2d_ctx->SetTarget(m_d2d_target);
    m_d2d_ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

    // 3) Cached brushes -- creating per-draw would dominate the frame.
    auto mk_brush = [&](const D2D1_COLOR_F& col,
                        ID2D1SolidColorBrush** out) -> bool {
        return SUCCEEDED(m_d2d_ctx->CreateSolidColorBrush(col, out));
    };
    if (!mk_brush(COL_ENEMY,   &m_brush_enemy))   return false;
    if (!mk_brush(COL_FRIEND,  &m_brush_friend))  return false;
    if (!mk_brush(COL_AI,      &m_brush_ai))      return false;
    if (!mk_brush(COL_DOWNED,  &m_brush_downed))  return false;
    if (!mk_brush(COL_TEXT,    &m_brush_text))    return false;
    if (!mk_brush(COL_MINIMAP, &m_brush_minimap)) return false;

    return true;
}

bool Canvas::create_dwrite() {
    HRESULT hr = DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(&m_dwrite));
    if (FAILED(hr)) return false;

    hr = m_dwrite->CreateTextFormat(
        L"Segoe UI",
        nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        12.0f,
        L"en-us",
        &m_text_fmt);
    if (FAILED(hr)) return false;

    m_text_fmt->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    m_text_fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
    return true;
}

void Canvas::release_back_buffer_views() {
    // D2D holds the target as an ID2D1Bitmap1 referencing the swap chain's
    // back buffer. Both must be released before ResizeBuffers.
    if (m_d2d_ctx) m_d2d_ctx->SetTarget(nullptr);
    safe_release(m_d2d_target);
    safe_release(m_rtv);
}

// ---- Resize ----------------------------------------------------------------

void Canvas::resize_if_needed(HWND_ hwnd) {
    if (!m_ready || !m_swap) return;

    RECT rc{};
    GetClientRect(from_opaque(hwnd), &rc);
    UINT w = (UINT)(rc.right  - rc.left);
    UINT h = (UINT)(rc.bottom - rc.top);
    if (w == 0 || h == 0)                  return;
    if (w == m_width && h == m_height)     return;

    release_back_buffer_views();

    HRESULT hr = m_swap->ResizeBuffers(
        0, w, h, DXGI_FORMAT_UNKNOWN, 0);
    if (FAILED(hr)) { teardown(); return; }

    // Re-bind the D2D target on the fresh back buffer.
    IDXGISurface* surface = nullptr;
    if (FAILED(m_swap->GetBuffer(0, __uuidof(IDXGISurface),
                                 reinterpret_cast<void**>(&surface))))
    {
        teardown();
        return;
    }
    D2D1_BITMAP_PROPERTIES1 bp{};
    bp.pixelFormat   = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                         D2D1_ALPHA_MODE_PREMULTIPLIED);
    bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET |
                       D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    hr = m_d2d_ctx->CreateBitmapFromDxgiSurface(surface, &bp, &m_d2d_target);
    safe_release(surface);
    if (FAILED(hr)) { teardown(); return; }

    m_d2d_ctx->SetTarget(m_d2d_target);

    m_width  = w;
    m_height = h;
}

// ---- Draw ------------------------------------------------------------------
//
// One D2D batch per frame: BeginDraw -> clear -> primitives -> EndDraw,
// then Present. A nullptr scene (dormant / panic) still goes through the
// clear+Present cycle so the previously-rendered frame doesn't linger
// frozen on screen.

void Canvas::draw(const proto::Scene* scene) {
    if (!m_ready) return;

    m_d2d_ctx->BeginDraw();
    m_d2d_ctx->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    if (scene && scene->entity_count > 0) {

        // Game-side declared resolution. If the scene packet was filled
        // before the game knew its viewport (rare) we fall back to the
        // window's own size so projection still produces sane pixels.
        float sw = (scene->screen_w > 0) ? (float)scene->screen_w : (float)m_width;
        float sh = (scene->screen_h > 0) ? (float)scene->screen_h : (float)m_height;

        // Discord's overlay HWND is monitor-sized, which matches the
        // game's swap chain on a borderless / fullscreen client. If the
        // user runs a windowed game whose client area is smaller than
        // the desktop, the projection will still produce coords in the
        // game's space; we scale to canvas coords on the fly.
        float scale_x = (float)m_width  / sw;
        float scale_y = (float)m_height / sh;

        for (uint16_t i = 0; i < scene->entity_count; ++i) {
            const proto::WireEntity& e = scene->entities[i];
            if (!(e.flags & proto::F_ALIVE)) continue;

            // World -> game-space screen.
            ProjPt feet = project(scene->viewproj,
                                  e.feet[0], e.feet[1], e.feet[2],
                                  sw, sh);
            ProjPt head = project(scene->viewproj,
                                  e.head[0], e.head[1], e.head[2],
                                  sw, sh);
            if (!feet.visible && !head.visible) continue;

            // Scale into the canvas pixel space.
            feet.x *= scale_x; feet.y *= scale_y;
            head.x *= scale_x; head.y *= scale_y;

            float box_h = std::fabs(feet.y - head.y);
            if (box_h < 6.0f) box_h = 6.0f;
            float box_w = box_h * 0.5f;
            float box_x = head.x - box_w * 0.5f;
            float box_y = head.y;

            ID2D1SolidColorBrush* brush =
                pick_brush(e.flags,
                           m_brush_enemy,
                           m_brush_friend,
                           m_brush_ai,
                           m_brush_downed);

            D2D1_RECT_F box{
                box_x,
                box_y,
                box_x + box_w,
                box_y + box_h
            };
            m_d2d_ctx->DrawRectangle(box, brush, 1.5f);

            // Health bar (left of box). Drawn whenever a health value
            // came across the wire; downed/AI flags zero this out on
            // the loader side already.
            if (e.health > 0.0f) {
                float frac = e.health / 100.0f;
                if (frac > 1.0f) frac = 1.0f;
                D2D1_RECT_F hp_bg{
                    box_x - 6.0f, box_y,
                    box_x - 2.0f, box_y + box_h
                };
                D2D1_RECT_F hp_fg{
                    box_x - 6.0f, box_y + box_h * (1.0f - frac),
                    box_x - 2.0f, box_y + box_h
                };
                m_d2d_ctx->FillRectangle(hp_bg, m_brush_text);  // muted bg
                m_d2d_ctx->FillRectangle(hp_fg, brush);
            }

            // Name + distance, centred under the box.
            if (e.name[0] != 0) {
                wchar_t buf[64];
                int distance_int = (int)e.distance;
                _snwprintf_s(buf, _TRUNCATE,
                             L"%hs [%dm]",
                             e.name, distance_int);

                D2D1_RECT_F text_box{
                    box_x - 40.0f,
                    box_y + box_h + 1.0f,
                    box_x + box_w + 40.0f,
                    box_y + box_h + 18.0f
                };
                m_d2d_ctx->DrawText(
                    buf,
                    (UINT32)std::wcslen(buf),
                    m_text_fmt,
                    text_box,
                    m_brush_text,
                    D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        }
    }

    HRESULT hr = m_d2d_ctx->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        // Device-lost equivalent for D2D -- next frame will rebuild.
        teardown();
        return;
    }

    // Present(1, 0): vsync, no flags. Low-latency and tear-free; on a
    // composited Windows desktop the DWM pulls flips at the monitor
    // rate regardless.
    m_swap->Present(1, 0);
}

} // namespace rc
