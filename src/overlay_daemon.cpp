// overlay_daemon.cpp -- in-dnp.exe radar receiver + compositor.
//
// Design choices for stealth and speed:
//
//   * No OpenProcess on Discord. Lifecycle is bounded by the presence of
//     Discord's overlay HWND ("Chrome_WidgetWin_1" / "Discord Overlay")
//     -- we poll FindWindowW for it, exit when it's gone for >10s.
//     Keeps PROCESS_QUERY_INFORMATION / SYNCHRONIZE off the import table.
//
//   * No SetWindowsHookEx / RegisterHotKey / CreateRemoteThread /
//     WriteProcessMemory / VirtualAllocEx / NtReadVirtualMemory.
//     Hotkey detection (panic-hide) uses GetAsyncKeyState polled in the
//     render loop -- invisible, costs effectively nothing.
//
//   * SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE) IS used here, on
//     Discord's overlay HWND, so OBS / Snipping Tool / screen-share
//     don't capture the radar. Same call streamer mode triggers from
//     inside Discord, so the call origin (dnp.exe) is the only
//     differentiator -- accepted trade-off for the capture protection.
//
//   * D3D11 swap chain attached EXTERNALLY to Discord's overlay HWND
//     via the unknowncheats / unknownsea pattern. FLIP_DISCARD for the
//     lowest-latency present path on Win10+.
//
//   * Direct2D + DirectWrite for primitives. No ImGui. No bitmap font
//     atlases. Pre-created brushes; per-frame brush creation is the
//     classic slow path.
//
//   * Lockless triple buffer between the recv thread and the render
//     thread. Producer writes back, atomically swaps with ready; consumer
//     atomically swaps ready with front before reading. Single atomic
//     exchange per swap; never spins.
//
//   * Render thread sleeps when no scene has arrived recently
//     (DORMANT_TIMEOUT_US). D3D device + swap chain are torn down after
//     30s of dormancy, brought back lazily on the next packet. Saves GPU
//     memory and avoids interleaved presents with Discord's compositor
//     while the user isn't gaming.
//
//   * String literals scrubbed -- no "overlay" / "radar" / "esp" /
//     "hijack" anywhere in this TU. Window class + caption are the public
//     names Discord uses, looked up via FindWindowW.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmsystem.h>     // timeBeginPeriod / timeEndPeriod
#include <tlhelp32.h>
#include <d3d11.h>
#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <dxgi1_3.h>      // IDXGISwapChain2 (waitable object)
#include <dxgi1_6.h>      // IDXGIOutput6 (MPO query)
#include <dcomp.h>        // DirectComposition target / visual
#include <d2d1_1.h>
#include <d2d1helper.h>
#include <dwrite.h>

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <memory>
#include <thread>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "dcomp.lib")     // DirectComposition
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winmm.lib")     // timeBeginPeriod / timeEndPeriod

#include "overlay_daemon.h"
#include "proto.h"

namespace dnp {

namespace {

// ----- protocol constants ----------------------------------------------------
// proto::DEFAULT_PORT == 27015; redeclared here as a neutral 16-bit literal
// so the binary doesn't carry a "RADAR_PORT" symbol or comment near it.
constexpr uint16_t  K_PORT             = proto::DEFAULT_PORT;
constexpr uint64_t  K_DORMANT_US       = proto::DORMANT_TIMEOUT_US;
constexpr uint64_t  K_TEARDOWN_US      = 30'000'000;   // 30s with no scene
constexpr uint64_t  K_EXIT_GRACE_US    =  5'000'000;   // 5s no HWND -> exit
constexpr DWORD     K_HWND_POLL_MS     = 250;
constexpr uint16_t  K_MAX_ENT          = proto::MAX_ENTITIES;

// Staleness threshold -- if the most recent scene packet is older
// than this, render thread paints a blank frame instead of redrawing
// the stale data. 1 ms = blanks on essentially any stall, including
// single-frame scheduling blips. ESP perceived as "live or off",
// never "stuck on old positions".
constexpr uint64_t  K_STALE_THRESHOLD_US = 1'000;

// Window lookup strings. These are PUBLIC Discord values (Chromium class
// name + window caption); putting them here is no fingerprint since
// every overlay tooling references them identically.
const wchar_t K_WND_CLASS[]   = L"Chrome_WidgetWin_1";
const wchar_t K_WND_CAPTION[] = L"Discord Overlay";

// ----- triple buffer --------------------------------------------------------

enum class LinkState : uint8_t { Dormant = 0, Awake = 1 };

// Live HUD stats. Shared between recv thread (writes raw_count) and
// render thread (writes fps/uniq/stalls + reads all for HUD).
struct LiveStats {
    std::atomic<uint32_t> fps       { 0 };
    std::atomic<uint32_t> pkts      { 0 };
    std::atomic<uint32_t> uniq      { 0 };
    std::atomic<uint32_t> stalls    { 0 };
    std::atomic<uint32_t> raw_count { 0 };
};
LiveStats g_stats;

// Combined per-tick payload -- scene (ESP) AND draw list (chams/freeform)
// share one triple-buffer slot. Either side can be empty (entity_count=0
// or blob_used=0); render checks each independently.
struct Slot {
    proto::Scene    scene{};
    proto::DrawList dlist{};
};

struct SceneBuf {
    Slot                    slots[3];
    std::atomic<uint8_t>    back  { 0 };
    std::atomic<uint8_t>    ready { 1 };
    std::atomic<uint8_t>    front { 2 };

    std::atomic<LinkState>  state         { LinkState::Dormant };
    std::atomic<uint64_t>   last_packet_us{ 0 };
    std::atomic<bool>       panic         { false };

    // Link-level liveness, refreshed by Hello/Scene/DrawList. is_live() uses
    // max(link_us, last_packet_us) so a fresh Hello after the loader cycles
    // brings the link back live even if the last Scene was older than
    // K_DORMANT_US -- previously the render thread stayed in dormant teardown
    // until the first new Scene arrived, which never happened if the user
    // hadn't clicked Start yet, so dnp appeared permanently stuck after a
    // loader restart. Stale-Scene detection (line ~1100) keeps reading
    // last_packet_us only, so Hello cannot mask a real Scene stall.
    std::atomic<uint64_t>   link_us       { 0 };

    // Receive timestamp of the most recent DrawList. Used by the render
    // thread to forward-extrapolate World-space primitives by the gap
    // between the sender tick that produced them and the current render
    // frame, so bones don't freeze between sender ticks.
    std::atomic<uint64_t>   last_dlist_us { 0 };

    // Auto-reset event signalled by the recv thread on every publish.
    // The render thread waits on this instead of polling -- zero idle
    // CPU when no packets, sub-microsecond wake when a packet lands.
    HANDLE                  new_scene     { nullptr };

