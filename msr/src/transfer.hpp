#pragma once
#include <cstdint>
#include <limits>
#include <optional>

namespace msr::transfer {
constexpr uint32_t kSectorBytes = 2048, kLeadIn = 150;

// The drive exposes 32-bit sector addresses and byte counts. Check wide
// intermediates before any disc read, guest write or stream state change.
constexpr std::optional<uint32_t> read_bytes(uint32_t frame, uint32_t sectors) {
    if (frame < kLeadIn || !sectors) return std::nullopt;
    const uint64_t bytes = uint64_t(sectors) * kSectorBytes;
    const uint64_t end = uint64_t(frame - kLeadIn) + sectors;
    if (bytes > std::numeric_limits<uint32_t>::max() || end > (uint64_t{1} << 32)) return std::nullopt;
    return static_cast<uint32_t>(bytes);
}
constexpr bool address_range(uint32_t to, uint32_t bytes) {
    return uint64_t(to) + bytes <= (uint64_t{1} << 32);
}
}  // namespace msr::transfer
