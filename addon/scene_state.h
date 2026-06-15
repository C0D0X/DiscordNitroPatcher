#pragma once
//
// scene_state.h -- lockless triple-buffered handoff between the UDP receiver
// thread (producer) and the render thread (consumer).
//
// Why triple-buffering: the receiver writes a ~4.4 KB Scene struct on every
// tick (up to 144 Hz). The render thread reads at vsync (60-240 Hz). With a
// single buffer we'd tear; with double-buffer + mutex we'd block one of
// them periodically. The triple-buffer pattern lets each side hold its own
// slot while a third floats between them, so neither ever waits.
//
// Slots:
//   back  -- producer owns; written to during recv parsing.
//   ready -- the most-recent fully-written snapshot; floats between sides.
//   front -- consumer owns; read by render_loop, never reallocated.
//
// Atomic swap protocol:
//   Producer finishes writing -> swap(back, ready).
//   Consumer about to read    -> swap(ready, front).
//   Both swaps are single std::atomic_exchange on uint8 indices -- no CAS
//   loop, no spin, lock-free, wait-free for one producer + one consumer.
//
// Liveness fields are tracked alongside the buffer so the render thread can
// answer "did we go dormant?" without locking anything.
//

#include <atomic>
#include <cstdint>
#include <cstring>

#include "proto.h"  // shared wire protocol; defines proto::Scene etc.

namespace rc {

enum class LinkState : uint8_t {
    Dormant = 0,   // no packet ever, or none in the last DORMANT_TIMEOUT_US.
    Awake   = 1,   // at least one Scene received within the window.
};

class SceneState {
public:
    SceneState();

    // -- Producer side (recv thread) --------------------------------------

    // Returns a writable pointer to the current "back" slot. The producer
    // fills it in place, then calls publish() to swap it into "ready".
    proto::Scene* writable_back();

    // Atomically swaps the back slot with ready. After this call, the
    // consumer's next read will see the just-published snapshot.
    // Also updates m_last_packet_us so the dormant check can fire.
    void publish(uint64_t now_us);

    // Called on Hello / Bye / Scene receipt to advance the link state.
    // Hello + Scene set Awake; Bye sets Dormant immediately.
    void mark_awake(uint64_t now_us);
    void mark_bye();

    // -- Consumer side (render thread) ------------------------------------

    // Atomically swaps ready into front and returns a stable pointer to it.
    // The returned pointer is valid until the next acquire() call from the
    // same consumer thread. Safe to read all fields without locking.
    const proto::Scene* acquire();

    // True if the link is currently AWAKE and the last packet arrived
    // within the dormant timeout window. The render thread skips draw
    // entirely when this returns false.
    bool is_live(uint64_t now_us) const;

    // -- Panic-hide flag (rendered thread reads, key-poll writes) ---------
    void set_panic(bool v) { m_panic.store(v, std::memory_order_relaxed); }
    bool panic()     const { return m_panic.load(std::memory_order_relaxed); }

private:
    // Three slots, indices 0..2. Two indices are owned by the threads
    // (back, front); the third (ready) is the atomically-swapped middle.
    proto::Scene          m_slots[3]{};
    std::atomic<uint8_t>  m_back  { 0 };
    std::atomic<uint8_t>  m_ready { 1 };
    std::atomic<uint8_t>  m_front { 2 };

    std::atomic<LinkState> m_state         { LinkState::Dormant };
    std::atomic<uint64_t>  m_last_packet_us{ 0 };
    std::atomic<bool>      m_panic         { false };
};

} // namespace rc
