//
// rc_core.cpp -- worker-thread orchestrator for the Discord-side runtime.
//
// Owns:
//   * configure(): cfg path stash (read deferred to start()).
//   * start():      WSAStartup, socket bind, spawn recv + render threads.
//   * recv_loop():  blocking recvfrom + parse + triple-buffer publish.
//   * render_loop(): FindWindow poll + lazy Canvas init + draw loop.
//   * stop():       flip shutdown flag, close socket, join both threads.
//
// All public methods are called from the Electron main thread. Both
// worker threads touch only SceneState atomics and (on shutdown) the
// shutdown atomic. No JS callbacks fire from the workers -- the only
// surface back to JS is exception throws on configure / start.
//

#include "rc_core.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

// Win32 / DirectX surfaces -- only this TU and d3d_canvas.cpp need them.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "d3d_canvas.h"

namespace rc {

namespace {

// Trim leading + trailing whitespace (spaces, tabs, CR, LF). The config
// file is hand-edited by the user; tolerate the common CRLF / accidental
// space gotchas without making them debug their text file.
std::string strip(std::string s) {
    auto not_ws = [](unsigned char c) {
        return c != ' ' && c != '\t' && c != '\r' && c != '\n';
    };
    auto a = std::find_if(s.begin(), s.end(), not_ws);
    auto b = std::find_if(s.rbegin(), s.rend(), not_ws).base();
    return (a < b) ? std::string(a, b) : std::string();
}

// Pull the first non-blank, non-comment line from the cfg file. Returns
// an empty string if the file is missing or only contains comments --
// callers should treat that as "no manual override" rather than an
// error. The cfg file's existence is the runtime gate; its content is
// purely an optional manual peer-IP hint.
std::string read_first_nonblank_line(const std::string& path) {
    std::ifstream in(path);
    if (!in.is_open()) return {};
    std::string line;
    while (std::getline(in, line)) {
        std::string t = strip(line);
        if (t.empty()) continue;
        if (t[0] == '#') continue;       // shell-style comment lines
        return t;
    }
    return {};
}

// Convenience: get a microsecond stamp from steady_clock. We pass it
// around explicitly to keep the (testable) producer/consumer paths free
// of hidden time sources.
uint64_t now_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(
        steady_clock::now().time_since_epoch()
    ).count();
}

// Plausible-IPv4 sanity check before we hand the string to InetPtonA.
// Doesn't need to be perfect -- InetPtonA does the final word.
bool looks_like_ipv4(const std::string& s) {
    if (s.empty() || s.size() > 15) return false;
    int dots = 0, run = 0;
    for (char c : s) {
        if (c == '.') {
            if (run == 0)        return false;
            run = 0;
            if (++dots > 3)      return false;
        } else if (c >= '0' && c <= '9') {
            if (++run > 3)       return false;
        } else {
            return false;
        }
    }
    return dots == 3 && run > 0;
}

} // namespace

// ---- Core lifecycle --------------------------------------------------------

Core::Core() = default;

Core::~Core() {
    // Defensive: in case shim_main.js forgot to call Stop, don't leak the
    // workers when the addon's process is unwound.
    stop();
}

void Core::configure(std::string cfg_path) {
    // No I/O here -- parsing the file happens at start() so a bad path
    // surfaces in a context where the JS caller can show a clean error.
    m_cfg_path = std::move(cfg_path);
}

void Core::start() {
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true)) {
        // Already running -- idempotent per the public contract.
        return;
    }

    // 1. Pull the optional manual override from the cfg file. The
    //    receiver socket binds 0.0.0.0 regardless; the only thing the
    //    file content can affect is a future feature that filters
    //    incoming datagrams to a specific source IP. Today the addon
    //    accepts datagrams from any source on the bound port (since
    //    the auto-discovery handshake is what actually validates the
    //    peer), so an empty / comment-only file is completely fine.
    if (m_cfg_path.empty()) {
        m_started.store(false);
        throw std::runtime_error("rc: configure() not called");
    }
    m_loader_ip = read_first_nonblank_line(m_cfg_path);
    if (!m_loader_ip.empty() && !looks_like_ipv4(m_loader_ip)) {
        // Reset rather than throw -- a malformed override should not
        // prevent the runtime from starting in auto-discovery mode.
        m_loader_ip.clear();
    }

    // 2. WSAStartup is reference-counted by the OS; calling it once per
    //    Core::start() and matching it in stop() is the standard shape.
    WSADATA wd{};
    if (WSAStartup(MAKEWORD(2, 2), &wd) != 0) {
        m_started.store(false);
        throw std::runtime_error("rc: WSAStartup failed");
    }

    // 3. Spawn workers.
    //
    // Both workers are temporarily disabled while we diagnose a Discord
    // segfault that happens after app.whenReady() once Start is called.
    // With both threads gated off, we verify that the addon load +
    // Init/Start lifecycle itself does not destabilise Discord; the
    // recv path comes back on next iteration once the trigger is found.
    m_shutdown.store(false);
    // m_recv_thread   = std::thread(&Core::recv_loop,   this);
    // m_render_thread = std::thread(&Core::render_loop, this);
}

