// The sound CPU's driver lifted to C++ (docs/sound.md): function discovery
// from the ARM's vectors and its BL targets, literal pools, jump tables
// (`add pc, pc, rN, lsl #2` over a table of branches), and each function
// emitted as C++ over arm_guest.hpp. Developer tool: std::format is fine.
#pragma once

#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "arm/decoder.hpp"

namespace qilin::arm {

// The driver as the ARM sees it: from its address 0 (the file after Sega's
// 32-byte SDRV header, when it has one).
struct Image {
    std::vector<uint8_t> bytes;
    bool mapped(uint32_t a, uint32_t n = 4) const { return uint64_t(a) + n <= bytes.size(); }
    uint32_t u32(uint32_t a) const {
        return uint32_t(bytes[a]) | uint32_t(bytes[a + 1]) << 8 | uint32_t(bytes[a + 2]) << 16 | uint32_t(bytes[a + 3]) << 24;
    }
    static Image of_driver(const std::vector<uint8_t>& file);
};

struct ArmFunction {
    uint32_t start = 0;
    std::set<uint32_t> insns;              // reached instructions
    std::set<uint32_t> labels;             // branch targets inside it
    std::set<uint32_t> loops;              // targets of backward branches (interrupt polls)
    std::map<uint32_t, std::vector<uint32_t>> tables;  // a jump through a table of branches: pc -> the targets
    std::vector<uint32_t> bad;             // reached a word that isn't an instruction this core runs
};

class ArmLifter {
public:
    explicit ArmLifter(Image image) : image_(std::move(image)) {}
    void run();
    const std::map<uint32_t, ArmFunction>& functions() const { return funcs_; }
    const std::set<uint32_t>& literals() const { return literals_; }
    // One function as C++ (namespace qilin::armgen).
    std::string emit(const ArmFunction& f) const;

private:
    ArmFunction explore(uint32_t start);
    bool is_call_jump(uint32_t pc) const;  // a pc write after `mov lr, pc`: a call through a register
    // A jump through a table of branch instructions, as the driver writes them: `add pc, pc,
    // rN, lsl #2`, or `add rP, pc, #k` then `add rT, rP, rN` (or `add pc, rP, rN`) and `mov pc,
    // rT`. The targets (the table's branches' own addresses), or none. `add pc, rP, rI` into
    // straight-line code: the word offsets an `and rI, rX, #mask` just before allows, or, with the
    // index not bounded there, every word to the block's end (docs/sound.md).
    std::vector<uint32_t> branch_table(uint32_t pc) const;
    // A jump through a table of addresses: `ldr rT, [pc, #k]` (the table), `ldr rT, [rT, rN,
    // lsl #2]`, `mov pc, rT`. Its entries, which become functions.
    std::vector<uint32_t> address_table(uint32_t pc) const;
    bool code_at(uint32_t a) const;  // an address that could be an instruction: aligned, in the image, valid
    void seed_data();                // data words that point at code: functions

    Image image_;
    std::set<uint32_t> starts_;
    std::set<uint32_t> literals_;  // words `ldr rN, [pc, #k]` reads
    std::map<uint32_t, ArmFunction> funcs_;
    bool seeded_ = false;
    std::set<uint32_t> rejected_;  // data words pointing at code that doesn't stand up as a function
};

}  // namespace qilin::arm
