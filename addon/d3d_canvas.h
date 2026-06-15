#pragma once
//
// d3d_canvas.h -- D3D11 swap-chain + Direct2D/DirectWrite compositor on
// top of Discord's overlay HWND.
//
// Why this lives in its own TU:
//   * Keeps every D3D / DXGI / D2D / DWrite header out of rc_core.cpp,
//     which then stays cheap to recompile.
//   * The "create / resize / draw / teardown" lifecycle is isolated so the
//     render loop in rc_core can call into a clean façade without owning
//     any COM pointers itself.
//
// All drawing is via Direct2D primitives -- FillEllipse for radar dots,
// DrawLine for ESP edges, DrawRectangle for boxes, DrawTextLayout for
// names. No ImGui anywhere. The wire data lands as a proto::Scene that
// d3d_canvas projects through the included viewproj matrix.
//

#include <cstdint>

#include "proto.h"

// Forward declarations -- callers don't need the heavy D3D/D2D headers.
struct ID3D11Device;
struct ID3D11DeviceContext;
struct IDXGISwapChain;
struct ID3D11RenderTargetView;
struct ID2D1Factory1;
struct ID2D1Device;
struct ID2D1DeviceContext;
struct ID2D1Bitmap1;
struct ID2D1SolidColorBrush;
struct IDWriteFactory;
struct IDWriteTextFormat;

using HWND_ = struct HWND__*;   // opaque alias so callers don't pull windows.h

namespace rc {

// One Canvas owns one swap chain bound to one HWND. Created lazily on the
// first AWAKE transition so we don't burn GPU memory while dormant.
class Canvas {
public:
    Canvas();
    ~Canvas();

    Canvas(const Canvas&)            = delete;
    Canvas& operator=(const Canvas&) = delete;

    // True once the swap chain + D2D context are live. Until then, draw()
    // is a no-op so the render loop can call it unconditionally.
    bool ready() const { return m_ready; }

    // Creates D3D11 device, swap chain on `hwnd`, and the D2D/DWrite
    // facade bound to the back buffer. Returns false on COM failure --
    // the render loop should back off and retry on the next tick.
    bool initialise(HWND_ hwnd);

    // Tears down COM objects in the reverse order of creation. Safe to
    // call repeatedly; safe to call on a half-initialised Canvas.
    void teardown();

    // Handles HWND resize since Discord's overlay can grow/shrink with the
    // monitor layout. Cheap when the size hasn't changed.
    void resize_if_needed(HWND_ hwnd);

    // Draws one frame from the supplied scene snapshot. Caller is the
    // render thread; `scene` is the SceneState's current front buffer.
    // No-op if !ready() or if scene->header.type != Scene.
    void draw(const proto::Scene* scene);

private:
    // COM lifecycle helpers (small, defined in d3d_canvas.cpp).
    bool create_device_and_swapchain(HWND_ hwnd);
    bool create_d2d_target();
    bool create_dwrite();
    void release_back_buffer_views();

    // Swap-chain / D3D state.
    ID3D11Device*         m_device   = nullptr;
    ID3D11DeviceContext*  m_ctx      = nullptr;
    IDXGISwapChain*       m_swap     = nullptr;
    ID3D11RenderTargetView* m_rtv    = nullptr;

    // D2D state.
    ID2D1Factory1*        m_d2d_factory = nullptr;
    ID2D1Device*          m_d2d_device  = nullptr;
    ID2D1DeviceContext*   m_d2d_ctx     = nullptr;
    ID2D1Bitmap1*         m_d2d_target  = nullptr;

    // Pre-created brushes -- creating an ID2D1SolidColorBrush per draw is
    // ~20x slower than recolouring a cached one. Keep one per role.
    ID2D1SolidColorBrush* m_brush_enemy   = nullptr;
    ID2D1SolidColorBrush* m_brush_friend  = nullptr;
    ID2D1SolidColorBrush* m_brush_ai      = nullptr;
    ID2D1SolidColorBrush* m_brush_downed  = nullptr;
    ID2D1SolidColorBrush* m_brush_text    = nullptr;
    ID2D1SolidColorBrush* m_brush_minimap = nullptr;

    // DWrite state.
    IDWriteFactory*       m_dwrite      = nullptr;
    IDWriteTextFormat*    m_text_fmt    = nullptr;

    // Cached dimensions (last seen client rect).
    uint32_t m_width  = 0;
    uint32_t m_height = 0;

    bool m_ready = false;
};

} // namespace rc