    // Drives the frame-skip path -- render only when scene_id advances
    // (or the panic flag flips). Saves the GPU when the radar packet
    // rate is lower than the monitor refresh.
    uint32_t                last_drawn_scene_id { 0xFFFFFFFFu };
};

inline uint64_t now_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(
        steady_clock::now().time_since_epoch()).count();
}

Slot* writable_back(SceneBuf& b) {
    return &b.slots[b.back.load(std::memory_order_acquire)];
}

void publish(SceneBuf& b, uint64_t t) {
    uint8_t prev_back  = b.back.load(std::memory_order_acquire);
    uint8_t prev_ready = b.ready.exchange(prev_back, std::memory_order_acq_rel);
    b.back.store(prev_ready, std::memory_order_release);
    b.last_packet_us.store(t, std::memory_order_release);
    b.link_us.store(t, std::memory_order_release);
    b.state.store(LinkState::Awake, std::memory_order_release);
    if (b.new_scene) SetEvent(b.new_scene);
}

const Slot* acquire(SceneBuf& b) {
    uint8_t prev_front = b.front.load(std::memory_order_acquire);
    uint8_t prev_ready = b.ready.exchange(prev_front, std::memory_order_acq_rel);
    b.front.store(prev_ready, std::memory_order_release);
    return &b.slots[prev_ready];
}

bool is_live(const SceneBuf& b, uint64_t t) {
    if (b.state.load(std::memory_order_acquire) != LinkState::Awake) return false;
    uint64_t last_scene = b.last_packet_us.load(std::memory_order_acquire);
    uint64_t last_link  = b.link_us.load(std::memory_order_acquire);
    uint64_t last = last_scene > last_link ? last_scene : last_link;
    if (last == 0) return false;
    if (t <= last) return true;
    return (t - last) <= K_DORMANT_US;
}

// ----- receiver thread ------------------------------------------------------

// Tiny diagnostic logger. Writes one line per call to
// %LOCALAPPDATA%\dnp\daemon.log. Steady-state lines are throttled in
// the threads; this wrapper also caps the file size by truncating to
// keep only the most recent 64 KB once it exceeds 256 KB.
inline void dlog(const char* fmt, ...) {
    wchar_t home[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", home, MAX_PATH);
    if (!n || n >= MAX_PATH) return;
    wchar_t path[MAX_PATH];
    _snwprintf_s(path, _TRUNCATE, L"%s\\dnp\\daemon.log", home);

    // Rotation: when size > 256 KB, truncate to the trailing 64 KB.
    // Cheap (a few syscalls every Nth log line) compared to letting
    // the file balloon to MBs over a long session.
    {
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(path, GetFileExInfoStandard, &fa)) {
            LARGE_INTEGER sz{};
            sz.HighPart = fa.nFileSizeHigh;
            sz.LowPart  = fa.nFileSizeLow;
            if (sz.QuadPart > 256 * 1024) {
                FILE* rf = nullptr;
                if (_wfopen_s(&rf, path, L"rb") == 0 && rf) {
                    fseek(rf, -64 * 1024, SEEK_END);
                    static thread_local char buf[64 * 1024];
                    size_t got = fread(buf, 1, sizeof(buf), rf);
                    fclose(rf);
                    FILE* wf = nullptr;
                    if (_wfopen_s(&wf, path, L"wb") == 0 && wf) {
                        fwrite(buf, 1, got, wf);
                        fclose(wf);
                    }
                }
            }
        }
    }

    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"a") != 0 || !f) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(f, "%02u:%02u:%02u.%03u ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap; va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

void recv_loop(SceneBuf* buf, std::atomic<bool>* stop) {
    // TIME_CRITICAL + boost off: PeerAck reply gets the lowest possible
    // wakeup latency the scheduler can give us.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadPriorityBoost(GetCurrentThread(), FALSE);
    SetThreadAffinityMask(GetCurrentThread(), 0x2);   // core 1
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) { dlog("recv: socket() failed"); return; }
    dlog("recv: thread up");

    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    int rcvbuf = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));
    // SO_SNDBUF=0 -- daemon's PeerAck sendto skips kernel-side buffering
    // and writes straight to the NIC. Saves 10-30 us per reply.
    int sndbuf = 0;
    setsockopt(s, SOL_SOCKET, SO_SNDBUF,
               reinterpret_cast<const char*>(&sndbuf), sizeof(sndbuf));

    // IP_DONTFRAGMENT -- never want fragmentation reassembly setup on
    // our 60 B Hello / 20 B PeerAck / 1456 B Scene packets.
    DWORD dontfrag = 1;
    setsockopt(s, IPPROTO_IP, IP_DONTFRAGMENT,
               reinterpret_cast<const char*>(&dontfrag), sizeof(dontfrag));

    // Aggressive timeout -- recvfrom returns every 100 ms when idle so
    // the shutdown flag is checked promptly. When packets are flowing,
    // recvfrom returns immediately on each one, so the timeout doesn't
    // affect hot-path latency.
    DWORD timeout = 100;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));

    sockaddr_in bind_addr{};
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port        = htons(K_PORT);
    if (bind(s, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr))
        == SOCKET_ERROR) { dlog("recv: bind() failed err=%d", WSAGetLastError());
                           closesocket(s); return; }
    dlog("recv: bound 0.0.0.0:%u", (unsigned)K_PORT);

    uint64_t n_hello = 0, n_scene = 0, n_bye = 0, n_other = 0;
    uint64_t last_log_us = 0;

    static constexpr size_t BUF_BYTES = proto::MAX_DATAGRAM_BYTES + 64;
    auto pkt = std::make_unique<uint8_t[]>(BUF_BYTES);

    while (!stop->load(std::memory_order_acquire)) {
        sockaddr_in from{};
        int from_len = sizeof(from);
        int n = recvfrom(s,
                         reinterpret_cast<char*>(pkt.get()),
                         static_cast<int>(BUF_BYTES),
                         0,
                         reinterpret_cast<sockaddr*>(&from), &from_len);
        if (n == SOCKET_ERROR) continue;
        if (static_cast<size_t>(n) < sizeof(proto::Header)) continue;

        const auto* h = reinterpret_cast<const proto::Header*>(pkt.get());
        if (std::memcmp(h->magic, proto::MAGIC, sizeof(h->magic)) != 0) continue;
        if (h->version != proto::VERSION) continue;

        switch (static_cast<proto::Type>(h->type)) {
            case proto::Type::Hello: {
                if (static_cast<size_t>(n) < sizeof(proto::Hello)) break;
                ++n_hello;
                // NOTE: do NOT stamp last_packet_us here. That field
                // tracks Scene freshness only and drives the staleness
                // gate in render_loop. Hello is a 3 s link keepalive; if
                // it refreshed the gate, a Hello arriving during a Scene
                // drop would mask the stall and render stale ESP. link_us
                // is the separate liveness clock used only by is_live() so
                // a fresh Hello drives the loader-restart recovery without
                // touching the stale-Scene path.
                buf->link_us.store(now_us(), std::memory_order_release);
                buf->state.store(LinkState::Awake, std::memory_order_release);
                // Discovery reply -- same shape as a Bye, type=PeerAck.
                // Build on the stack (no malloc); sendto is the OS's
                // fastest path for unconnected UDP.
                proto::PeerAck ack;
                std::memcpy(ack.header.magic, proto::MAGIC, sizeof(ack.header.magic));
                ack.header.version = proto::VERSION;
                ack.header.type    = static_cast<uint16_t>(proto::Type::PeerAck);
                ack.header.seq     = h->seq;
                ack.header.tick_us = now_us();
                sendto(s, reinterpret_cast<const char*>(&ack),
                       static_cast<int>(sizeof(ack)), 0,
                       reinterpret_cast<sockaddr*>(&from), from_len);
                // Wake the render thread too so it can re-evaluate live
                // state (e.g. lazy D3D init runs on first Hello).
                if (buf->new_scene) SetEvent(buf->new_scene);
                break;
            }
            case proto::Type::Bye:
                ++n_bye;
                buf->state.store(LinkState::Dormant, std::memory_order_release);
                break;
            case proto::Type::Scene: {
                if (static_cast<size_t>(n) < sizeof(proto::Scene)) break;
                ++n_scene;
                g_stats.raw_count.fetch_add(1, std::memory_order_relaxed);
                const auto* in = reinterpret_cast<const proto::Scene*>(pkt.get());
                uint16_t cnt = in->entity_count;
                if (cnt > K_MAX_ENT) cnt = K_MAX_ENT;
                Slot* slot       = writable_back(*buf);
                auto& dst        = slot->scene;
                dst.header       = in->header;
                std::memcpy(dst.viewproj, in->viewproj, sizeof(dst.viewproj));
                dst.screen_w     = in->screen_w;
                dst.screen_h     = in->screen_h;
                dst.scene_id     = in->scene_id;
                dst.entity_count = cnt;
                dst.reserved     = 0;
                if (cnt) std::memcpy(dst.entities, in->entities,
                                     cnt * sizeof(proto::WireEntity));
                publish(*buf, now_us());
                break;
            }
            case proto::Type::DrawList: {
                if (static_cast<size_t>(n) < sizeof(proto::DrawList)) break;
                ++n_other;
                const auto* in = reinterpret_cast<const proto::DrawList*>(pkt.get());
                uint16_t used = in->blob_used;
                if (used > proto::DRAW_BLOB_BYTES) used = proto::DRAW_BLOB_BYTES;
                Slot* slot     = writable_back(*buf);
                slot->dlist.header    = in->header;
                slot->dlist.blob_used = used;
                slot->dlist.reserved  = 0;
                if (used) std::memcpy(slot->dlist.blob, in->blob, used);
                const uint64_t t = now_us();
                buf->last_dlist_us.store(t, std::memory_order_release);
                publish(*buf, t);
                break;
            }
            case proto::Type::PeerAck:
                ++n_other;
                break;
        }

        // throttled stats line, ~every 30 s
        uint64_t t = now_us();
        if (t - last_log_us > 30'000'000) {
            last_log_us = t;
            dlog("recv: hello=%llu scene=%llu bye=%llu other=%llu",
                 (unsigned long long)n_hello,
                 (unsigned long long)n_scene,
                 (unsigned long long)n_bye,
                 (unsigned long long)n_other);
        }
    }

    closesocket(s);
    dlog("recv: thread exiting");
}