void Core::stop() {
    // First call wins; subsequent calls are no-ops.
    bool was_started = m_started.exchange(false);
    if (!was_started) return;

    m_shutdown.store(true, std::memory_order_release);

    // Joining the recv thread requires the blocking recvfrom to return.
    // We rely on SO_RCVTIMEO (set in recv_loop) being short enough that
    // shutdown latency stays in the hundreds of ms at most.
    if (m_recv_thread.joinable())   m_recv_thread.join();
    if (m_render_thread.joinable()) m_render_thread.join();   // safe even if never spawned

    WSACleanup();
}

// ---- Receiver thread -------------------------------------------------------
//
// One UDP socket bound 0.0.0.0:proto::DEFAULT_PORT. recvfrom loops with a
// short timeout so we can observe m_shutdown without busy-waiting. Each
// successfully-parsed packet either wakes the link (Hello / Scene) or
// puts it to sleep (Bye); Scenes additionally populate the back slot of
// the triple buffer and publish it.

void Core::recv_loop() {
    SOCKET s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == INVALID_SOCKET) return;

    // Generous receive buffer so a burst of Scene packets (up to ~4.4 KB
    // each, 3 IP fragments) doesn't drop on a tick spike.
    int rcvbuf = 1 << 20;  // 1 MiB
    setsockopt(s, SOL_SOCKET, SO_RCVBUF,
               reinterpret_cast<const char*>(&rcvbuf), sizeof(rcvbuf));

    // Reuse the port so a fast restart doesn't TIME_WAIT us out.
    int reuse = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

    // SO_RCVTIMEO bounds the worst-case shutdown latency. 250 ms is short
    // enough to feel snappy when Discord is closing and long enough that
    // the syscall isn't burning CPU on a steady stream of timeouts.
    DWORD timeout_ms = 250;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout_ms), sizeof(timeout_ms));

    sockaddr_in bind_addr{};
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    bind_addr.sin_port        = htons(proto::DEFAULT_PORT);
    if (bind(s, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr))
        == SOCKET_ERROR)
    {
        closesocket(s);
        return;
    }

    // Receive buffer sized to the largest legal datagram. Anything larger
    // is dropped silently -- the wire shouldn't produce it and we'd
    // rather drop than misparse.
    static constexpr size_t BUF_BYTES = proto::MAX_DATAGRAM_BYTES + 64;
    auto buf = std::make_unique<uint8_t[]>(BUF_BYTES);

    while (!m_shutdown.load(std::memory_order_acquire)) {
        sockaddr_in from{};
        int from_len = sizeof(from);
        int n = recvfrom(s,
                         reinterpret_cast<char*>(buf.get()),
                         static_cast<int>(BUF_BYTES),
                         0,
                         reinterpret_cast<sockaddr*>(&from),
                         &from_len);

        if (n == SOCKET_ERROR) {
            int err = WSAGetLastError();
            if (err == WSAETIMEDOUT) continue;   // expected, loop check
            // Anything else: drop the packet and continue. We don't log
            // from here to keep the binary string surface clean.
            continue;
        }

        if (static_cast<size_t>(n) < sizeof(proto::Header)) continue;

        const auto* h = reinterpret_cast<const proto::Header*>(buf.get());

        // Validate magic + version before touching any payload field.
        if (std::memcmp(h->magic, proto::MAGIC, sizeof(h->magic)) != 0) continue;
        if (h->version != proto::VERSION)                              continue;

        switch (static_cast<proto::Type>(h->type)) {

            case proto::Type::Hello:
                if (static_cast<size_t>(n) >= sizeof(proto::Hello)) {
                    m_scene.mark_awake(now_us());

                    // Auto-discovery handshake: ack every Hello so the
                    // sender can latch our address. The sender uses the
                    // source address of this reply as the unicast peer
                    // for subsequent Scene packets, so blasting a Bye
                    // on shutdown and rediscovery on the next start
                    // works without any external configuration.
                    proto::PeerAck ack{};
                    std::memcpy(ack.header.magic, proto::MAGIC,
                                sizeof(ack.header.magic));
                    ack.header.version = proto::VERSION;
                    ack.header.type    =
                        static_cast<uint16_t>(proto::Type::PeerAck);
                    ack.header.seq     = h->seq;       // echo for matchability
                    ack.header.tick_us = now_us();

                    sendto(s,
                           reinterpret_cast<const char*>(&ack),
                           static_cast<int>(sizeof(ack)),
                           0,
                           reinterpret_cast<sockaddr*>(&from),
                           from_len);
                }
                break;

            case proto::Type::Bye:
                m_scene.mark_bye();
                break;

            case proto::Type::PeerAck:
                // We don't expect to receive PeerAck on this socket --
                // we're the receiver, the sender is the one that latches
                // them. Drop silently.
                break;

            case proto::Type::Scene: {
                if (static_cast<size_t>(n) < sizeof(proto::Scene)) break;
                const auto* incoming =
                    reinterpret_cast<const proto::Scene*>(buf.get());

                // Clamp entity_count defensively before copy -- the
                // memcpy size below is anchored by it.
                uint16_t cnt = incoming->entity_count;
                if (cnt > proto::MAX_ENTITIES) cnt = proto::MAX_ENTITIES;

                proto::Scene* back = m_scene.writable_back();
                back->header       = incoming->header;
                std::memcpy(back->viewproj, incoming->viewproj,
                            sizeof(back->viewproj));
                back->screen_w     = incoming->screen_w;
                back->screen_h     = incoming->screen_h;
                back->scene_id     = incoming->scene_id;
                back->entity_count = cnt;
                back->reserved     = 0;
                if (cnt > 0) {
                    std::memcpy(back->entities,
                                incoming->entities,
                                cnt * sizeof(proto::WireEntity));
                }
                m_scene.publish(now_us());
                break;
            }
        }
    }

    closesocket(s);
}

