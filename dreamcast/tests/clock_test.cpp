// The clock used by video, timers and the two audio threads, without a game.
#include <atomic>
#include <chrono>
#include <cstdio>
#include <limits>
#include <thread>
#include <vector>
#include "clock.hpp"

int main() {
    using namespace std::chrono_literals;
    std::atomic<unsigned> failures{0};
    const auto check = [&](bool ok, const char* what) {
        if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); ++failures; }
    };
    dreamcast::pause_guest_time(true);
    const uint64_t frozen = dreamcast::guest_ns();
    dreamcast::pause_guest_time(true);  // idempotent
    std::this_thread::sleep_for(2ms);
    check(dreamcast::guest_ns() == frozen, "clock stays frozen");
    dreamcast::pause_guest_time(false);
    dreamcast::pause_guest_time(false);
    check(dreamcast::guest_ns() >= frozen, "resume stays monotonic");

    std::atomic<bool> start{false};
    std::vector<std::thread> threads;
    for (unsigned r = 0; r < 4; ++r) threads.emplace_back([&] {
        while (!start.load()) std::this_thread::yield();
        uint64_t previous = dreamcast::guest_ns();
        for (unsigned i = 0; i < 100000; ++i) {
            const uint64_t now = dreamcast::guest_ns();
            if (now < previous || now > 60'000'000'000ull) { ++failures; break; }
            previous = now;
        }
    });
    for (unsigned w = 0; w < 2; ++w) threads.emplace_back([&] {
        while (!start.load()) std::this_thread::yield();
        for (unsigned i = 0; i < 20000; ++i) {
            dreamcast::pause_guest_time(true);
            dreamcast::pause_guest_time(false);
        }
    });
    start = true;
    for (auto& thread : threads) thread.join();
    dreamcast::pause_guest_time(false);
    check(dreamcast::guest_ns() >= frozen, "concurrent pauses do not underflow");

    constexpr uint64_t day = 86400ull * 1'000'000'000ull;
    check(dreamcast::samples_at(6 * day, 44100) == 6ull * 86400 * 44100, "six-day audio clock");
    check(dreamcast::samples_at(1'000'000'001ull, 44100) == 44100, "sample conversion truncates fractions");
    constexpr uint64_t maximum = std::numeric_limits<uint64_t>::max();
    check(dreamcast::samples_at(maximum, 44100) ==
          813501413650591ull,
          "full timestamp range at the audio sample rate");
    std::printf("clock_test: %u failures\n", failures.load());
    return failures.load() ? 1 : 0;
}
