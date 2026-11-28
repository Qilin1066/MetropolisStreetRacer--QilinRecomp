// Fibers: see fiber.hpp. Runtime code: no std::format.
#include "fiber.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

#if defined(__SANITIZE_ADDRESS__)
#define QILIN_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define QILIN_ASAN 1
#endif
#endif
#if defined(QILIN_ASAN)
#include <sanitizer/common_interface_defs.h>
#endif

#if defined(_WIN32)
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace qilin::rt {

namespace {
thread_local Fiber* t_current = nullptr;
[[maybe_unused]] thread_local Fiber* t_from = nullptr;  // the fiber the last switch left (not on Windows)

// AddressSanitizer must be told of every switch, with the bounds of the stack
// switched to. Those of the thread's own stack are learnt the first time it
// is switched away from: the side that wakes up reports the stack it left.
void asan_arrived([[maybe_unused]] void* fake) {
#if defined(QILIN_ASAN)
    const void* bottom = nullptr;
    size_t size = 0;
    __sanitizer_finish_switch_fiber(fake, &bottom, &size);
    if (t_from) t_from->note_stack(bottom, size);
#endif
}

[[noreturn]] void die(const char* why) {
    std::fputs("qilin: ", stderr);
    std::fputs(why, stderr);
    std::fputc('\n', stderr);
    std::abort();
}
}  // namespace

// A fiber's first code: runs its entry, which must never return (nor throw:
// what it runs catches everything it can recover from).
void Fiber::start(void* self) {
    Fiber* f = static_cast<Fiber*>(self);
    asan_arrived(nullptr);
    try {
        f->entry_(f->arg_);
    } catch (...) {
        die("an exception left a fiber");
    }
    die("a fiber's entry returned");
}

Fiber& Fiber::current() {
    if (!t_current) t_current = new Fiber();  // the thread's own stack; lives as long as the thread
    return *t_current;
}

#if defined(_WIN32)

namespace {
VOID CALLBACK fiber_main(LPVOID self);
}  // namespace

Fiber::Fiber() {
    sp_ = ConvertThreadToFiber(nullptr);
    if (!sp_ && GetLastError() == ERROR_ALREADY_FIBER) sp_ = GetCurrentFiber();
    ok_ = sp_ != nullptr;
}

Fiber::Fiber(Entry entry, void* arg, size_t stack_size) : stack_size_(stack_size), entry_(entry), arg_(arg) {
    current();  // the thread must be a fiber before it can switch to one
    sp_ = CreateFiber(stack_size, fiber_main, this);
    ok_ = sp_ != nullptr;
}

Fiber::~Fiber() {
    if (entry_ && sp_) DeleteFiber(sp_);
}

void Fiber::switch_to() {
    if (&current() == this) return;
    t_current = this;
    SwitchToFiber(sp_);
}

namespace {
VOID CALLBACK fiber_main(LPVOID self) { Fiber::start(self); }
}  // namespace

#else

// The switch: push the callee-saved registers, save the stack pointer through
// the first argument, load the second, pop and return into the other fiber.
// A new fiber's stack is laid out as if it had switched away, returning into
// the trampoline, which calls Fiber::start(this).
#if defined(__APPLE__)
#define QILIN_SYM(name) "_" #name
#else
#define QILIN_SYM(name) #name
#endif

extern "C" void qilin_fiber_switch(void** save, void* to);
extern "C" void qilin_fiber_trampoline();

#if defined(__x86_64__)
// System V: rbx, rbp, r12-r15 are preserved; no vector register is.
__asm__(".text\n"
        ".globl " QILIN_SYM(qilin_fiber_switch) "\n" QILIN_SYM(qilin_fiber_switch) ":\n"
        "  pushq %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
        "  movq %rsp, (%rdi)\n"
        "  movq %rsi, %rsp\n"
        "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbx\n  popq %rbp\n"
        "  ret\n"
        ".globl " QILIN_SYM(qilin_fiber_trampoline) "\n" QILIN_SYM(qilin_fiber_trampoline) ":\n"
        "  movq %r12, %rdi\n"
        "  callq *%r13\n"
        "  ud2\n");
constexpr unsigned kSaved = 6;  // r15, r14, r13, r12, rbx, rbp (lowest address first), then the return address
#elif defined(__aarch64__)
// AAPCS64: x19-x29, x30 (the return address) and d8-d15 are preserved.
__asm__(".text\n"
        ".globl " QILIN_SYM(qilin_fiber_switch) "\n.p2align 2\n" QILIN_SYM(qilin_fiber_switch) ":\n"
        "  sub sp, sp, #176\n"
        "  stp x19, x20, [sp, #0]\n  stp x21, x22, [sp, #16]\n  stp x23, x24, [sp, #32]\n"
        "  stp x25, x26, [sp, #48]\n  stp x27, x28, [sp, #64]\n  stp x29, x30, [sp, #80]\n"
        "  stp d8, d9, [sp, #96]\n  stp d10, d11, [sp, #112]\n  stp d12, d13, [sp, #128]\n  stp d14, d15, [sp, #144]\n"
        "  mov x2, sp\n  str x2, [x0]\n"
        "  mov sp, x1\n"
        "  ldp x19, x20, [sp, #0]\n  ldp x21, x22, [sp, #16]\n  ldp x23, x24, [sp, #32]\n"
        "  ldp x25, x26, [sp, #48]\n  ldp x27, x28, [sp, #64]\n  ldp x29, x30, [sp, #80]\n"
        "  ldp d8, d9, [sp, #96]\n  ldp d10, d11, [sp, #112]\n  ldp d12, d13, [sp, #128]\n  ldp d14, d15, [sp, #144]\n"
        "  add sp, sp, #176\n"
        "  ret\n"
        ".globl " QILIN_SYM(qilin_fiber_trampoline) "\n.p2align 2\n" QILIN_SYM(qilin_fiber_trampoline) ":\n"
        "  mov x0, x20\n"
        "  blr x19\n"
        "  brk #0\n");
#else
#error "fibers: no context switch for this architecture (64-bit x86 and ARM only)"
#endif

Fiber::Fiber() : ok_(true) {}

Fiber::Fiber(Entry entry, void* arg, size_t stack_size) : stack_size_(stack_size), entry_(entry), arg_(arg) {
    const size_t page = static_cast<size_t>(sysconf(_SC_PAGESIZE));
    stack_size_ = (stack_size + page - 1) / page * page + page;  // and a guard page below
    void* stack = mmap(nullptr, stack_size_, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED) return;
    stack_ = stack;
    mprotect(stack_, page, PROT_NONE);
    bottom_ = static_cast<char*>(stack_) + page;
    size_ = stack_size_ - page;
    auto* top = reinterpret_cast<uintptr_t*>((reinterpret_cast<uintptr_t>(stack_) + stack_size_) & ~uintptr_t{15});
    const auto start_fn = reinterpret_cast<uintptr_t>(&Fiber::start);
    const auto self = reinterpret_cast<uintptr_t>(this);
    const auto trampoline = reinterpret_cast<uintptr_t>(&qilin_fiber_trampoline);
#if defined(__x86_64__)
    // After the pops and ret the stack pointer is `top`, 16-aligned, as a call needs.
    uintptr_t* s = top - (kSaved + 1);
    s[0] = s[1] = 0;            // r15, r14
    s[2] = start_fn;            // r13: what the trampoline calls
    s[3] = self;                // r12: its argument
    s[4] = s[5] = 0;            // rbx, rbp
    s[6] = trampoline;          // the return address
    sp_ = s;
#else
    uintptr_t* s = top - 176 / sizeof(uintptr_t);
    for (unsigned i = 0; i < 176 / sizeof(uintptr_t); ++i) s[i] = 0;
    s[0] = start_fn;            // x19: what the trampoline calls
    s[1] = self;                // x20: its argument
    s[11] = trampoline;         // x30
    sp_ = s;
#endif
    ok_ = true;
}

Fiber::~Fiber() {
    if (stack_) munmap(stack_, stack_size_);
}

void Fiber::switch_to() {
    Fiber& from = current();
    if (&from == this) return;
    t_current = this;
    t_from = &from;
    [[maybe_unused]] void* fake = nullptr;
#if defined(QILIN_ASAN)
    __sanitizer_start_switch_fiber(&fake, bottom_, size_);
#endif
    qilin_fiber_switch(&from.sp_, sp_);
    asan_arrived(fake);  // back on `from`
}

#endif

}  // namespace qilin::rt
