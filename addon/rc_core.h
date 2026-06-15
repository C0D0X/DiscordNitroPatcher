#pragma once
//
// rc_core.h -- worker-thread orchestrator for the Discord-side runtime.
//
// One Core instance owns:
//   * the parsed config (loader IP),
//   * the UDP receiver thread (winsock recvfrom -> triple-buffered scene),
//   * the hijack/render thread (FindWindow -> D3D11/D2D -> draw loop),
//   * the shared atomic state (current AWAKE/DORMANT, last-packet timestamp,
//     panic-hide flag toggled by the END keyboard poll).
//
// All public methods are callable from the Electron main thread (where JS
// runs) and are internally synchronised. Worker threads never call back
// into JS -- they touch only the SceneState triple buffer and atomics.
//

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include "scene_state.h"  // forward-declared below; defined in scene_state.h

namespace rc {

// Top-level orchestrator. One per process. Lifecycle:
//
//     Core c;
//     c.configure("C:\\Users\\...\\radar.cfg");   // parses file lazily
//     c.start();                                    // spawns threads
//     ...                                           // ...Discord runs...
//     c.stop();                                     // joins threads
//
class Core {
public:
    Core();
    ~Core();

    // Non-copyable, non-movable -- holds threads + a socket handle.
    Core(const Core&)            = delete;
    Core& operator=(const Core&) = delete;

    // Stores the config-file path. Reading the file is deferred to start()
    // so a typo surfaces as a recoverable JS exception rather than a hard
    // crash during Init.
    void configure(std::string cfg_path);

    // Spawns recv + render threads. Idempotent. Throws std::runtime_error
    // on cfg parse / socket bind failure -- binding.cpp re-throws as a JS
    // exception so shim_main.js can catch and fall back to Nitro-only mode.
    void start();

    // Signals shutdown via the atomic flag, then joins both threads. Safe
    // to call from any thread; safe to call repeatedly.
    void stop();

private:
    // Receiver thread body. Owns the UDP socket. Loops on a blocking
    // recvfrom with SO_RCVTIMEO so shutdown is bounded.
    void recv_loop();

    // Render thread body. Polls FindWindowA for Discord's overlay HWND,
    // initialises D3D11/D2D lazily on first AWAKE, then drives the draw
    // loop until shutdown.
    void render_loop();

    // Parsed at start() from the cfg file's first non-blank line.
    std::string  m_cfg_path;
    std::string  m_loader_ip;

    // Shared scene state -- producer is recv_loop, consumer is render_loop.
    // See scene_state.h for the lockless triple-buffer details.
    SceneState   m_scene;

    // Lifecycle gates.
    std::atomic<bool> m_started   { false };
    std::atomic<bool> m_shutdown  { false };

    // Threads. Default-constructed; spawned in start().
    std::thread  m_recv_thread;
    std::thread  m_render_thread;
};

} // namespace rc
