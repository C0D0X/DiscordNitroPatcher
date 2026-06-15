//
// scene_state.cpp -- single-producer / single-consumer triple buffer.
//
// The design is a three-slot rotation:
//
//        +-----------+   produces into m_back, then publish() ->
//   --> | back  | -- atomically swaps m_back with m_ready
//        +-----------+
//        | ready | -- floating; whoever swaps first owns the latest data
//        +-----------+
//   --> | front | -- consumed from m_front, then acquire() ->
//        +-----------+   atomically swaps m_front with m_ready
//
// Both swaps are single std::atomic_exchange calls on uint8 indices, so
// neither side ever waits. The slots themselves are plain memory whose
// ownership rotates with the indices -- nothing dereferences a slot it
// doesn't currently own.
//

#include "scene_state.h"

namespace rc {

SceneState::SceneState() = default;

// ---- Producer side ---------------------------------------------------------

proto::Scene* SceneState::writable_back() {
    // Index value is loaded with acquire so any prior publish by the
    // consumer (acquire() does an exchange) is fully visible before we
    // touch the slot's storage.
    uint8_t idx = m_back.load(std::memory_order_acquire);
    return &m_slots[idx];
}

void SceneState::publish(uint64_t now_us) {
    // exchange returns the old ready index -- that becomes our new back
    // slot for the next write cycle. Both reads use acq_rel so the slot's
    // bytes are flushed before the consumer can see them through ready.
    uint8_t prev_back  = m_back.load(std::memory_order_acquire);
    uint8_t prev_ready = m_ready.exchange(prev_back, std::memory_order_acq_rel);
    m_back.store(prev_ready, std::memory_order_release);

    m_last_packet_us.store(now_us, std::memory_order_release);
    m_state.store(LinkState::Awake, std::memory_order_release);
}

void SceneState::mark_awake(uint64_t now_us) {
    m_last_packet_us.store(now_us, std::memory_order_release);
    m_state.store(LinkState::Awake, std::memory_order_release);
}

void SceneState::mark_bye() {
    // No data change, just an early dormant transition so the render
    // thread can stop drawing without waiting for the silence timeout.
    m_state.store(LinkState::Dormant, std::memory_order_release);
}

// ---- Consumer side ---------------------------------------------------------

const proto::Scene* SceneState::acquire() {
    uint8_t prev_front = m_front.load(std::memory_order_acquire);
    uint8_t prev_ready = m_ready.exchange(prev_front, std::memory_order_acq_rel);
    m_front.store(prev_ready, std::memory_order_release);
    return &m_slots[prev_ready];
}

bool SceneState::is_live(uint64_t now_us) const {
    if (m_state.load(std::memory_order_acquire) != LinkState::Awake) {
        return false;
    }
    uint64_t last = m_last_packet_us.load(std::memory_order_acquire);
    if (last == 0)                       return false;
    if (now_us <= last)                  return true;
    return (now_us - last) <= proto::DORMANT_TIMEOUT_US;
}

} // namespace rc
