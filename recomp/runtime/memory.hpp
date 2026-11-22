// The guest's address space for the runtime: 4 GB reserved at a host base
// (the runtime sets rt::g_mem to it), with each of the Dreamcast's memories mapped at every address
// the hardware decodes to it. Main RAM (16 MiB) appears in area 3 four times
// (0x0C000000-0x0FFFFFFF) and that area at P0, P1 (cached), P2 (uncached)
// and P3: sixteen views of one block, so a guest load or store stays one host
// access with no address arithmetic. Everything not mapped faults.
//
// The compiler doesn't know that two of these addresses are one byte: where
// it can prove two accesses use different offsets from the base (constant
// addresses at two aliases in one function), it may reorder them. Lifted code
// that writes through one alias and reads through another has to be lifted
// with that in mind (an open point: canonical addresses for constants).
//
// Runtime code: no std::format.
#pragma once

#include <cstddef>
#include <cstdint>

namespace qilin::rt {

class GuestMemory {
public:
    static constexpr uint32_t kRamSize = 16u << 20;
    static constexpr uint32_t kRamArea = 0x0C000000u;  // area 3: RAM and its three mirrors

    // Reserves the address space and maps main RAM at all its aliases. On
    // failure, `error()` says why and nothing stays mapped. Independent of
    // guest.hpp, so checked and release code can share it.
    GuestMemory();
    ~GuestMemory();
    GuestMemory(const GuestMemory&) = delete;
    GuestMemory& operator=(const GuestMemory&) = delete;

    bool ok() const { return base_ != nullptr; }
    const char* error() const { return error_; }
    uint8_t* base() const { return base_; }
    uint8_t* ram() const { return ram_view_; }  // the block itself, at its cached address

    // The guest addresses main RAM appears at (for tests and diagnostics).
    static constexpr unsigned kRamViews = 16;
    static uint32_t ram_view(unsigned i) {  // i: 0..15 -> segment (P0, P1, P2, P3) x mirror
        static constexpr uint32_t segments[4] = {0x00000000u, 0x80000000u, 0xA0000000u, 0xC0000000u};
        return segments[i / 4] | (kRamArea + (i % 4) * kRamSize);
    }

private:
    uint8_t* base_ = nullptr;
    uint8_t* ram_view_ = nullptr;
    const char* error_ = nullptr;
    struct Native;  // the platform's handles
    Native* native_ = nullptr;
    void release();
};

}  // namespace qilin::rt