// ----- D3D11 + Direct2D facade ----------------------------------------------

struct Gfx {
    ID3D11Device*           dev   = nullptr;
    ID3D11DeviceContext*    ctx   = nullptr;
    IDXGISwapChain*         swap  = nullptr;
    IDXGISwapChain2*        swap2 = nullptr;        // for waitable
    HANDLE                  waitable = nullptr;     // GetFrameLatencyWaitableObject

    // DirectComposition path (preferred). When these are non-null we
    // use FLIP_DISCARD + tearing + waitable; when they're null we fall
    // back to legacy bitblt DISCARD on the HWND directly.
    IDCompositionDevice*    dcomp     = nullptr;
    IDCompositionTarget*    dtarget   = nullptr;
    IDCompositionVisual*    dvisual   = nullptr;
    bool                    use_dcomp = false;
    bool                    tearing   = false;       // ALLOW_TEARING supported
    bool                    mpo       = false;       // MPO available (info-only)

    ID2D1Factory1*          d2f   = nullptr;
    ID2D1Device*            d2d   = nullptr;
    ID2D1DeviceContext*     d2c   = nullptr;
    ID2D1Bitmap1*           tgt   = nullptr;

    ID2D1SolidColorBrush*   b_enemy  = nullptr;
    ID2D1SolidColorBrush*   b_friend = nullptr;
    ID2D1SolidColorBrush*   b_ai     = nullptr;
    ID2D1SolidColorBrush*   b_downed = nullptr;
    ID2D1SolidColorBrush*   b_text   = nullptr;

    IDWriteFactory*         dw      = nullptr;
    IDWriteTextFormat*      tf      = nullptr;

    UINT w = 0;
    UINT h = 0;
    bool ready = false;
};

template<class T>
inline void rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

void gfx_teardown(Gfx& g) {
    rel(g.tf);  rel(g.dw);
    rel(g.b_enemy); rel(g.b_friend); rel(g.b_ai); rel(g.b_downed); rel(g.b_text);
    rel(g.tgt); rel(g.d2c); rel(g.d2d); rel(g.d2f);
    rel(g.dvisual); rel(g.dtarget); rel(g.dcomp);
    if (g.waitable) { g.waitable = nullptr; } // owned by swap2, no CloseHandle
    rel(g.swap2);
    rel(g.swap); rel(g.ctx); rel(g.dev);
    g.use_dcomp = false;
    g.tearing   = false;
    g.mpo       = false;
    g.w = g.h = 0;
    g.ready = false;
}

