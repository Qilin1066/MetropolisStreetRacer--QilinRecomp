// Indirect calls through the generated function table. Runtime code: no
// std::format.
#include <algorithm>
#include <cstdlib>
#include <exception>
#include <memory>
#include <utility>
#include <vector>

#include "fiber.hpp"
#include "guest.hpp"
#include "system.hpp"

namespace qilin::rt {

namespace {
// The program's code is at its cached (P1) addresses. Start-up runs its cache
// set-up through the uncached alias (P2: or 0xA0000000), as it must on the
// console; there is no cache here, so an alias in P2 (or P0, untranslated) runs
// the same function.
uint32_t cached(uint32_t target) {
    const uint32_t area = target >> 29;
    return area == 5 || area <= 3 ? (target & 0x1FFFFFFFu) | 0x80000000u : target;
}

const FnEntry* find(uint32_t target) {
    const FnEntry* end = g_functions + g_function_count;
    const uint32_t a = cached(target);
    const FnEntry* it = std::lower_bound(g_functions, end, a, [](const FnEntry& e, uint32_t x) { return e.addr < x; });
    return it == end || it->addr != a ? nullptr : it;
}

// An address with no lifted function: native code the runtime provides (the
// boot ROM's system calls), which returns to PR; anything else stops.
void call_native(Context& c, uint32_t target, uint32_t from) {
    const Provided* p = native_at(cached(target));
    if (!p) bad_jump(from, target);
    p->fn(c);
    c.ret = c.pr;
}
}  // namespace

// Guest threads. The libraries' scheduler (0x8C147D78) runs each task by a
// longjmp into its context and the task yields by a setjmp of its own and a
// longjmp back; a new task's context returns to its entry function on a new
// guest stack. Each guest thread is a fiber here, with the setjmp sites armed
// in its frames; the host thread's own stack is the first.
struct GuestThread {
    std::unique_ptr<Fiber> fiber;  // none for the host thread's own
    Fiber* host = nullptr;         // the first thread's: the host thread's own fiber
    JumpSite* sites = nullptr;     // armed, innermost first
    uint32_t entry_sp = 0;         // the guest stack pointer it started with (0: the first thread)
    enum State { Running, Suspended, Done } state = Running;
    bool kill = false;                  // to end: unwind its frames when it next runs
    GuestThread* resumer = nullptr;     // the thread that last switched to it
    Context* c = nullptr;               // for a new thread: what to run
    Fn fn = nullptr;
    uint32_t resume = 0;
    std::exception_ptr error;           // why it ended, for its resumer to rethrow
};

bool g_guest_threads = false;

namespace {
thread_local GuestThread t_first;
thread_local GuestThread* t_running = nullptr;
thread_local std::vector<std::unique_ptr<GuestThread>> t_threads;  // all but the first
thread_local uint32_t t_resume = 0;
thread_local uint64_t t_arms = 0;

GuestThread& running() { return t_running ? *t_running : t_first; }

Fiber& fiber_of(GuestThread& t) { return t.fiber ? *t.fiber : *t.host; }

// Thrown on a thread that is being ended, to its fiber's first frame.
struct ThreadExit {};

const JumpSiteEntry* jump_site(uint32_t ret) {
    const JumpSiteEntry* end = g_jump_sites + g_jump_site_count;
    const JumpSiteEntry* it =
        std::lower_bound(g_jump_sites, end, ret, [](const JumpSiteEntry& e, uint32_t x) { return e.ret < x; });
    return it != end && it->ret == ret ? it : nullptr;
}

const JumpSite* find_site(const GuestThread& t, uint32_t ret, uint32_t sp) {
    const JumpSite* best = nullptr;
    for (const JumpSite* s = t.sites; s; s = s->next)
        if (s->ret == ret && s->sp == sp && (!best || s->armed > best->armed)) best = s;
    return best;
}

// The lowest guest stack pointer a thread's live frames hold.
uint32_t stack_low(const GuestThread& t) {
    uint32_t low = UINT32_MAX;
    for (const JumpSite* s = t.sites; s; s = s->next) low = std::min(low, s->sp);
    return low;
}

void reap() {
    std::erase_if(t_threads, [](const std::unique_ptr<GuestThread>& t) { return t->state == GuestThread::Done; });
}

// Runs `to` until something switches back to the running thread; then the
// thread that did so, if it ended with an error, passes it on.
void switch_to(GuestThread& to) {
    GuestThread& me = running();
    if (!me.fiber && !me.host) me.host = &Fiber::current();  // the first switch from the host stack
    me.state = GuestThread::Suspended;
    to.state = GuestThread::Running;
    to.resumer = &me;
    t_running = &to;
    fiber_of(to).switch_to();
    // Back on this thread: whoever switched here set t_running to it.
    GuestThread* from = me.resumer;
    if (from && from->state == GuestThread::Done) {
        std::exception_ptr e = std::exchange(from->error, nullptr);
        reap();
        if (e) std::rethrow_exception(e);
    }
}

void thread_main(void* arg) {
    GuestThread& t = *static_cast<GuestThread*>(arg);
    try {
        t_resume = t.resume;
        t.fn(*t.c);
        // Its first function returned: there is no frame to return to.
        throw Stop{Stop::BadReturn, t.c->ret, t.c->ret};
    } catch (const ThreadExit&) {
    } catch (...) {
        t.error = std::current_exception();
    }
    t.state = GuestThread::Done;
    t.sites = nullptr;
    GuestThread& to = *t.resumer;
    to.state = GuestThread::Running;
    to.resumer = &t;
    t_running = &to;
    fiber_of(to).switch_to();
    std::abort();  // a thread that ended is never switched to
}

// A new guest thread running `fn` (entered at `resume`, a setjmp site, or at
// its start) on the guest stack at `sp`. A suspended thread whose live frames
// span `sp` has had its guest stack taken over: it ends first. Returns when
// something switches back to the running thread.
void start_thread(Context& c, Fn fn, uint32_t resume, uint32_t sp) {
    for (size_t i = 0; i < t_threads.size(); ++i) {
        GuestThread& old = *t_threads[i];
        if (old.state == GuestThread::Suspended && old.entry_sp && stack_low(old) <= sp && sp <= old.entry_sp) {
            old.kill = true;
            switch_to(old);  // it unwinds and comes back ended (reaped: indices may shift)
            i = static_cast<size_t>(-1);
        }
    }
    auto t = std::make_unique<GuestThread>();
    t->entry_sp = sp;
    t->c = &c;
    t->fn = fn;
    t->resume = resume;
    t->fiber = std::make_unique<Fiber>(thread_main, t.get());
    if (!t->fiber->ok()) throw Stop{Stop::Unsupported, resume, sp};  // no host stack for it
    GuestThread& to = *t;
    t_threads.push_back(std::move(t));
    switch_to(to);
}
}  // namespace

void JumpSite::arm(uint32_t sp_) {
    sp = sp_;
    armed = ++t_arms;
    if (thread) return;  // armed again (a loop): already registered
    GuestThread& t = running();
    next = t.sites;
    t.sites = this;
    thread = &t;
}

JumpSite::~JumpSite() {
    if (!thread) return;
    // Frames end innermost first, so this is nearly always the head.
    for (JumpSite** p = &thread->sites; *p; p = &(*p)->next)
        if (*p == this) {
            *p = next;
            return;
        }
}

uint32_t take_resume() { return std::exchange(t_resume, 0); }

void replaced(Context& c, uint32_t fn) {
    const Provided* p = native_at(fn);
    if (!p) throw Stop{Stop::Unsupported, fn, fn};  // replaced in the port, and this build has no native for it
    p->fn(c);
    c.ret = c.pr;
}

void end_guest_threads() {
    if (t_running && t_running != &t_first) throw Stop{Stop::Unsupported, 0, 0};  // not from a guest thread
    while (!t_threads.empty()) {
        GuestThread& t = *t_threads.back();
        if (t.state != GuestThread::Suspended) {
            t_threads.pop_back();  // ended, or never started: no frames
            continue;
        }
        t.kill = true;
        switch_to(t);  // it unwinds and comes back ended (reaped)
    }
    t_first = GuestThread{};
    t_running = nullptr;
    t_resume = 0;
}

void nonlocal_return(Context& c, uint32_t from) {
    for (;;) {
        GuestThread& me = running();
        if (me.kill) throw ThreadExit{};
        // A longjmp restores the stack pointer setjmp saw and returns to its caller.
        if (const JumpSite* s = find_site(me, c.ret, c.r[15])) throw Unwind{s};
        const JumpSiteEntry* site = jump_site(c.ret);
        const FnEntry* fn = nullptr;
        if (!site) {
            const FnEntry* end = g_functions + g_function_count;
            const FnEntry* it =
                std::lower_bound(g_functions, end, c.ret, [](const FnEntry& e, uint32_t x) { return e.addr < x; });
            if (it != end && it->addr == c.ret) fn = it;
        }
        if (!g_guest_threads) throw Stop{site || fn ? Stop::Unsupported : Stop::BadReturn, from, c.ret};
        // Another thread's site: the latest armed of those alike.
        GuestThread* to = nullptr;
        const JumpSite* best = nullptr;
        for (const auto& t : t_threads)
            if (t->state == GuestThread::Suspended)
                if (const JumpSite* s = find_site(*t, c.ret, c.r[15]); s && (!best || s->armed > best->armed)) best = s, to = t.get();
        if (&me != &t_first && t_first.state == GuestThread::Suspended)
            if (const JumpSite* s = find_site(t_first, c.ret, c.r[15]); s && (!best || s->armed > best->armed)) to = &t_first;
        if (to) switch_to(*to);
        else if (site) start_thread(c, site->fn, c.ret, c.r[15]);
        else if (fn) start_thread(c, fn->fn, 0, c.r[15]);
        else throw Stop{Stop::BadReturn, from, c.ret};
        // Resumed: by a longjmp to a site of this thread, or to end it.
    }
}

// The code after an indirect call goes on in the program's mode (PR, SZ and
// FR clear): a callee that may return in another checks it.
void call(Context& c, uint32_t target, uint32_t from) {
    const FnEntry* e = find(target);
    if (!e) return call_native(c, target, from);
    e->fn(c);
    if (!e->keeps_mode) expect_fp_mode(c.fpscr, 0, from);
}

void call_any_mode(Context& c, uint32_t target, uint32_t from) {
    const FnEntry* e = find(target);
    if (!e) {
        expect_fp_mode(c.fpscr, 0, from);  // native code runs in the program's mode
        return call_native(c, target, from);
    }
    if (!e->any_mode) expect_fp_mode(c.fpscr, 0, from);
    e->fn(c);
}

}  // namespace qilin::rt
