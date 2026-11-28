// Fibers: host call stacks to switch between on one host thread. The game's
// libraries run cooperative threads (setjmp/longjmp onto another guest
// stack); each guest thread's lifted code runs on its own fiber. Switching
// saves only what the host ABI says a call preserves; the floating-point
// environment (the rounding mode FPSCR.RM sets) is shared, as the guest's
// FPSCR is.
//
// x86-64 (System V) and ARM64 on Linux and macOS: a few lines of assembly.
// Windows: the system's fibers. Runtime code: no std::format.
#pragma once

#include <cstddef>

namespace qilin::rt {

class Fiber {
public:
    using Entry = void (*)(void* arg);

    // The fiber the calling host thread is running on now (made on first use
    // from the thread itself).
    static Fiber& current();

    // A new fiber that will run entry(arg) when first switched to. Entry must
    // not return: switch away for good instead (it stops the process if it does).
    Fiber(Entry entry, void* arg, size_t stack_size = 1u << 20);
    ~Fiber();
    Fiber(const Fiber&) = delete;
    Fiber& operator=(const Fiber&) = delete;

    bool ok() const { return ok_; }

    // Suspends the current fiber and runs this one. Returns when something
    // switches back to the one that called it.
    void switch_to();

    // A new fiber's first code (internal: public for the platform's start callback).
    static void start(void* self);
    // For AddressSanitizer: the bounds of this fiber's stack, where they weren't known.
    void note_stack(const void* bottom, size_t size) {
        if (!bottom_) bottom_ = bottom, size_ = size;
    }

private:
    Fiber();  // the host thread's own stack
    void* sp_ = nullptr;      // saved stack pointer (or the system fiber, on Windows)
    // Allocated stack (none for the thread's own); Windows' fibers keep their own.
    [[maybe_unused]] void* stack_ = nullptr;
    [[maybe_unused]] size_t stack_size_ = 0;
    Entry entry_ = nullptr;
    void* arg_ = nullptr;
    bool ok_ = false;
    // For AddressSanitizer, which must be told of each switch: the stack's bounds.
    const void* bottom_ = nullptr;
    size_t size_ = 0;
};

}  // namespace qilin::rt