bool gfx_init(Gfx& g, HWND hwnd) {
    RECT rc{};
    GetClientRect(hwnd, &rc);
    g.w = (UINT)(rc.right - rc.left);
    g.h = (UINT)(rc.bottom - rc.top);
    if (!g.w || !g.h) return false;

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL levels[] = {
        D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
    };
    D3D_FEATURE_LEVEL got;

    // ---- Path A: DirectComposition + FLIP_DISCARD + Waitable + ALLOW_TEARING
    //
    // Lowest-latency path. Creates an HWND-less swap chain ("for
    // composition"), wraps it in a DComp visual, and binds the visual
    // as the root of a DComp target on the Discord overlay HWND. DWM
    // composes our content over whatever Discord's overlay HWND
    // already has, and SetMaximumFrameLatency(1) + Waitable lets us
    // pace presents to the GPU's actual ready edge instead of the
    // default 3-frame queue. ALLOW_TEARING is set so the GPU can flip
    // the moment we Present, regardless of vsync.
    //
    // Fallback (Path B below) if any of this fails -- DComp is
    // available since Win8 but the CreateTargetForHwnd call can
    // refuse if some other code (e.g. Discord itself) already owns
    // the HWND's composition root.

    if (FAILED(D3D11CreateDevice(
            nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
            levels, (UINT)(sizeof(levels) / sizeof(levels[0])),
            D3D11_SDK_VERSION, &g.dev, &got, &g.ctx))) {
        return false;
    }

    bool dcomp_ok = false;

    IDXGIDevice* dxgi_device = nullptr;
    if (SUCCEEDED(g.dev->QueryInterface(
            __uuidof(IDXGIDevice),
            reinterpret_cast<void**>(&dxgi_device)))) {
        IDXGIAdapter* adapter = nullptr;
        if (SUCCEEDED(dxgi_device->GetAdapter(&adapter))) {
            IDXGIFactory2* factory2 = nullptr;
            if (SUCCEEDED(adapter->GetParent(
                    __uuidof(IDXGIFactory2),
                    reinterpret_cast<void**>(&factory2)))) {

                // MPO probe -- informational; lets us log whether the
                // GPU can put us on a hardware overlay plane.
                IDXGIOutput* out0 = nullptr;
                if (SUCCEEDED(adapter->EnumOutputs(0, &out0)) && out0) {
                    IDXGIOutput6* out6 = nullptr;
                    if (SUCCEEDED(out0->QueryInterface(
                            __uuidof(IDXGIOutput6),
                            reinterpret_cast<void**>(&out6))) && out6) {
                        UINT comp_flags = 0;
                        if (SUCCEEDED(out6->CheckHardwareCompositionSupport(&comp_flags))) {
                            g.mpo = (comp_flags &
                                DXGI_HARDWARE_COMPOSITION_SUPPORT_FLAG_WINDOWED) != 0;
                        }
                        out6->Release();
                    }
                    out0->Release();
                }

                // ALLOW_TEARING support probe.
                IDXGIFactory5* factory5 = nullptr;
                if (SUCCEEDED(factory2->QueryInterface(
                        __uuidof(IDXGIFactory5),
                        reinterpret_cast<void**>(&factory5))) && factory5) {
                    BOOL t = FALSE;
                    if (SUCCEEDED(factory5->CheckFeatureSupport(
                            DXGI_FEATURE_PRESENT_ALLOW_TEARING,
                            &t, sizeof(t)))) {
                        g.tearing = (t != FALSE);
                    }
                    factory5->Release();
                }

                DXGI_SWAP_CHAIN_DESC1 scd1{};
                scd1.Width       = g.w;
                scd1.Height      = g.h;
                scd1.Format      = DXGI_FORMAT_B8G8R8A8_UNORM;
                scd1.Stereo      = FALSE;
                scd1.SampleDesc  = { 1, 0 };
                scd1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                scd1.BufferCount = 2;
                scd1.Scaling     = DXGI_SCALING_STRETCH;
                scd1.SwapEffect  = DXGI_SWAP_EFFECT_FLIP_DISCARD;
                scd1.AlphaMode   = DXGI_ALPHA_MODE_PREMULTIPLIED;
                scd1.Flags       = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
                if (g.tearing) scd1.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;

                IDXGISwapChain1* swap1 = nullptr;
                if (SUCCEEDED(factory2->CreateSwapChainForComposition(
                        g.dev, &scd1, nullptr, &swap1))) {
                    // Promote to swap chain interface we keep + the
                    // SwapChain2 that owns the waitable handle.
                    g.swap = swap1;     // base interface (rendered targets)
                    swap1->QueryInterface(
                        __uuidof(IDXGISwapChain2),
                        reinterpret_cast<void**>(&g.swap2));

                    if (g.swap2) {
                        g.swap2->SetMaximumFrameLatency(1);
                        g.waitable = g.swap2->GetFrameLatencyWaitableObject();
                    }

                    // Build DComp tree binding the swap chain into the
                    // Discord overlay HWND. CreateTargetForHwnd with
                    // topmost=TRUE places our visual above any existing
                    // child content of the HWND.
                    if (SUCCEEDED(DCompositionCreateDevice(
                            dxgi_device, __uuidof(IDCompositionDevice),
                            reinterpret_cast<void**>(&g.dcomp)))) {
                        if (SUCCEEDED(g.dcomp->CreateTargetForHwnd(
                                hwnd, TRUE, &g.dtarget))) {
                            if (SUCCEEDED(g.dcomp->CreateVisual(&g.dvisual))) {
                                g.dvisual->SetContent(g.swap);
                                g.dtarget->SetRoot(g.dvisual);
                                if (SUCCEEDED(g.dcomp->Commit())) {
                                    dcomp_ok    = true;
                                    g.use_dcomp = true;
                                }
                            }
                        }
                    }
                }

                factory2->Release();
            }
            adapter->Release();
        }
        dxgi_device->Release();
    }

    // ---- Path B: fallback to legacy bitblt on the HWND directly.
    //
    // Same swap-effect as before -- DISCARD bitblt composites through
    // DWM and reliably honours alpha on Discord's overlay HWND. The
    // extra back buffer (BufferCount=2) lets DWM keep one composited
    // frame queued, smoothing out tiny stutters from variable Present
    // arrival times.
    if (!dcomp_ok) {
        // Tear down any partial DComp state.
        rel(g.dvisual); rel(g.dtarget); rel(g.dcomp);
        if (g.waitable) g.waitable = nullptr;
        rel(g.swap2);
        rel(g.swap);
        g.use_dcomp = false;

        DXGI_SWAP_CHAIN_DESC sd{};
        sd.BufferCount       = 2;          // bumped from 1 -- smoother under DWM
        sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.BufferDesc.Width  = g.w;
        sd.BufferDesc.Height = g.h;
        sd.BufferUsage       = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow      = hwnd;
        sd.SampleDesc.Count  = 1;
        sd.Windowed          = TRUE;
        sd.SwapEffect        = DXGI_SWAP_EFFECT_DISCARD;

        // We already have a device -- create just the swap chain via
        // the IDXGIFactory derived from the device.
        IDXGIDevice* dxd2 = nullptr;
        if (FAILED(g.dev->QueryInterface(
                __uuidof(IDXGIDevice),
                reinterpret_cast<void**>(&dxd2)))) {
            return false;
        }
        IDXGIAdapter* ad2 = nullptr;
        if (FAILED(dxd2->GetAdapter(&ad2))) { dxd2->Release(); return false; }
        IDXGIFactory* fac2 = nullptr;
        if (FAILED(ad2->GetParent(__uuidof(IDXGIFactory),
                                  reinterpret_cast<void**>(&fac2)))) {
            ad2->Release(); dxd2->Release(); return false;
        }
        HRESULT hr_sc = fac2->CreateSwapChain(g.dev, &sd, &g.swap);
        fac2->Release(); ad2->Release(); dxd2->Release();
        if (FAILED(hr_sc)) return false;

        // Cap GPU frame-queue depth at 1. Default is 3 -- means up to
        // 3 frames pending on the GPU at once. When CS2 saturates the
        // GPU, our Present queues behind their work; with default
        // queue depth, that's up to 3 of our frames stalled in flight,
        // visible to the user as "ESP frozen for a sec". Cutting to 1
        // means we either render now or stall briefly -- but the
        // *backlog* never grows, so a transient CS2 GPU spike is
        // recovered from in one frame instead of three.
        IDXGIDevice1* dxgi_dev1 = nullptr;
        if (SUCCEEDED(g.dev->QueryInterface(
                __uuidof(IDXGIDevice1),
                reinterpret_cast<void**>(&dxgi_dev1)))) {
            dxgi_dev1->SetMaximumFrameLatency(1);
            dxgi_dev1->Release();
        }
    }
    dlog("gfx: %s tearing=%d mpo=%d",
         g.use_dcomp ? "DComp+FLIP_DISCARD+Waitable" : "bitblt DISCARD",
         g.tearing ? 1 : 0, g.mpo ? 1 : 0);

    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 __uuidof(ID2D1Factory1),
                                 reinterpret_cast<void**>(&g.d2f)))) return false;

    IDXGIDevice* dxd = nullptr;
    if (FAILED(g.dev->QueryInterface(__uuidof(IDXGIDevice),
                                     reinterpret_cast<void**>(&dxd)))) return false;
    HRESULT hr = g.d2f->CreateDevice(dxd, &g.d2d);
    rel(dxd);
    if (FAILED(hr)) return false;

    if (FAILED(g.d2d->CreateDeviceContext(
            D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &g.d2c))) return false;

    IDXGISurface* surf = nullptr;
    if (FAILED(g.swap->GetBuffer(0, __uuidof(IDXGISurface),
                                 reinterpret_cast<void**>(&surf)))) return false;
    D2D1_BITMAP_PROPERTIES1 bp{};
    bp.pixelFormat   = D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM,
                                         D2D1_ALPHA_MODE_PREMULTIPLIED);
    bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET |
                       D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    hr = g.d2c->CreateBitmapFromDxgiSurface(surf, &bp, &g.tgt);
    rel(surf);
    if (FAILED(hr)) return false;

    g.d2c->SetTarget(g.tgt);
    g.d2c->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);

    auto mk = [&](const D2D1_COLOR_F& c, ID2D1SolidColorBrush** out) {
        return SUCCEEDED(g.d2c->CreateSolidColorBrush(c, out));
    };
    if (!mk(D2D1::ColorF(0xff4040, 1.0f), &g.b_enemy))  return false;
    if (!mk(D2D1::ColorF(0x40ff60, 1.0f), &g.b_friend)) return false;
    if (!mk(D2D1::ColorF(0xffd040, 1.0f), &g.b_ai))     return false;
    if (!mk(D2D1::ColorF(0xc060ff, 1.0f), &g.b_downed)) return false;
    if (!mk(D2D1::ColorF(0xffffff, 1.0f), &g.b_text))   return false;

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                   __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(&g.dw))))
        return false;
    if (FAILED(g.dw->CreateTextFormat(L"Segoe UI", nullptr,
                                      DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                      DWRITE_FONT_STYLE_NORMAL,
                                      DWRITE_FONT_STRETCH_NORMAL,
                                      12.0f, L"en-us", &g.tf))) return false;
    g.tf->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    g.tf->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    g.ready = true;
    return true;
}

