// The guest's time: what the timers, the video timing, the sound and the
// frame pacing read. The host's steady clock since start, less the time the
// game spent paused (the settings menu).
#pragma once

#include <cstdint>

namespace dreamcast {

uint64_t guest_ns();
// Convert without overflowing the intermediate product after days of uptime.
// Rates are samples/second; audio-rate results fit across the timestamp range.
constexpr uint64_t samples_at(uint64_t nanoseconds, uint32_t rate) {
    return (nanoseconds / 1'000'000'000u) * rate +
           (nanoseconds % 1'000'000'000u) * rate / 1'000'000'000u;
}
// Stops the guest's time (true) and starts it again (false).
void pause_guest_time(bool paused);

}  // namespace dreamcast
