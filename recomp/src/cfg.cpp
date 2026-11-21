#include "cfg.hpp"

#include <algorithm>
#include <array>
#include <format>
#include <unordered_map>
#include <unordered_set>

namespace qilin {

namespace {

using sh4::Insn;
using sh4::Op;

// The general registers known to hold a constant at some point.
struct State {
    std::array<uint32_t, 16> value{};
    uint16_t known = 0;

    bool has(unsigned r) const { return known >> r & 1; }
    void set(unsigned r, uint32_t v) {
        value[r] = v;
        known = static_cast<uint16_t>(known | 1u << r);
    }
    void forget(uint16_t mask) { known = static_cast<uint16_t>(known & ~mask); }
    bool operator==(const State& o) const {
        if (known != o.known) return false;
        for (unsigned r = 0; r < 16; ++r)
            if (has(r) && value[r] != o.value[r]) return false;
        return true;
    }
};

// Where two paths join, a register is known only if both agree on it.
State meet(const State& a, const State& b) {
    State m = a;
    m.known = static_cast<uint16_t>(a.known & b.known);
    for (unsigned r = 0; r < 16; ++r)
        if (m.has(r) && a.value[r] != b.value[r]) m.forget(static_cast<uint16_t>(1u << r));
    return m;
}

constexpr uint32_t kMaxPadding = 32;  // hand-written library routines are aligned to 32 bytes with nops

bool is_padding_word(uint16_t w) { return w == 0x0000 || w == 0x0009; }

}  // namespace

const char* origin_name(Origin o) {
    switch (o) {
    case Origin::Entry: return "entry";
    case Origin::Call: return "call";
    case Origin::TailCall: return "tail call";
    case Origin::Literal: return "literal pointer";
    case Origin::DataPointer: return "pointer in data";
    case Origin::Gap: return "gap";
    }
    return "?";
}

std::string percent(uint64_t part, uint64_t whole, int decimals) {
    if (whole == 0) return "n/a";
    uint64_t scale = 1;
    for (int i = 0; i < decimals; ++i) scale *= 10;
    const uint64_t scaled = (part * 100 * scale + whole / 2) / whole;
    if (decimals == 0) return std::format("{}%", scaled);
    return std::format("{}.{:0{}}%", scaled / scale, scaled % scale, decimals);
}

Cfg::Cfg(const BootFile& boot) : boot_(boot) {
    const std::span<const uint8_t> image = boot.image();
    decoded_.reserve(image.size() / 2);
    for (size_t i = 0; i + 2 <= image.size(); i += 2) decoded_.push_back(sh4::decode(read_le16(image.data() + i)));
}

Insn Cfg::insn(uint32_t a) const {
    if (!in_image(a)) return {};
    return decoded_[(a - boot_.base()) / 2];
}

uint32_t Cfg::bound_of(uint32_t start) const {
    const auto next = starts_.upper_bound(start);
    return next == starts_.end() ? boot_.end() : next->first;
}

bool Cfg::add_start(uint32_t a, Origin o) {
    if (!in_image(a)) return false;
    const auto [it, inserted] = starts_.try_emplace(a, o);
    if (!inserted && o < it->second) it->second = o;  // stronger evidence for a known start
    return inserted;
}

// The Hitachi compiler's switch: see the header.
bool Cfg::resolve_table(uint32_t at, JumpTable& out) const {
    if (at < boot_.base() + 20) return false;
    const Insn load = insn(at - 2), mova = insn(at - 4), copy = insn(at - 6), shift = insn(at - 8);
    if (insn(at).op != Op::braf || insn(at).n() != 0) return false;
    if (load.op != Op::mov_w_load_r0 || load.n() != 0) return false;
    const unsigned index = load.m();
    if (index == 0) return false;
    if (mova.op != Op::mova) return false;
    if (copy.op != Op::mov || copy.m() != 0 || copy.n() != index) return false;
    if (shift.op != Op::shll || shift.n() != 0) return false;
    // The bound check before it, in one of two shapes:
    //   cmp/hs rB,r0 ; bf L ; bra default ; <slot> ; L:     (default far away)
    //   cmp/hs rB,r0 ; bt default ; L:                      (default near)
    //   cmp/hs rB,r0 ; bf L ; mov.l D,r0 ; jmp @r0 ; <slot> ; L:   (default out of bra's reach)
    uint32_t cmp_at = 0;
    const auto bf_over = [&](uint32_t a) { return insn(a).op == Op::bf && insn(a).branch_target(a) == at - 8; };
    if (insn(at - 10).op == Op::bt) {
        cmp_at = at - 12;
    } else if (insn(at - 12).op == Op::bra && bf_over(at - 14)) {
        cmp_at = at - 16;
    } else if (insn(at - 12).op == Op::jmp && insn(at - 14).op == Op::mov_l_pc && bf_over(at - 16)) {
        cmp_at = at - 18;
    } else {
        return false;
    }
    const Insn cmp = insn(cmp_at), limit = insn(cmp_at - 2);
    if (cmp.op != Op::cmp_hs || cmp.n() != 0 || cmp.m() == 0) return false;
    const unsigned bound_reg = cmp.m();
    int64_t count = 0;
    if (limit.op == Op::mov_imm && limit.n() == bound_reg) {
        count = limit.simm8();
    } else if (limit.op == Op::mov_w_pc && limit.n() == bound_reg) {
        const uint32_t lit = limit.pc_address(cmp_at - 2);
        if (!boot_.mapped(lit, 2)) return false;
        count = static_cast<int16_t>(boot_.u16(lit));
    } else {
        return false;
    }
    if (count <= 0 || count > 4096) return false;
    out.at = at;
    out.table = mova.pc_address(at - 4);
    out.targets.clear();
    for (int64_t i = 0; i < count; ++i) {
        const uint32_t entry = out.table + 2 * static_cast<uint32_t>(i);
        if (!boot_.mapped(entry, 2)) return false;
        const uint32_t target = at + 4 + static_cast<uint32_t>(static_cast<int16_t>(boot_.u16(entry)));
        if (!in_image(target)) return false;
        out.targets.push_back(target);
    }
    return true;
}

Function Cfg::explore(uint32_t start, uint32_t bound) const {
    Function f;
    f.start = start;
    f.bound = bound;

    struct Edge {
        uint32_t pc;
        State state;
    };

    // One instruction (with its delay slot, if it has one): where control
    // goes next and with which constants. With `record`, also what it tells
    // us about the function.
    const auto step = [&](uint32_t pc, const State& st, std::vector<Edge>& out, bool record) {
        const Insn in = insn(pc);
        if (!in.valid()) {
            if (record) f.bad.push_back(pc);
            return;
        }
        const uint32_t flags = sh4::spec(in.op).flags;
        // Straight on to `next`, unless that leaves the function.
        const auto fall = [&](uint32_t next, const State& s) {
            if (next < bound) out.push_back({next, s});
            else if (bound < boot_.end()) {
                if (record) f.falls_into = bound;
            } else if (record) f.bad.push_back(pc);  // ran off the end of the image
        };
        // A branch to `t`: inside the function, or a tail call.
        const auto edge = [&](uint32_t t, const State& s) {
            if (!in_image(t)) {
                if (record) f.bad.push_back(pc);
            } else if (t >= start && t < bound) {
                out.push_back({t, s});
                if (record) f.leaders.insert(t);
            } else if (record) {
                f.tails[pc] = t;
            }
        };

        if (!(flags & sh4::kBranch)) {
            State s = st;
            s.forget(sh4::gpr_writes(in));
            switch (in.op) {
            case Op::mov_l_pc: {
                const uint32_t lit = in.pc_address(pc);
                if (record) f.literals[lit] = 4;
                if (boot_.mapped(lit, 4)) s.set(in.n(), boot_.u32(lit));
                else if (record) f.bad.push_back(pc);
                break;
            }
            case Op::mov_w_pc: {
                const uint32_t lit = in.pc_address(pc);
                if (record) f.literals[lit] = 2;
                if (boot_.mapped(lit, 2)) s.set(in.n(), static_cast<uint32_t>(static_cast<int16_t>(boot_.u16(lit))));
                else if (record) f.bad.push_back(pc);
                break;
            }
            case Op::mova:
                if (record) f.movas.insert(in.pc_address(pc));
                s.set(0, in.pc_address(pc));
                break;
            case Op::mov_imm: s.set(in.n(), static_cast<uint32_t>(in.simm8())); break;
            case Op::mov:
                if (st.has(in.m())) s.set(in.n(), st.value[in.m()]);
                break;
            case Op::add_imm:
                if (st.has(in.n())) s.set(in.n(), st.value[in.n()] + static_cast<uint32_t>(in.simm8()));
                break;
            default: break;
            }
            fall(pc + 2, s);
            return;
        }

        // A branch. The slot's instruction runs before the branch takes
        // effect, but after the branch has read its own operands.
        State after = st;
        uint32_t next = pc + 2;
        if (flags & sh4::kDelayed) {
            const uint32_t slot = pc + 2;
            const Insn s = insn(slot);
            if (slot >= bound || !s.valid() || (sh4::spec(s.op).flags & sh4::kSlotIllegal)) {
                if (record) f.bad_slots.push_back(pc);
                return;
            }
            if (record) f.slots.insert(slot);
            after.forget(sh4::gpr_writes(s));
            if (s.op == Op::mov_imm) after.set(s.n(), static_cast<uint32_t>(s.simm8()));
            next = pc + 4;
        }
        const State nothing{};  // after a call: the callee may have changed any register
        const auto constant = [&](unsigned r) { return st.has(r); };
        switch (in.op) {
        case Op::bt: case Op::bf: case Op::bt_s: case Op::bf_s:
            edge(in.branch_target(pc), after);
            if (record && next < bound) f.leaders.insert(next);
            fall(next, after);
            break;
        case Op::bra: {
            // Once PR has been restored the function is leaving: a forward
            // bra then is a tail call to a function placed after this one,
            // even when nothing else has shown that function to exist yet.
            const auto restores_pr = [&](uint32_t a) { return insn(a).op == Op::lds_l_pr && insn(a).n() == 15; };
            const uint32_t target = in.branch_target(pc);
            if (target > pc && in_image(target) && (restores_pr(pc + 2) || restores_pr(pc - 2))) {
                if (record) f.tails[pc] = target;
            } else {
                edge(target, after);
            }
            break;
        }
        case Op::bsr:
            if (!in_image(in.branch_target(pc))) {
                if (record) f.bad.push_back(pc);
                break;
            }
            if (record) f.calls[pc] = in.branch_target(pc);
            fall(next, nothing);
            break;
        case Op::jsr: case Op::bsrf: {
            const uint32_t target = in.op == Op::jsr ? st.value[in.n()] : pc + 4 + st.value[in.n()];
            if (record) {
                if (constant(in.n()) && in_image(target)) f.calls[pc] = target;
                else f.icalls.push_back(pc);
            }
            fall(next, nothing);
            break;
        }
        case Op::jmp: case Op::braf: {
            const uint32_t target = in.op == Op::jmp ? st.value[in.n()] : pc + 4 + st.value[in.n()];
            JumpTable table;
            if (constant(in.n()) && in_image(target)) {
                // A compiler branches inside a function with bra, bt and bf;
                // a jump through a constant is a tail call, unless it goes
                // back into code this function has already been through.
                if (target >= start && target < pc) edge(target, after);
                else if (record) f.tails[pc] = target;
            } else if (in.op == Op::braf && !constant(in.n()) && resolve_table(pc, table)) {
                State s = after;
                s.forget(static_cast<uint16_t>(1u | 1u << insn(pc - 2).m()));
                for (const uint32_t t : table.targets) edge(t, s);
                if (record) f.jtabs.push_back(std::move(table));
            } else if (record) {
                f.itails.push_back(pc);
            }
            break;
        }
        case Op::rts: case Op::rte:
            if (record) f.returns = true;
            break;
        default:
            if (record) f.bad.push_back(pc);  // a branch this pass doesn't know: a decoder table change
            break;
        }
    };

    // Constants to a fixed point. States only ever lose registers, so this ends.
    std::unordered_map<uint32_t, State> in;
    std::vector<Edge> work{{start, State{}}}, out;
    while (!work.empty()) {
        const Edge e = work.back();
        work.pop_back();
        const auto it = in.find(e.pc);
        if (it == in.end()) {
            in.emplace(e.pc, e.state);
        } else {
            const State m = meet(it->second, e.state);
            if (m == it->second) continue;
            it->second = m;
        }
        out.clear();
        step(e.pc, in.at(e.pc), out, false);
        work.insert(work.end(), out.begin(), out.end());
    }

    // What is reachable with the final constants, and what it says.
    std::unordered_set<uint32_t> seen;
    std::vector<uint32_t> stack{start};
    f.leaders.insert(start);
    while (!stack.empty()) {
        const uint32_t pc = stack.back();
        stack.pop_back();
        if (!seen.insert(pc).second) continue;
        out.clear();
        step(pc, in.at(pc), out, true);
        for (const Edge& e : out) stack.push_back(e.pc);
    }
    for (const uint32_t pc : seen)
        if (insn(pc).valid()) f.insns.push_back(pc);
    for (const uint32_t slot : f.slots) {
        if (seen.contains(slot)) f.slot_targets.push_back(slot);
        else f.insns.push_back(slot);
    }
    std::ranges::sort(f.insns);
    for (const JumpTable& t : f.jtabs) f.movas.erase(t.table);
    for (auto* v : {&f.bad, &f.bad_slots, &f.slot_targets, &f.icalls, &f.itails}) {
        std::ranges::sort(*v);
        v->erase(std::unique(v->begin(), v->end()), v->end());
    }
    std::ranges::sort(f.jtabs, {}, &JumpTable::at);
    f.end = f.insns.empty() ? start : f.insns.back() + 2;
    return f;
}

bool Cfg::explore_all() {
    bool any = false;
    for (bool changed = true; changed;) {
        changed = false;
        const std::vector<std::pair<uint32_t, Origin>> snapshot(starts_.begin(), starts_.end());
        for (const auto& [start, origin] : snapshot) {
            const uint32_t bound = bound_of(start);
            const auto known = funcs_.find(start);
            if (known != funcs_.end() && known->second.bound == bound) {
                known->second.origin = starts_.at(start);
                continue;
            }
            Function f = explore(start, bound);
            f.origin = starts_.at(start);
            for (const auto& [site, target] : f.calls) changed |= add_start(target, Origin::Call);
            for (const auto& [site, target] : f.tails) changed |= add_start(target, Origin::TailCall);
            funcs_[start] = std::move(f);
        }
        any |= changed;
    }
    return any;
}

void Cfg::claim(const Function& f, std::vector<Kind>& kinds) const {
    const uint32_t base = boot_.base();
    const auto mark = [&](uint32_t a, uint32_t n, Kind k, bool overwrite) {
        for (uint32_t i = 0; i < n; ++i)
            if (boot_.mapped(a + i) && (overwrite || kinds[a + i - base] == Kind::Unknown)) kinds[a + i - base] = k;
    };
    for (const uint32_t pc : f.insns) mark(pc, 2, Kind::Code, true);
    for (const auto& [a, size] : f.literals) mark(a, size, Kind::Literal, false);
    for (const uint32_t a : f.movas) mark(a, 4, Kind::Literal, false);
    for (const JumpTable& t : f.jtabs) mark(t.table, t.end() - t.table, Kind::Table, false);
}

std::vector<Cfg::Kind> Cfg::claims() const {
    std::vector<Kind> kinds(boot_.image().size(), Kind::Unknown);
    for (const auto& [start, f] : funcs_) claim(f, kinds);
    return kinds;
}

// Is a function explored on trial good enough to keep on weak evidence?
bool Cfg::acceptable(const Function& f, const std::vector<Kind>& kinds) const {
    if (!f.clean() || f.insns.empty() || f.falls_into) return false;
    const uint32_t base = boot_.base();
    const auto free_or = [&](uint32_t a, uint32_t n, Kind also) {
        for (uint32_t i = 0; i < n; ++i) {
            if (!boot_.mapped(a + i)) return false;
            const Kind k = kinds[a + i - base];
            if (k != Kind::Unknown && k != also) return false;
        }
        return true;
    };
    for (const uint32_t pc : f.insns)
        if (!free_or(pc, 2, Kind::Unknown)) return false;
    for (const auto& [a, size] : f.literals)
        if (!free_or(a, size, Kind::Literal)) return false;
    for (const JumpTable& t : f.jtabs)
        if (!free_or(t.table, t.end() - t.table, Kind::Unknown)) return false;
    // Its own literals mustn't be its own code either.
    for (const auto& [a, size] : f.literals)
        for (uint32_t i = 0; i < size; i += 2)
            if (std::ranges::binary_search(f.insns, (a + i) & ~1u)) return false;
    // What it calls or jumps to must at least be free to be code.
    const auto could_be_code = [&](uint32_t t) {
        const Kind k = kinds[t - base];
        return k == Kind::Code ? starts_.contains(t) : k == Kind::Unknown;
    };
    for (const auto& [site, target] : f.calls)
        if (!could_be_code(target) && !std::ranges::binary_search(f.insns, target)) return false;
    for (const auto& [site, target] : f.tails)
        if (!could_be_code(target)) return false;
    return true;
}

// A weak candidate vouches for what it calls and jumps to: accepting it makes
// those function starts with strong evidence. So each target that isn't a
// known function must be clean on trial too (one level down; theirs are
// checked when they are explored for real, and show up as suspects if not).
bool Cfg::targets_acceptable(const Function& f, const std::vector<Kind>& kinds) const {
    std::vector<Kind> with = kinds;
    claim(f, with);
    const auto ok = [&](uint32_t target) {
        if (starts_.contains(target) || (target >= f.start && target < f.end)) return true;
        return acceptable(explore(target, bound_of(target)), with);
    };
    for (const auto& [site, target] : f.calls)
        if (!ok(target)) return false;
    for (const auto& [site, target] : f.tails)
        if (!ok(target)) return false;
    return true;
}

// Longwords that reached code loads from its pools and that point into the image.
bool Cfg::seed_literals() {
    std::vector<Kind> kinds = claims();
    std::vector<uint32_t> candidates;
    for (const auto& [start, f] : funcs_)
        for (const auto& [a, size] : f.literals)
            if (size == 4 && boot_.mapped(a, 4)) candidates.push_back(boot_.u32(a));
    std::ranges::sort(candidates);
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    bool changed = false;
    for (const uint32_t c : candidates) {
        if (!in_image(c) || starts_.contains(c) || kinds[c - boot_.base()] != Kind::Unknown) continue;
        if (!tried_.insert(c).second) continue;
        const Function trial = explore(c, bound_of(c));
        if (!acceptable(trial, kinds) || !targets_acceptable(trial, kinds)) {
            rejected_.push_back(c);
            continue;
        }
        add_start(c, Origin::Literal);
        claim(trial, kinds);
        changed = true;
    }
    return changed;
}

// The first unclaimed word after each function and its pool.
bool Cfg::seed_gaps() {
    std::vector<Kind> kinds = claims();
    const uint32_t base = boot_.base();
    std::vector<uint32_t> ends;
    for (const auto& [start, f] : funcs_) ends.push_back(f.end);
    bool changed = false;
    for (uint32_t a : ends) {
        uint32_t padding = 0;
        for (; boot_.mapped(a, 2); a += 2) {
            const Kind k0 = kinds[a - base], k1 = kinds[a + 1 - base];
            if (k0 == Kind::Code || k1 == Kind::Code) break;
            if (k0 != Kind::Unknown || k1 != Kind::Unknown) continue;  // this function's pool or table
            if (!is_padding_word(boot_.u16(a)) || padding >= kMaxPadding) break;
            padding += 2;
        }
        if (!in_image(a) || kinds[a - base] != Kind::Unknown || starts_.contains(a)) continue;
        if (!tried_.insert(a).second) continue;
        const Function trial = explore(a, bound_of(a));
        if (!acceptable(trial, kinds) || !targets_acceptable(trial, kinds)) {
            rejected_.push_back(a);
            continue;
        }
        add_start(a, Origin::Gap);
        claim(trial, kinds);
        changed = true;
    }
    return changed;
}

// Longwords in unclaimed bytes that point at the first unclaimed word after
// claimed ones: tables of function pointers. The boundary matters: a pointer
// into the middle of unclaimed bytes could as well be a pointer to data.
bool Cfg::seed_data_pointers() {
    std::vector<Kind> kinds = claims();
    const uint32_t base = boot_.base(), end = boot_.end();
    // The first word of each unclaimed run that isn't padding.
    std::set<uint32_t> boundaries;
    for (uint32_t a = base; a < end;) {
        if (kinds[a - base] != Kind::Unknown) {
            ++a;
            continue;
        }
        uint32_t e = a;
        while (e < end && kinds[e - base] == Kind::Unknown) ++e;
        uint32_t first = (a + 1) & ~1u, padding = 0;
        while (first + 2 <= e && is_padding_word(boot_.u16(first)) && padding < kMaxPadding) first += 2, padding += 2;
        if (first + 2 <= e && a > base) boundaries.insert(first);
        a = e;
    }
    std::vector<uint32_t> candidates;
    for (uint32_t a = base; a + 4 <= end; a += 4) {
        bool unclaimed = true;
        for (uint32_t i = 0; i < 4; ++i) unclaimed &= kinds[a + i - base] == Kind::Unknown;
        if (!unclaimed) continue;
        const uint32_t v = boot_.u32(a);
        if (boundaries.contains(v)) candidates.push_back(v);
    }
    std::ranges::sort(candidates);
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
    bool changed = false;
    for (const uint32_t c : candidates) {
        if (starts_.contains(c) || kinds[c - base] != Kind::Unknown) continue;
        if (!tried_.insert(c).second) continue;
        const Function trial = explore(c, bound_of(c));
        if (!acceptable(trial, kinds) || !targets_acceptable(trial, kinds)) {
            rejected_.push_back(c);
            continue;
        }
        add_start(c, Origin::DataPointer);
        claim(trial, kinds);
        changed = true;
    }
    return changed;
}

std::string Cfg::trial(uint32_t a) const {
    if (!in_image(a)) return "not an address of a word in the image\n";
    const std::vector<Kind> kinds = claims();
    const Function f = explore(a, bound_of(a));
    std::string s = std::format("{:08x}-{:08x} (bound {:08x}): {} instructions, {} literals, {} calls, {} tail calls, "
                                "{} indirect calls, {} indirect jumps, {} jump tables, returns: {}\n",
                                f.start, f.end, f.bound, f.insns.size(), f.literals.size(), f.calls.size(),
                                f.tails.size(), f.icalls.size(), f.itails.size(), f.jtabs.size(), f.returns);
    static constexpr const char* kKinds[] = {"unclaimed", "code", "a literal", "a jump table"};
    const auto kind = [&](uint32_t x) { return kKinds[static_cast<size_t>(kinds[x - boot_.base()])]; };
    if (starts_.contains(a)) s += "already a function\n";
    for (const uint32_t x : f.bad) s += std::format("reaches {:08x}: not an instruction, or outside the image\n", x);
    for (const uint32_t x : f.bad_slots) s += std::format("delayed branch at {:08x} has no legal delay slot\n", x);
    for (const uint32_t x : f.slot_targets) s += std::format("branches to the delay slot at {:08x}\n", x);
    if (f.falls_into) s += std::format("runs into the function at {:08x}\n", f.falls_into);
    for (const uint32_t pc : f.insns)
        if (kinds[pc - boot_.base()] != Kind::Unknown && !starts_.contains(a)) {
            s += std::format("its instruction at {:08x} is already {}\n", pc, kind(pc));
            break;
        }
    for (const auto& [x, size] : f.literals)
        if (boot_.mapped(x) && kinds[x - boot_.base()] == Kind::Code) s += std::format("its literal at {:08x} is code\n", x);
    for (const auto& [site, t] : f.calls)
        if (!starts_.contains(t)) s += std::format("calls {:08x} (from {:08x}), which is {} and not a function\n", t, site, kind(t));
    for (const auto& [site, t] : f.tails)
        if (!starts_.contains(t)) s += std::format("jumps to {:08x} (from {:08x}), which is {} and not a function\n", t, site, kind(t));
    if (!starts_.contains(a))
        s += acceptable(f, kinds) && targets_acceptable(f, kinds) ? "would be accepted\n" : "would be rejected\n";
    return s;
}

void Cfg::find_suspects() {
    suspects_.clear();
    std::vector<Kind> code(boot_.image().size(), Kind::Unknown);
    for (const auto& [start, f] : funcs_)
        for (const uint32_t pc : f.insns) code[pc - boot_.base()] = code[pc + 1 - boot_.base()] = Kind::Code;
    for (const auto& [start, f] : funcs_) {
        const auto add = [&](std::string why) { suspects_.push_back({start, std::move(why)}); };
        for (const uint32_t a : f.bad) add(std::format("reaches {:08x}, which isn't an instruction or leaves the image", a));
        for (const uint32_t a : f.bad_slots) add(std::format("delayed branch at {:08x} has no legal delay slot", a));
        for (const uint32_t a : f.slot_targets) add(std::format("something branches to the delay slot at {:08x}", a));
        for (const uint32_t a : f.itails)
            if (insn(a).op == Op::braf) add(std::format("braf at {:08x} isn't a recognised jump table", a));
        for (const auto& [a, size] : f.literals)
            for (uint32_t i = 0; i < size; ++i)
                if (boot_.mapped(a + i) && code[a + i - boot_.base()] == Kind::Code) {
                    add(std::format("literal at {:08x} is also reached as code", a));
                    break;
                }
        if (f.falls_into && strong(f.origin)) add(std::format("runs into the function at {:08x}", f.falls_into));
    }
    std::ranges::sort(rejected_);
}

void Cfg::run() {
    starts_.clear();
    funcs_.clear();
    tried_.clear();
    rejected_.clear();
    add_start(boot_.entry(), Origin::Entry);
    for (;;) {
        explore_all();
        if (seed_literals()) continue;
        if (seed_data_pointers()) continue;
        if (!seed_gaps()) break;
    }
    // A start that later evidence put inside this function's old range moved
    // its bound; explore_all has redone those. Starts with no function left
    // can't happen: every start is explored.
    find_suspects();
}

Coverage Cfg::coverage() const {
    Coverage c;
    if (funcs_.empty()) return c;
    const std::vector<Kind> kinds = claims();
    const uint32_t base = boot_.base();
    c.lo = funcs_.begin()->first;
    size_t last = 0;
    for (size_t i = 0; i < kinds.size(); ++i)
        if (kinds[i] != Kind::Unknown) last = i + 1;
    c.hi = base + static_cast<uint32_t>((last + 1) & ~size_t(1));
    c.hi = std::min(c.hi, boot_.end());
    c.before = c.lo - base;
    c.after = boot_.end() - c.hi;
    // Addresses in the image that reached code holds in its literal pools.
    std::vector<uint32_t> references;
    for (const auto& [start, f] : funcs_)
        for (const auto& [a, size] : f.literals)
            if (size == 4 && boot_.mapped(a, 4) && boot_.mapped(boot_.u32(a))) references.push_back(boot_.u32(a));
    std::ranges::sort(references);
    references.erase(std::unique(references.begin(), references.end()), references.end());
    for (uint32_t a = c.lo; a < c.hi;) {
        const Kind k = kinds[a - base];
        if (k == Kind::Code) ++c.code, ++a;
        else if (k == Kind::Literal) ++c.literal, ++a;
        else if (k == Kind::Table) ++c.table, ++a;
        else {
            uint32_t e = a;
            while (e < c.hi && kinds[e - base] == Kind::Unknown) ++e;
            // An unclaimed run is padding if it is short and all zero words or nops.
            bool padding = e - a <= kMaxPadding;
            for (uint32_t w = a & ~1u; padding && w < e; w += 2)
                if (!boot_.mapped(w, 2) || !is_padding_word(boot_.u16(w))) padding = false;
            const auto referenced = std::ranges::upper_bound(references, e - 1) - std::ranges::lower_bound(references, a);
            // Inside a function's extent: bytes its code jumps over and never reaches.
            const auto owner = funcs_.upper_bound(a);
            const bool inside = owner != funcs_.begin() && e <= std::prev(owner)->second.end;
            if (padding) c.padding += e - a;
            else if (referenced == 0 && inside) c.dead += e - a;
            else if (referenced > 0) {
                c.data += e - a;
                ++c.data_runs;
                c.data_references += static_cast<uint64_t>(referenced);
            } else {
                c.unknown += e - a;
                c.unknown_spans.emplace_back(a, e);
            }
            a = e;
        }
    }
    return c;
}

std::string Cfg::summary() const {
    std::array<size_t, 6> by_origin{};
    size_t insns = 0, slots = 0, literals = 0, calls = 0, icalls = 0, tails = 0, itails = 0, tables = 0, entries = 0;
    for (const auto& [start, f] : funcs_) {
        ++by_origin[static_cast<size_t>(f.origin)];
        insns += f.insns.size();
        slots += f.slots.size();
        literals += f.literals.size();
        calls += f.calls.size();
        icalls += f.icalls.size();
        tails += f.tails.size();
        itails += f.itails.size();
        tables += f.jtabs.size();
        for (const JumpTable& t : f.jtabs) entries += t.targets.size();
    }
    std::string s = std::format("{} functions:", funcs_.size());
    for (size_t i = 0; i < by_origin.size(); ++i)
        s += std::format(" {} by {}{}", by_origin[i], origin_name(static_cast<Origin>(i)), i + 1 < by_origin.size() ? "," : "\n");
    s += std::format("{} instructions ({} in delay slots), {} literals\n", insns, slots, literals);
    s += std::format("calls: {} direct, {} indirect; tail calls: {} direct, {} indirect or unresolved\n", calls, icalls,
                     tails, itails);
    s += std::format("{} jump tables, {} entries\n", tables, entries);
    s += std::format("{} suspects; {} weak candidates rejected\n", suspects_.size(), rejected_.size());
    const Coverage c = coverage();
    const uint64_t extent = c.hi - c.lo;
    s += std::format("code extent {:08x}-{:08x} ({} bytes): code {} ({}), literal {}, table {}, padding {}, "
                     "data {} in {} runs ({} references), dead {}, unknown {} ({})\n",
                     c.lo, c.hi, extent, c.code, percent(c.code, extent, 2), c.literal, c.table, c.padding, c.data,
                     c.data_runs, c.data_references, c.dead, c.unknown, percent(c.unknown, extent, 2));
    s += std::format("outside it: {} bytes before, {} after (not classified)\n", c.before, c.after);
    return s;
}

std::string Cfg::to_json() const {
    const auto list = [](const auto& v) {
        std::string s = "[";
        bool first = true;
        for (const uint32_t a : v) {
            s += std::format("{}\"{:08x}\"", first ? "" : ", ", a);
            first = false;
        }
        return s + "]";
    };
    const auto pairs = [](const std::map<uint32_t, uint32_t>& m) {
        std::string s = "[";
        bool first = true;
        for (const auto& [a, b] : m) {
            s += std::format("{}[\"{:08x}\", \"{:08x}\"]", first ? "" : ", ", a, b);
            first = false;
        }
        return s + "]";
    };
    std::string j = "{\n  \"functions\": [\n";
    size_t i = 0;
    for (const auto& [start, f] : funcs_) {
        j += std::format("    {{\"start\": \"{:08x}\", \"end\": \"{:08x}\", \"origin\": \"{}\", \"instructions\": {}, "
                         "\"returns\": {}, \"calls\": {}, \"tails\": {}, \"icalls\": {}, \"itails\": {}, \"tables\": [",
                         f.start, f.end, origin_name(f.origin), f.insns.size(), f.returns, pairs(f.calls),
                         pairs(f.tails), list(f.icalls), list(f.itails));
        for (size_t t = 0; t < f.jtabs.size(); ++t)
            j += std::format("{}{{\"at\": \"{:08x}\", \"table\": \"{:08x}\", \"targets\": {}}}", t ? ", " : "",
                             f.jtabs[t].at, f.jtabs[t].table, list(f.jtabs[t].targets));
        j += std::format("]{}}}{}\n", f.falls_into ? std::format(", \"falls_into\": \"{:08x}\"", f.falls_into) : "",
                         ++i < funcs_.size() ? "," : "");
    }
    j += "  ],\n  \"suspects\": [\n";
    for (size_t k = 0; k < suspects_.size(); ++k)
        j += std::format("    {{\"start\": \"{:08x}\", \"why\": \"{}\"}}{}\n", suspects_[k].start, suspects_[k].why,
                         k + 1 < suspects_.size() ? "," : "");
    j += std::format("  ],\n  \"rejected\": {}\n}}\n", list(rejected_));
    return j;
}

}  // namespace qilin