bool gfx_resize(Gfx& g, HWND hwnd) {
    if (!g.ready) return true;
    RECT rc{}; GetClientRect(hwnd, &rc);
    UINT w = (UINT)(rc.right - rc.left), h = (UINT)(rc.bottom - rc.top);
    if (!w || !h || (w == g.w && h == g.h)) return true;

    g.d2c->SetTarget(nullptr);
    rel(g.tgt);
    if (FAILED(g.swap->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, 0))) {
        gfx_teardown(g); return false;
    }
    IDXGISurface* surf = nullptr;
    if (FAILED(g.swap->GetBuffer(0, __uuidof(IDXGISurface),
                                 reinterpret_cast<void**>(&surf)))) {
        gfx_teardown(g); return false;
    }
    D2D1_BITMAP_PROPERTIES1 bp{};
    bp.pixelFormat   = D2D1::PixelFormat(DXGI_FORMAT_R8G8B8A8_UNORM,
                                         D2D1_ALPHA_MODE_PREMULTIPLIED);
    bp.bitmapOptions = D2D1_BITMAP_OPTIONS_TARGET |
                       D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
    HRESULT hr = g.d2c->CreateBitmapFromDxgiSurface(surf, &bp, &g.tgt);
    rel(surf);
    if (FAILED(hr)) { gfx_teardown(g); return false; }

    g.d2c->SetTarget(g.tgt);
    g.w = w; g.h = h;
    return true;
}

ID2D1SolidColorBrush* pick_brush(uint16_t flags, const Gfx& g) {
    if (flags & proto::F_DOWNED) return g.b_downed;
    if (flags & proto::F_AI)     return g.b_ai;
    if (flags & proto::F_ENEMY)  return g.b_enemy;
    return g.b_friend;
}

struct ProjPt { float x, y; bool vis; };
ProjPt project(const float vp[16], float wx, float wy, float wz,
               float sw, float sh) {
    float xc = vp[0]*wx + vp[1]*wy + vp[2]*wz + vp[3];
    float yc = vp[4]*wx + vp[5]*wy + vp[6]*wz + vp[7];
    float wc = vp[12]*wx + vp[13]*wy + vp[14]*wz + vp[15];
    ProjPt p{0,0,false};
    if (wc <= 0.0001f) return p;
    float nx = xc / wc, ny = yc / wc;
    p.x   = (nx * 0.5f + 0.5f) * sw;
    p.y   = (1.0f - (ny * 0.5f + 0.5f)) * sh;
    p.vis = (p.x >= 0 && p.x < sw && p.y >= 0 && p.y < sh);
    return p;
}

// Solid colour brush, recoloured per call rather than recreated.
// CreateSolidColorBrush is the per-draw slow path; SetColor is a few
// stores and a flush of the brush's CPU-side cache. Kept singleton on
// the Gfx; allocated lazily on first use.
ID2D1SolidColorBrush* get_scratch_brush(Gfx& g, uint32_t argb) {
    if (!g.b_text) return nullptr;  // gfx still booting
    static ID2D1SolidColorBrush* scratch = nullptr;
    if (!scratch) {
        if (FAILED(g.d2c->CreateSolidColorBrush(
                D2D1::ColorF(0xffffff, 1.0f), &scratch))) return nullptr;
    }
    float a = ((argb >> 24) & 0xff) / 255.0f;
    float r = ((argb >> 16) & 0xff) / 255.0f;
    float gn= ((argb >>  8) & 0xff) / 255.0f;
    float b = ( argb        & 0xff) / 255.0f;
    scratch->SetColor(D2D1::ColorF(r, gn, b, a));
    return scratch;
}

