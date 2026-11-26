#include "clock.hpp"

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace dreamcast {

namespace {
using Clock = std::chrono::steady_clock;
const Clock::time_point g_start = Clock::now();
std::atomic<int64_t> g_paused_total{0};  // ns spent paused, before the current pause
std::atomic<int64_t> g_paused_at{-1};    // when the current pause began, or -1
std::atomic<uint64_t> g_sequence{0};  // even: a coherent clock state
std::mutex g_writer;                // serialise concurrent pause/resume requests
int64_t since_start() { return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - g_start).count(); }
}  // namespace

uint64_t guest_ns() {
    // The sound CPU reads this concurrently with the window pausing/resuming.
    // Sequentially consistent atomics keep both state loads between the
    // sequence loads; readers retry rather than mixing two clock states.
    for (;;) {
        const uint64_t sequence = g_sequence.load();
        if (sequence & 1u) {
            // The audio reader can outrank the window thread. Park briefly
            // rather than starving a pre-empted writer while its state is odd.
            std::this_thread::sleep_for(std::chrono::microseconds(1));
            continue;
        }
        const int64_t at = g_paused_at.load();
        const int64_t total = g_paused_total.load();
        const int64_t now = at >= 0 ? at : since_start();
        if (g_sequence.load() == sequence) return static_cast<uint64_t>(now - total);
    }
}

void pause_guest_time(bool paused) {
    const std::lock_guard lock(g_writer);
    const int64_t at = g_paused_at.load();
    if (paused == (at >= 0)) return;
    g_sequence.fetch_add(1);
    if (paused) {
        g_paused_at.store(since_start());
    } else {
        g_paused_total.fetch_add(since_start() - at);
        g_paused_at.store(-1);
    }
    g_sequence.fetch_add(1);
}

}  // namespace dreamcast