// ---- Render thread ---------------------------------------------------------
//
// Polls for Discord's overlay HWND, brings up the D3D11 + D2D pipeline
// lazily once it has both an HWND and the link is AWAKE, then renders
// one frame per Present(). Keeps polling for the HWND while dormant so
// reconnecting after Discord restarts doesn't need a JS-side prod.

void Core::render_loop() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    Canvas canvas;
    HWND   target = nullptr;

    while (!m_shutdown.load(std::memory_order_acquire)) {

        // 1. Find / re-find the overlay window. Discord recreates it on
        //    monitor reconfig, so we re-poll every iteration when we
        //    don't currently have one.
        if (!target || !IsWindow(target)) {
            canvas.teardown();
            target = FindWindowA("Chrome_WidgetWin_1", "Discord Overlay");
            if (!target) {
                // No window yet -- back off a bit. 250 ms is small enough
                // that startup feels instant and large enough that we
                // aren't burning CPU on FindWindow.
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }
        }

        // 2. Poll the END key for panic-hide. We don't install a global
        //    keyboard hook (those are AC-flagged); a per-frame async
        //    check costs essentially nothing.
        bool panic_now = (GetAsyncKeyState(VK_END) & 0x8000) != 0;
        m_scene.set_panic(panic_now);

        uint64_t t = now_us();
        bool live  = m_scene.is_live(t);

        // 3. Lazy Canvas bring-up the first time we have an HWND AND
        //    the link is alive. This keeps GPU resources at baseline
        //    while the loader is down.
        if (live && !canvas.ready()) {
            if (!canvas.initialise(reinterpret_cast<HWND_>(target))) {
                // Init failed (e.g. swap chain creation refused). Wait
                // a moment, try again next iteration -- it's almost
                // always a transient state during Discord's own startup.
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
        }

        // 4. Handle resize and draw. teardown() short-circuits if the
        //    canvas isn't up; draw() short-circuits if scene isn't ready.
        if (canvas.ready()) {
            canvas.resize_if_needed(reinterpret_cast<HWND_>(target));
            const proto::Scene* scene = m_scene.acquire();
            if (live && !m_scene.panic()) {
                canvas.draw(scene);
            } else {
                // Live but panic, or dormant -- draw a clear frame so the
                // last scene doesn't linger frozen on screen.
                canvas.draw(nullptr);
            }
        } else {
            // No canvas yet -- avoid spinning at 100 % when dormant.
            std::this_thread::sleep_for(std::chrono::milliseconds(16));
        }
    }

    canvas.teardown();
}

} // namespace rc