// Walk a DrawList blob and emit D2D primitives. The blob is a sequence
// of DrawCmdHeader-prefixed records; each record's `size` tells us how
// far to advance. Unknown commands are skipped silently so a newer
// loader doesn't break an older daemon.
//
// Coordinate scaling: primitives come in the GAME's screen-space pixels
// (the same space send_scene_matrix advertises as screen_w/h). Discord's
// overlay HWND is monitor-sized, so when the game runs at a different
// resolution we have to scale game-space pixels into canvas-space pixels.
// `sx, sy` carry that ratio; identity (1,1) is correct when both spaces
// match. Caller computes this from the latest Scene packet's screen_w/h.
void render_draw_list(Gfx& g, const proto::DrawList& dl, float sx, float sy,
                      const float vp[16], float sw, float sh, float extrap_us)
{
    const uint8_t* p   = dl.blob;
    const uint8_t* end = dl.blob + dl.blob_used;

    // Active per-pawn velocity context for World-space primitives. Reset to
    // zero at the start of each draw list and on every SkeletonGroup record.
    // Bones following a group record extrapolate forward by skel_vel * dt.
    float skel_vx = 0.0f, skel_vy = 0.0f, skel_vz = 0.0f;
    const bool have_vp = (vp != nullptr && sw > 0.0f && sh > 0.0f);

    while (p + sizeof(proto::DrawCmdHeader) <= end) {
        const auto* h = reinterpret_cast<const proto::DrawCmdHeader*>(p);
        if (h->size < sizeof(proto::DrawCmdHeader)) break;          // malformed
        if (p + h->size > end)                       break;          // overrun

        // SkeletonGroup carries no colour; everything else routes through a
        // scratch brush sized to h->color. Handle the group up-front so the
        // brush fetch is skipped for it.
        if (static_cast<proto::Cmd>(h->type) == proto::Cmd::SkeletonGroup) {
            if (h->size >= sizeof(proto::CmdSkeletonGroup)) {
                const auto* c = reinterpret_cast<const proto::CmdSkeletonGroup*>(p);
                skel_vx = c->vel_x;
                skel_vy = c->vel_y;
                skel_vz = c->vel_z;
            }
            p += h->size;
            continue;
        }

        ID2D1SolidColorBrush* br = get_scratch_brush(g, h->color);
        if (!br) { p += h->size; continue; }

        switch (static_cast<proto::Cmd>(h->type)) {
            case proto::Cmd::Line: {
                if (h->size < sizeof(proto::CmdLine)) break;
                const auto* c = reinterpret_cast<const proto::CmdLine*>(p);
                g.d2c->DrawLine(D2D1::Point2F(c->x1 * sx, c->y1 * sy),
                                D2D1::Point2F(c->x2 * sx, c->y2 * sy),
                                br, c->thickness);
                break;
            }
            case proto::Cmd::Rect: {
                if (h->size < sizeof(proto::CmdRect)) break;
                const auto* c = reinterpret_cast<const proto::CmdRect*>(p);
                D2D1_RECT_F r{ c->x * sx, c->y * sy,
                               (c->x + c->w)  * sx,
                               (c->y + c->_h) * sy };
                if (c->flags & proto::F_FILLED) g.d2c->FillRectangle(r, br);
                else                            g.d2c->DrawRectangle(r, br, c->thickness);
                break;
            }
            case proto::Cmd::Ellipse: {
                if (h->size < sizeof(proto::CmdEllipse)) break;
                const auto* c = reinterpret_cast<const proto::CmdEllipse*>(p);
                D2D1_ELLIPSE e{ D2D1::Point2F(c->cx * sx, c->cy * sy),
                                c->rx * sx, c->ry * sy };
                if (c->flags & proto::F_FILLED) g.d2c->FillEllipse(e, br);
                else                            g.d2c->DrawEllipse(e, br, c->thickness);
                break;
            }
            case proto::Cmd::Text: {
                if (h->size < sizeof(proto::CmdText)) break;
                const auto* c = reinterpret_cast<const proto::CmdText*>(p);
                // Defensive cap: text_len is uint16, total must fit in `size`.
                uint16_t tl = c->text_len;
                if (tl > h->size - (offsetof(proto::CmdText, text))) {
                    tl = (uint16_t)(h->size - offsetof(proto::CmdText, text));
                }
                wchar_t wbuf[128];
                int wlen = MultiByteToWideChar(CP_UTF8, 0,
                            c->text, tl, wbuf, 127);
                if (wlen <= 0) break;
                wbuf[wlen] = 0;
                float cx = c->x * sx;
                float cy = c->y * sy;
                D2D1_RECT_F r{ cx - 60.0f, cy - 8.0f,
                               cx + 60.0f, cy + 16.0f };
                g.d2c->DrawText(wbuf, (UINT32)wlen, g.tf, r, br,
                                D2D1_DRAW_TEXT_OPTIONS_NONE);
                break;
            }
            case proto::Cmd::Line3D: {
                if (!have_vp) break;
                if (h->size < sizeof(proto::CmdLine3D)) break;
                const auto* c = reinterpret_cast<const proto::CmdLine3D*>(p);
                float ax = c->ax + skel_vx * extrap_us;
                float ay = c->ay + skel_vy * extrap_us;
                float az = c->az + skel_vz * extrap_us;
                float bx = c->bx + skel_vx * extrap_us;
                float by = c->by + skel_vy * extrap_us;
                float bz = c->bz + skel_vz * extrap_us;
                ProjPt pa = project(vp, ax, ay, az, sw, sh);
                ProjPt pb = project(vp, bx, by, bz, sw, sh);
                if (!pa.vis && !pb.vis) break;
                g.d2c->DrawLine(D2D1::Point2F(pa.x * sx, pa.y * sy),
                                D2D1::Point2F(pb.x * sx, pb.y * sy),
                                br, c->thickness);
                break;
            }
            case proto::Cmd::Ellipse3D: {
                if (!have_vp) break;
                if (h->size < sizeof(proto::CmdEllipse3D)) break;
                const auto* c = reinterpret_cast<const proto::CmdEllipse3D*>(p);
                float cx = c->cx + skel_vx * extrap_us;
                float cy = c->cy + skel_vy * extrap_us;
                float cz = c->cz + skel_vz * extrap_us;
                float ax = c->ax + skel_vx * extrap_us;
                float ay = c->ay + skel_vy * extrap_us;
                float az = c->az + skel_vz * extrap_us;
                ProjPt pc = project(vp, cx, cy, cz, sw, sh);
                ProjPt pa = project(vp, ax, ay, az, sw, sh);
                if (!pc.vis) break;
                float dx = (pc.x - pa.x) * sx;
                float dy = (pc.y - pa.y) * sy;
                float r  = std::sqrtf(dx*dx + dy*dy) * c->radius_scale;
                if (r < 2.0f) r = 2.0f;
                D2D1_ELLIPSE e{ D2D1::Point2F(pc.x * sx, pc.y * sy), r, r };
                g.d2c->DrawEllipse(e, br, c->thickness);
                break;
            }
            case proto::Cmd::SkeletonGroup:
                // Handled before the brush fetch above. Should not reach here.
                break;
            case proto::Cmd::Polyline: {
                if (h->size < sizeof(proto::CmdPolyline)) break;
                const auto* c = reinterpret_cast<const proto::CmdPolyline*>(p);
                uint16_t n = c->n_pts;
                // pts laid out as float pairs after the struct prefix.
                size_t pts_off = offsetof(proto::CmdPolyline, pts);
                if (h->size < pts_off + (size_t)n * 8) break;
                const float* pts = c->pts;
                for (uint16_t i = 1; i < n; ++i) {
                    g.d2c->DrawLine(
                        D2D1::Point2F(pts[(i-1)*2] * sx, pts[(i-1)*2+1] * sy),
                        D2D1::Point2F(pts[ i   *2] * sx, pts[ i   *2+1] * sy),
                        br, c->thickness);
                }
                if ((c->flags & proto::F_FILLED) && n > 2) {
                    // close
                    g.d2c->DrawLine(
                        D2D1::Point2F(pts[(n-1)*2] * sx, pts[(n-1)*2+1] * sy),
                        D2D1::Point2F(pts[0]       * sx, pts[1]         * sy),
                        br, c->thickness);
                }
                break;
            }
        }

        p += h->size;
    }
}

// LiveStats moved above recv_loop -- see top of anonymous namespace.

void render_frame(Gfx& g, const Slot* slot, SceneBuf* buf_ptr) {
    if (!g.ready) return;

    g.d2c->BeginDraw();
    g.d2c->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

    // Compute game→canvas scale once per frame. Both Scene-path boxes and
    // DrawList-path primitives are authored in game-screen pixels by the
    // loader; we map them onto Discord's monitor-sized overlay HWND here.
    // Falls back to identity if no Scene has arrived yet (no screen_w/h).
    const proto::Scene* scene = slot ? &slot->scene : nullptr;
    float dl_sx = 1.0f, dl_sy = 1.0f;
    if (scene && scene->screen_w > 0 && scene->screen_h > 0 && g.w > 0 && g.h > 0) {
        dl_sx = (float)g.w / (float)scene->screen_w;
        dl_sy = (float)g.h / (float)scene->screen_h;
    }

    // ---- DrawList path (game-agnostic primitives from loader) -------------
    if (slot && slot->dlist.blob_used > 0) {
        // Forward extrapolation interval. Time between this render frame
        // and the moment the last DrawList landed in our buffer is the
        // gap the sender's pre-baked positions would otherwise freeze
        // across. Per-pawn skeleton velocity (CmdSkeletonGroup) gets
        // multiplied by this delta so World-space primitives chase the
        // model between sender ticks. Capped to one tick so a paused
        // sender doesn't run away with the prediction.
        float extrap_us = 0.0f;
        const uint64_t last_dl = buf_ptr ? buf_ptr->last_dlist_us.load(std::memory_order_acquire) : 0;
        if (last_dl != 0) {
            uint64_t dt = now_us() - last_dl;
            if (dt > 33'000) dt = 33'000;
            extrap_us = (float)dt;
        }
        const float* vp = scene ? scene->viewproj : nullptr;
        float sw = (scene && scene->screen_w > 0) ? (float)scene->screen_w : (float)g.w;
        float sh = (scene && scene->screen_h > 0) ? (float)scene->screen_h : (float)g.h;
        render_draw_list(g, slot->dlist, dl_sx, dl_sy, vp, sw, sh, extrap_us);
    }

    // ---- Legacy Scene path (built-in ESP boxes) ---------------------------
    if (scene && scene->entity_count > 0) {
        float sw = (scene->screen_w > 0) ? (float)scene->screen_w : (float)g.w;
        float sh = (scene->screen_h > 0) ? (float)scene->screen_h : (float)g.h;
        float sx = (float)g.w / sw;
        float sy = (float)g.h / sh;

        for (uint16_t i = 0; i < scene->entity_count; ++i) {
            const proto::WireEntity& e = scene->entities[i];
            if (!(e.flags & proto::F_ALIVE)) continue;
            ProjPt feet = project(scene->viewproj,
                                  e.feet_x, e.feet_y, e.feet_z, sw, sh);
            ProjPt head = project(scene->viewproj,
                                  e.head_x, e.head_y, e.head_z, sw, sh);
            if (!feet.vis && !head.vis) continue;

            feet.x *= sx; feet.y *= sy;
            head.x *= sx; head.y *= sy;

            float bh = std::fabs(feet.y - head.y);
            if (bh < 6.0f) bh = 6.0f;
            float bw = bh * 0.5f;
            float bx = head.x - bw * 0.5f;
            float by = head.y;

            ID2D1SolidColorBrush* brush = pick_brush(e.flags, g);
            D2D1_RECT_F box{ bx, by, bx + bw, by + bh };
            g.d2c->DrawRectangle(box, brush, 1.5f);

            if (e.health > 0.0f) {
                float frac = e.health / 100.0f;
                if (frac > 1.0f) frac = 1.0f;
                D2D1_RECT_F hp_bg{ bx - 6.0f, by, bx - 2.0f, by + bh };
                D2D1_RECT_F hp_fg{ bx - 6.0f, by + bh * (1.0f - frac),
                                   bx - 2.0f, by + bh };
                g.d2c->FillRectangle(hp_bg, g.b_text);
                g.d2c->FillRectangle(hp_fg, brush);
            }

            if (e.name[0] != 0) {
                wchar_t buf[64];
                _snwprintf_s(buf, _TRUNCATE, L"%hs [%dm]",
                             e.name, (int)e.distance);
                D2D1_RECT_F tb{ bx - 40.0f, by + bh + 1.0f,
                                bx + bw + 40.0f, by + bh + 18.0f };
                g.d2c->DrawText(buf, (UINT32)std::wcslen(buf),
                                g.tf, tb, g.b_text,
                                D2D1_DRAW_TEXT_OPTIONS_NONE);
            }
        }
    }

    HRESULT hr = g.d2c->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        gfx_teardown(g);
        return;
    }

    UINT flags = (g.use_dcomp && g.tearing) ? DXGI_PRESENT_ALLOW_TEARING : 0;
    flags |= DXGI_PRESENT_DO_NOT_WAIT;
    HRESULT phr = g.swap->Present(0, flags);
    if (phr == DXGI_ERROR_WAS_STILL_DRAWING) return;
    if (phr == DXGI_ERROR_DEVICE_REMOVED || phr == DXGI_ERROR_DEVICE_RESET) {
        gfx_teardown(g);
        return;
    }
    if (g.use_dcomp && g.dcomp) g.dcomp->Commit();
}

// ----- render thread --------------------------------------------------------

void render_loop(SceneBuf* buf, std::atomic<bool>* stop) {
    // TIME_CRITICAL puts this thread above almost everything else in the
    // process. The render loop is short and bounded -- it never spins
    // long enough to starve other threads -- so the priority is safe.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadPriorityBoost(GetCurrentThread(), FALSE);

    // Affinity to core 2 -- keeps the render thread off the OS-scheduler
    // pool that the recv thread (core 1) and Discord's own renderer
    // (default scheduling) compete for. If the machine has <3 logical
    // cores SetThreadAffinityMask falls back gracefully.
    SetThreadAffinityMask(GetCurrentThread(), 0x4);
    dlog("render: thread up");

    Gfx g{};
    HWND target = nullptr;
    bool wda_applied = false;
    uint64_t last_log_us = 0;
    uint64_t frames_drawn = 0;
    uint16_t last_entity_count = 0;

    // Stutter telemetry. Tracks scene_id advance vs render iterations
    // and reports any gap >100ms between scene_ids -- that's the
    // signature the user is seeing.
    uint32_t last_seen_scene_id = 0xFFFFFFFFu;
    uint64_t last_scene_advance_us = 0;
    uint32_t stall_count = 0;
    uint64_t last_stall_log_us = 0;

    // Live-rate accumulators for the on-screen HUD. Reset every 1 s.
    uint32_t rate_frames    = 0;
    uint32_t rate_scenes    = 0;
    uint32_t rate_stalls    = 0;
    uint64_t rate_window_us = now_us();
    uint32_t last_raw_count = 0;

    // Per-iteration timing -- catches frame intervals >25ms which we
    // missed with the per-Present-only log. If the render loop itself
    // is being preempted, this shows it.
    uint64_t last_iter_us = 0;

    // Tracks how long the overlay HWND has been missing. Brief misses
    // (Discord recreates the HWND on voice-popup toggle, alt-tab,
    // fullscreen state changes, etc.) shouldn't cost a full gfx_teardown
    // + reinit -- that's a ~200-500 ms freeze. Wait HWND_GRACE_MS before
    // tearing anything down; usually the HWND comes back well before
    // then with the same handle, and we resume without a hitch.
    uint64_t hwnd_lost_us = 0;
    constexpr uint64_t HWND_GRACE_US = 800'000;   // 800 ms

    while (!stop->load(std::memory_order_acquire)) {
        // Event-driven wake -- back to the original. The recv thread
        // signals new_scene on every publish, render fires then.
        if (g.waitable && buf->new_scene) {
            HANDLE waits[2] = { g.waitable, buf->new_scene };
            WaitForMultipleObjects(2, waits, FALSE, 50);
        } else if (buf->new_scene) {
            WaitForSingleObject(buf->new_scene, 50);
        }

        // (Re-)find the overlay window. Discord recreates the HWND on
        // a variety of UI events; rather than yank D3D every time, we
        // observe a grace period and only teardown if the HWND really
        // stays gone.
        if (!target || !IsWindow(target)) {
            HWND fresh = FindWindowW(K_WND_CLASS, K_WND_CAPTION);
            if (fresh && fresh != target) {
                // New HWND -- swap chain is bound to the old one, must
                // be rebuilt. Teardown immediately for the new one.
                wda_applied = false;
                gfx_teardown(g);
                target = fresh;
                SetWindowDisplayAffinity(target, WDA_EXCLUDEFROMCAPTURE);
                wda_applied   = true;
                hwnd_lost_us  = 0;
            } else if (fresh == target && target != nullptr) {
                // Same HWND, just transiently failed IsWindow (rare).
                hwnd_lost_us = 0;
            } else {
                // HWND is gone. Honour the grace window before tearing
                // down so brief recreations don't visibly stutter.
                uint64_t t = now_us();
                if (hwnd_lost_us == 0) hwnd_lost_us = t;
                if ((t - hwnd_lost_us) > HWND_GRACE_US) {
                    if (g.ready) {
                        wda_applied = false;
                        gfx_teardown(g);
                    }
                    target = nullptr;
                }
                continue;
            }
        } else {
            hwnd_lost_us = 0;
            if (!wda_applied) {
                SetWindowDisplayAffinity(target, WDA_EXCLUDEFROMCAPTURE);
                wda_applied = true;
            }
        }

        // Panic-hide poll -- cheap, no syscall.
        bool panic_now = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
        buf->panic.store(panic_now, std::memory_order_relaxed);

        uint64_t t = now_us();
        bool live = is_live(*buf, t);

        // Lazy bring-up.
        if (live && !g.ready) {
            if (!gfx_init(g, target)) continue;
        }

        // Dormancy teardown -- free GPU resources after 30 s silence.
        if (g.ready && !live) {
            uint64_t last = buf->last_packet_us.load(std::memory_order_acquire);
            if (last == 0 || (t - last) > K_TEARDOWN_US) {
                gfx_teardown(g);
                wda_applied = false;
                continue;
            }
        }

        if (g.ready) {
            gfx_resize(g, target);
            const Slot* slot = acquire(*buf);

            // Staleness gate. If the last packet is older than
            // K_STALE_THRESHOLD_US, treat the scene as stale and blank
            // the overlay this frame. Avoids the "frozen ESP" look
            // during loader DMA stalls.
            uint64_t now_for_stale = now_us();
            uint64_t last_pkt = buf->last_packet_us.load(std::memory_order_relaxed);
            bool stale = (last_pkt == 0) ||
                         ((now_for_stale > last_pkt) &&
                          (now_for_stale - last_pkt) > K_STALE_THRESHOLD_US);

            bool draw_live = live && !buf->panic.load(std::memory_order_relaxed)
                                  && !stale;

            // Always Present when live -- the GPU's flip-model pipeline
            // expects a steady cadence to keep latency at the floor we
            // configured via SetMaximumFrameLatency(1). Skipping Present
            // when scene_id is unchanged was causing the waitable to
            // stall briefly when the loader's DMA tick paused for a
            // frame, manifesting as the "stutter / unresponsive for a
            // second" pattern. D2D work for a handful of boxes is
            // sub-millisecond, so always-render is the right call.
            if (draw_live) {
                last_entity_count = slot ? slot->scene.entity_count : 0;
                if (slot) {
                    uint32_t sid = slot->scene.scene_id;
                    if (sid != last_seen_scene_id) {
                        if (last_scene_advance_us != 0) {
                            uint64_t gap = now_us() - last_scene_advance_us;
                            if (gap > 50'000) {
                                ++stall_count;
                                ++rate_stalls;
                            }
                        }
                        last_seen_scene_id    = sid;
                        last_scene_advance_us = now_us();
                        ++rate_scenes;
                    }
                }
                render_frame(g, slot, buf);
                ++frames_drawn;
                ++rate_frames;
            } else {
                // Dormant / panic: keep Presenting empty frames so the
                // swap chain pipeline doesn't go stale, but don't burn
                // CPU on a redraw if nothing has changed.
                if (buf->last_drawn_scene_id != 0xFFFFFFFEu) {
                    render_frame(g, nullptr, buf);
                    buf->last_drawn_scene_id = 0xFFFFFFFEu;
                }
            }
        }

        // Iteration-time tracking -- counts only, no per-event log spam.
        // Counter is surfaced via the on-screen HUD `st:` field.
        uint64_t iter_now = now_us();
        if (last_iter_us != 0) {
            uint64_t dt = iter_now - last_iter_us;
            if (dt > 25'000) ++rate_stalls;
        }
        last_iter_us = iter_now;

        // 1-second live-rate update for the on-screen HUD.
        if (iter_now - rate_window_us >= 1'000'000) {
            uint32_t raw_now = g_stats.raw_count.load(std::memory_order_relaxed);
            uint32_t raw_delta = raw_now - last_raw_count;
            last_raw_count = raw_now;

            g_stats.fps.store(rate_frames,    std::memory_order_relaxed);
            g_stats.pkts.store(raw_delta,     std::memory_order_relaxed);
            g_stats.uniq.store(rate_scenes,   std::memory_order_relaxed);
            g_stats.stalls.fetch_add(rate_stalls, std::memory_order_relaxed);
            rate_frames = 0;
            rate_scenes = 0;
            rate_stalls = 0;
            rate_window_us = iter_now;
        }

        // throttled status, ~every 30 s
        uint64_t t2 = now_us();
        if (t2 - last_log_us > 30'000'000) {
            last_log_us = t2;
            dlog("render: hwnd=%p ready=%d live=%d frames=%llu last_ent=%u",
                 (void*)target, g.ready ? 1 : 0, live ? 1 : 0,
                 (unsigned long long)frames_drawn,
                 (unsigned)last_entity_count);
        }
    }

    gfx_teardown(g);
    dlog("render: thread exiting");
}

// ----- WSA singleton --------------------------------------------------------

struct WsaScope {
    bool ok;
    WsaScope() { WSADATA d{}; ok = (WSAStartup(MAKEWORD(2, 2), &d) == 0); }
    ~WsaScope() { if (ok) WSACleanup(); }
};

} // namespace

// ---- public entry ----------------------------------------------------------

// Snapshot the process list and return true if any process whose image
// name (lowercased) starts with "discord." is alive. Used to bound the
// daemon's lifetime without OpenProcess'ing Discord.
bool any_discord_process_alive() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return true; // err on the side of staying alive
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            wchar_t name[MAX_PATH];
            wcsncpy_s(name, pe.szExeFile, _TRUNCATE);
            for (wchar_t* p = name; *p; ++p) *p = (wchar_t)towlower(*p);
            // Match anything starting with "discord." -- covers
            // Discord.exe / DiscordPTB.exe / DiscordCanary.exe.
            if (wcsncmp(name, L"discord", 7) == 0) {
                const wchar_t* dot = wcschr(name, L'.');
                if (dot) { found = true; break; }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

int run_overlay_daemon() {
    WsaScope wsa;
    if (!wsa.ok) return 1;

    // Bump scheduler granularity from the default 15.6 ms tick down to
    // 1 ms. The render loop's WaitForSingleObject and dormancy checks
    // honour this immediately. Effect on the rest of the system is
    // negligible.
    timeBeginPeriod(1);

    // NtSetTimerResolution -- pushes the scheduler tick to 0.5 ms,
    // tighter than timeBeginPeriod alone. Same path TimerResolution.exe
    // and every modern game uses; not flagged by Vanguard / EAC / BE.
    typedef LONG (WINAPI *NtSetTimerResolution_t)(ULONG, BOOLEAN, PULONG);
    if (auto ntdll = GetModuleHandleW(L"ntdll.dll")) {
        if (auto pfn = (NtSetTimerResolution_t)GetProcAddress(
                ntdll, "NtSetTimerResolution")) {
            ULONG current = 0;
            pfn(5000, TRUE, &current);
        }
    }

    SceneBuf buf{};
    // Auto-reset event -- one SetEvent unblocks one WaitForSingleObject.
    buf.new_scene = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    std::atomic<bool> stop{ false };

    std::thread rxt(recv_loop,   &buf, &stop);
    std::thread rdt(render_loop, &buf, &stop);

    // Main thread bounds the daemon's lifetime by polling the process
    // list for any Discord-family executable. If none has been alive for
    // two consecutive checks (~10s), signal stop. Discord can take a
    // few seconds to appear after launch_discord(), so we hold off the
    // exit decision for the first 30s no matter what.
    int  empty_streak = 0;
    auto t0 = std::chrono::steady_clock::now();
    while (!stop.load(std::memory_order_acquire)) {
        Sleep(5000);
        bool alive = any_discord_process_alive();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (!alive) {
            if (elapsed < 30) continue;  // still in startup window
            if (++empty_streak >= 2) {
                stop.store(true, std::memory_order_release);
                break;
            }
        } else {
            empty_streak = 0;
        }
    }

    // Signal once more so render thread breaks out of its wait promptly.
    if (buf.new_scene) SetEvent(buf.new_scene);

    rxt.join();
    rdt.join();
    if (buf.new_scene) CloseHandle(buf.new_scene);
    timeEndPeriod(1);
    return 0;
}

} // namespace dnp
