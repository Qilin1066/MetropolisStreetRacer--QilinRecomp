// Self-contained tests: SHA-256, the SH-4 decoder, IP.BIN and boot-file
// parsing, function discovery on synthetic programs, and the GD-ROM reader on a synthetic disc image written to a
// temporary folder. No game files required (runs in CI).
#include <algorithm>
#include <bit>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <print>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bootfile.hpp"
#include "cfg.hpp"
#include "decoder.hpp"
#include "gdrom.hpp"
#include "guest.hpp"
#include "lifter.hpp"
#include "report.hpp"
#include "sha256.hpp"

using namespace qilin;

namespace {

int failures = 0;
#define CHECK(cond)                                                                     \
    do {                                                                                \
        if (!(cond)) {                                                                  \
            std::println(stderr, "{}:{}: CHECK failed: {}", __FILE__, __LINE__, #cond); \
            ++failures;                                                                 \
        }                                                                               \
    } while (0)

// ---------------------------------------------------------------- SHA-256

std::vector<uint8_t> pattern(size_t n) {
    std::vector<uint8_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<uint8_t>(i * 7 + 3);
    return v;
}

void test_sha256() {
    CHECK(sha256_hex({}) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    const std::string_view abc = "abc";
    CHECK(sha256_hex({reinterpret_cast<const uint8_t*>(abc.data()), abc.size()}) ==
          "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    // Padding edges: the length fits in the last block, just doesn't, and exact blocks.
    CHECK(sha256_hex(pattern(55)) == "e7313d333c272e639f790978283f9eb392e843d0f29b7016828bb1daa4aac70b");
    CHECK(sha256_hex(pattern(56)) == "4324d65f3c103567f5589c710bc08f8523f929a9272e3af36fc968e52abc6c27");
    CHECK(sha256_hex(pattern(63)) == "81c80242132f230c3bd41b3e63bbcff16107339549214a99614ff26664625055");
    CHECK(sha256_hex(pattern(64)) == "39e3d7b6b5d075d37d053ad89b24b41bef4f3c29760c84447cab3f3be1882241");
    CHECK(sha256_hex(pattern(119)) == "9ce7368e4daf32341631b492e80359dc9f594b48453cd0dd5bf0b19279cc177e");
    CHECK(sha256_hex(pattern(1000)) == "1e9bc38cbf860b9ec31918b065f9b52476c549a782e0e7990bed8ce3868d2371");
    const std::vector<uint8_t> million(1000000, 'a');
    CHECK(sha256_hex(million) == "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0");
}

// ---------------------------------------------------------------- decoder

std::string dis(uint16_t word, uint32_t pc = 0x8C010000u) { return sh4::disasm(sh4::decode(word), pc); }

void test_decoder() {
    using sh4::Op;
    // Text, as GNU objdump prints it (tools/check/verify_decoder.py compares all 65,536 words).
    CHECK(dis(0x0009) == "nop");
    CHECK(dis(0x000B) == "rts");
    CHECK(dis(0x4F22) == "sts.l pr,@-r15");
    CHECK(dis(0x2FE6) == "mov.l r14,@-r15");
    CHECK(dis(0x7FFC) == "add #-4,r15");
    CHECK(dis(0xE17F) == "mov #127,r1");
    CHECK(dis(0xE180) == "mov #-128,r1");
    CHECK(dis(0x80AA) == "mov.b r0,@(10,r10)");
    CHECK(dis(0x81AA) == "mov.w r0,@(20,r10)");
    CHECK(dis(0x1AAA) == "mov.l r10,@(40,r10)");
    CHECK(dis(0x5AB3) == "mov.l @(12,r11),r10");
    CHECK(dis(0xC2AA) == "mov.l r0,@(680,gbr)");
    CHECK(dis(0xCDAA) == "and.b #170,@(r0,gbr)");
    CHECK(dis(0x0AC2) == "stc r4_bank,r10");
    CHECK(dis(0x000F) == "mac.l @r0+,@r0+");
    CHECK(dis(0x4A5F) == "mac.w @r5+,@r10+");
    CHECK(dis(0xF2FD) == "fsca fpul,dr2");
    CHECK(dis(0xFA7D) == "fsrra fr10");
    CHECK(dis(0xF6ED) == "fipr fv8,fv4");
    CHECK(dis(0xF9FD) == "ftrv xmtrx,fv8");
    CHECK(dis(0xF12E) == "fmac fr0,fr2,fr1");
    CHECK(dis(0xF3FD) == "fschg");
    CHECK(dis(0xFBFD) == "frchg");
    CHECK(dis(0x8D00) == "bt.s 0x8c010004");

    // PC-relative: mov.w from pc + 4, mov.l and mova from the longword at or before pc + 4.
    CHECK(dis(0x9102, 0x8C010000u) == "mov.w 0x8c010008,r1");
    CHECK(dis(0x9102, 0x8C010002u) == "mov.w 0x8c01000a,r1");
    CHECK(dis(0xD101, 0x8C010000u) == "mov.l 0x8c010008,r1");
    CHECK(dis(0xD101, 0x8C010002u) == "mov.l 0x8c010008,r1");
    CHECK(dis(0xC701, 0x8C010002u) == "mova 0x8c010008,r0");
    // Branches: pc + 4 + signed displacement * 2.
    CHECK(dis(0xA003, 0x8C010000u) == "bra 0x8c01000a");
    CHECK(dis(0xAFFF, 0x8C010000u) == "bra 0x8c010002");
    CHECK(dis(0xB800, 0x8C010000u) == "bsr 0x8c00f004");
    CHECK(dis(0x8BFE, 0x8C010000u) == "bf 0x8c010000");
    CHECK(sh4::decode(0xA7FF).branch_target(0x8C010000u) == 0x8C011002u);

    // Not instructions on the SH7091.
    // (0x4036 and 0x403A are the SH-4A's `ldc.l @rN+,sgr` and `ldc rN,sgr`.)
    for (const unsigned w : {0x0000u, 0x0001u, 0xFFFDu, 0xF7FDu, 0xF1ADu, 0xF1BDu, 0x4036u, 0x403Au,
                             0xF0CDu, 0x8200u, 0x8600u, 0x8A00u, 0x8E00u, 0xFFFFu}) {
        CHECK(!sh4::decode(static_cast<uint16_t>(w)).valid());
        CHECK(dis(static_cast<uint16_t>(w)) == std::format(".word 0x{:04x}", w));
    }

    // Properties the analysis will lean on.
    const auto flags = [](uint16_t w) { return sh4::spec(sh4::decode(w).op).flags; };
    CHECK(sh4::decode(0x400B).op == Op::jsr);
    CHECK((flags(0x400B) & (sh4::kBranch | sh4::kDelayed | sh4::kCall | sh4::kIndirect)) ==
          (sh4::kBranch | sh4::kDelayed | sh4::kCall | sh4::kIndirect));
    CHECK((flags(0x8900) & sh4::kConditional) && !(flags(0x8900) & sh4::kDelayed));  // bt
    CHECK((flags(0x8D00) & sh4::kConditional) && (flags(0x8D00) & sh4::kDelayed));   // bt.s
    CHECK((flags(0x000B) & sh4::kReturn) && (flags(0x000B) & sh4::kDelayed));
    CHECK((flags(0xD000) & sh4::kPcRelative) && (flags(0xD000) & sh4::kSlotIllegal));
    CHECK(flags(0xF00C) & sh4::kFpSize);       // fmov
    CHECK(flags(0xF000) & sh4::kFpPrecision);  // fadd
    CHECK(flags(0x400E) & sh4::kPrivileged);   // ldc rN,sr
    CHECK(!(flags(0x401E) & sh4::kPrivileged));  // ldc rN,gbr

    // Every form decodes back to itself, and every valid word belongs to
    // exactly the form the table gives it.
    size_t valid = 0;
    for (uint32_t w = 0; w < 65536; ++w) {
        const sh4::Insn in = sh4::decode(static_cast<uint16_t>(w));
        if (!in.valid()) continue;
        ++valid;
        const sh4::Spec& s = sh4::spec(in.op);
        CHECK((w & s.mask) == s.match);
    }
    CHECK(sh4::specs().size() == static_cast<size_t>(Op::count_) - 1);
    for (const sh4::Spec& s : sh4::specs()) CHECK(sh4::decode(s.match).op == s.op);
    // The count the binutils cross-check agreed on (docs/recompiler-core.md).
    CHECK(valid == sh4::kValidWords);
}

// ---------------------------------------------------------------- IP.BIN and the boot file

void put_field(std::vector<uint8_t>& d, size_t offset, size_t size, std::string_view text) {
    std::memset(d.data() + offset, ' ', size);
    std::memcpy(d.data() + offset, text.data(), std::min(size, text.size()));
}

std::vector<uint8_t> synthetic_ip_bin() {
    std::vector<uint8_t> ip(kIpBinSize, 0);
    put_field(ip, 0x00, 16, "SEGA SEGAKATANA");
    put_field(ip, 0x10, 16, "SEGA ENTERPRISES");
    put_field(ip, 0x20, 16, "ABCD GD-ROM1/1");
    put_field(ip, 0x30, 8, "JUE");
    put_field(ip, 0x38, 8, "0799A10");
    put_field(ip, 0x40, 10, "T-00000");
    put_field(ip, 0x4A, 6, "V1.002");
    put_field(ip, 0x50, 16, "20001130");
    put_field(ip, 0x60, 16, "1ST_READ.BIN");
    put_field(ip, 0x70, 16, "QILIN TESTS");
    put_field(ip, 0x80, 128, "A SYNTHETIC DISC");
    return ip;
}

void test_ip_bin() {
    std::vector<uint8_t> bytes = synthetic_ip_bin();
    const auto ip = IpBin::parse(bytes);
    CHECK(ip.has_value());
    if (ip) {
        CHECK(ip->hardware_id == "SEGA SEGAKATANA");
        CHECK(ip->device_info == "ABCD GD-ROM1/1");
        CHECK(ip->area_symbols == "JUE");
        CHECK(ip->product_number == "T-00000");
        CHECK(ip->version == "V1.002");
        CHECK(ip->release_date == "20001130");
        CHECK(ip->boot_file == "1ST_READ.BIN");
        CHECK(ip->title == "A SYNTHETIC DISC");
        CHECK(!ip->uses_windows_ce);
        CHECK(ip->supports_vga);
    }
    put_field(bytes, 0x38, 8, "0799A01");
    const auto ce = IpBin::parse(bytes);
    CHECK(ce && ce->uses_windows_ce && !ce->supports_vga);

    // The first 256 bytes are enough; fewer, another console's ID, bytes
    // that aren't text, a bad peripherals field or no boot file are refused.
    CHECK(IpBin::parse(std::span(bytes).first(0x100)).has_value());
    CHECK(!IpBin::parse(std::span(bytes).first(0xFF)));
    auto bad = synthetic_ip_bin();
    put_field(bad, 0x00, 16, "SEGA SEGASATURN");
    CHECK(!IpBin::parse(bad));
    bad = synthetic_ip_bin();
    bad[0x85] = 0;
    CHECK(!IpBin::parse(bad));
    bad = synthetic_ip_bin();
    put_field(bad, 0x38, 8, "07Z9A10");
    CHECK(!IpBin::parse(bad));
    bad = synthetic_ip_bin();
    put_field(bad, 0x60, 16, "");
    CHECK(!IpBin::parse(bad));
}

void test_boot_file() {
    // mov.l 0x8c010004,r1 ; rts ; a literal ; nop
    const std::vector<uint8_t> bytes = {0x00, 0xD1, 0x0B, 0x00, 0x78, 0x56, 0x34, 0x12, 0x09, 0x00};
    const auto boot = BootFile::parse(bytes);
    CHECK(boot.has_value());
    if (boot) {
        CHECK(boot->base() == 0x8C010000u && boot->entry() == 0x8C010000u && boot->end() == 0x8C01000Au);
        CHECK(boot->sha256 == sha256_hex(bytes));
        CHECK(boot->u16(0x8C010000u) == 0xD100);
        CHECK(boot->u32(0x8C010004u) == 0x12345678u);
        CHECK(boot->u8(0x8C010009u) == 0x00);
        CHECK(boot->mapped(0x8C010009u) && !boot->mapped(0x8C01000Au) && !boot->mapped(0x8C00FFFFu));
        CHECK(!boot->mapped(0x8C010008u, 4));
        bool threw = false;
        try {
            (void)boot->u32(0x8C010008u);
        } catch (const BootFileError&) {
            threw = true;
        }
        CHECK(threw);
        CHECK(known_build(boot->sha256) == nullptr);
        const KnownBuild* game = known_build("7780f031bc7124dd68e2b411daf5e6fb636589ab4ea9b36427531d95517d9888");
        CHECK(game && game->file == "MSR.BIN" && game->version == "V1.010");
        const KnownBuild* launcher = known_build("b5515a96ba7e67dc2dfe08b6adf7d99a4d6c95e7e9fb015fe1f70dffa59af0e9");
        CHECK(launcher && launcher->file == "1ST_READ.BIN" && launcher->product_number == "MK-51012");
        const std::string json = to_json(*boot, nullptr);
        CHECK(json.contains("\"build\": null"));
        CHECK(json.contains("\"load_address\": \"0x8C010000\""));
    }
    CHECK(!BootFile::parse({}));
    const std::vector<uint8_t> too_big((16u << 20) - 0x10000 + 2);
    CHECK(!BootFile::parse(too_big));
    CHECK(BootFile::parse(std::span(too_big).first(too_big.size() - 2)).has_value());
    CHECK(json_string("a\"b\\c\n") == "\"a\\\"b\\\\c\\u000a\"");
}

// ---------------------------------------------------------------- function discovery

BootFile program(std::initializer_list<unsigned> words) {
    std::vector<uint8_t> bytes;
    for (const unsigned w : words) {
        bytes.push_back(static_cast<uint8_t>(w));
        bytes.push_back(static_cast<uint8_t>(w >> 8));
    }
    return *BootFile::parse(bytes);
}

void test_cfg() {
    constexpr uint32_t B = 0x8C010000u;
    // Five functions, each found a different way.
    const BootFile boot = program({
        /* 00 main */ 0xD102,  // mov.l 0x0c,r1      (the address of A)
        /* 02 */ 0x410B,       // jsr @r1
        /* 04 */ 0x0009,       // nop
        /* 06 */ 0x000B,       // rts
        /* 08 */ 0x0009,       // nop
        /* 0a */ 0x0000,       // padding
        /* 0c */ 0x0010, 0x8C01,
        /* 10 A */ 0x6043,     // mov r4,r0
        /* 12 */ 0xE103,       // mov #3,r1
        /* 14 */ 0x3012,       // cmp/hs r1,r0
        /* 16 */ 0x890B,       // bt 0x30            (default)
        /* 18 */ 0x4000,       // shll r0
        /* 1a */ 0x6103,       // mov r0,r1
        /* 1c */ 0xC701,       // mova 0x24,r0
        /* 1e */ 0x001D,       // mov.w @(r0,r1),r0
        /* 20 */ 0x0023,       // braf r0
        /* 22 */ 0x0009,       // nop
        /* 24 */ 0x0006, 0x0008, 0x000C,  // table: 0x2a, 0x2c, 0x30
        /* 2a */ 0xE001,       // mov #1,r0
        /* 2c */ 0x000B,       // rts
        /* 2e */ 0x0009,       // nop
        /* 30 */ 0xD301,       // mov.l 0x38,r3      (the address of C)
        /* 32 */ 0x432B,       // jmp @r3            (a tail call)
        /* 34 */ 0x0009,       // nop
        /* 36 */ 0x0000,       // padding
        /* 38 */ 0x0040, 0x8C01,
        /* 3c */ 0x0000, 0x0000,
        /* 40 C */ 0xD201,     // mov.l 0x48,r2      (the address of D: a callback, never called here)
        /* 42 */ 0x000B,       // rts
        /* 44 */ 0x0009,       // nop
        /* 46 */ 0x0000,
        /* 48 */ 0x0050, 0x8C01,
        /* 4c */ 0x0000, 0x0000,
        /* 50 D */ 0x000B,     // rts
        /* 52 */ 0x0009,       // nop
        /* 54 E */ 0xE000,     // mov #0,r0          (nothing refers to E: found as the gap after D)
        /* 56 */ 0x000B,       // rts
        /* 58 */ 0x0009,       // nop
        /* 5a */ 0xFFFF, 0xFFFF,  // data
    });
    Cfg cfg(boot);
    cfg.run();
    const auto& fns = cfg.functions();
    CHECK(fns.size() == 5);
    if (fns.size() == 5) {
        const auto origin = [&](uint32_t a) { return fns.contains(B + a) ? fns.at(B + a).origin : Origin::Gap; };
        CHECK(fns.contains(B) && origin(0x00) == Origin::Entry);
        CHECK(fns.contains(B + 0x10) && origin(0x10) == Origin::Call);
        CHECK(fns.contains(B + 0x40) && origin(0x40) == Origin::TailCall);
        CHECK(fns.contains(B + 0x50) && origin(0x50) == Origin::Literal);
        CHECK(fns.contains(B + 0x54) && origin(0x54) == Origin::Gap);
    }
    if (fns.contains(B)) {
        const Function& f = fns.at(B);
        CHECK(f.calls.size() == 1 && f.calls.at(B + 0x02) == B + 0x10);
        CHECK(f.insns.size() == 5 && f.slots.size() == 2 && f.returns && f.end == B + 0x0A);
        CHECK(f.literals.size() == 1 && f.literals.at(B + 0x0C) == 4);
    }
    if (fns.contains(B + 0x10)) {
        const Function& f = fns.at(B + 0x10);
        CHECK(f.jtabs.size() == 1);
        if (f.jtabs.size() == 1) {
            CHECK(f.jtabs[0].at == B + 0x20 && f.jtabs[0].table == B + 0x24);
            CHECK(f.jtabs[0].targets == std::vector<uint32_t>({B + 0x2A, B + 0x2C, B + 0x30}));
        }
        CHECK(f.movas.empty());  // the mova is the table's, not a literal's
        CHECK(f.tails.size() == 1 && f.tails.at(B + 0x32) == B + 0x40);
        CHECK(f.itails.empty() && f.clean() && f.returns);
        CHECK(f.leaders.contains(B + 0x2A) && f.leaders.contains(B + 0x2C) && f.leaders.contains(B + 0x30) &&
              f.leaders.contains(B + 0x18));
    }
    CHECK(cfg.suspects().empty());
    CHECK(cfg.rejected() == std::vector<uint32_t>({B + 0x5A}));
    const Coverage c = cfg.coverage();
    CHECK(c.lo == B && c.hi == B + 0x5A && c.after == 4 && c.before == 0);
    CHECK(c.literal == 12 && c.table == 6 && c.padding == 14 && c.unknown == 0);
    CHECK(c.code + c.literal + c.table + c.padding == 0x5A);
    CHECK(cfg.to_json().contains("\"origin\": \"literal pointer\""));
    CHECK(percent(1, 3, 2) == "33.33%" && percent(2, 3, 0) == "67%" && percent(0, 0, 1) == "n/a");

    // Constants where paths join: the same value on both is kept, different values aren't.
    const auto join = [](unsigned second_literal_load) {
        return program({
            /* 00 */ 0xD103,   // mov.l 0x10,r1
            /* 02 */ 0x8900,   // bt 0x06
            /* 04 */ second_literal_load,
            /* 06 */ 0x410B,   // jsr @r1
            /* 08 */ 0x0009,   // nop
            /* 0a */ 0x000B,   // rts
            /* 0c */ 0x0009,   // nop
            /* 0e */ 0x0000,
            /* 10 */ 0x0018, 0x8C01,
            /* 14 */ 0x001C, 0x8C01,
            /* 18 */ 0x000B, 0x0009,
            /* 1c */ 0x000B, 0x0009,
        });
    };
    {
        const BootFile same = join(0xD102);       // mov.l 0x10,r1 again
        const Function f = Cfg(same).explore(B, same.end());
        CHECK(f.calls.size() == 1 && f.icalls.empty());
        const BootFile different = join(0xD103);  // mov.l 0x14,r1
        const Function g = Cfg(different).explore(B, different.end());
        CHECK(g.calls.empty() && g.icalls == std::vector<uint32_t>({B + 0x06}));
        // A call forgets every register: the second jsr isn't resolved.
        const BootFile twice = program({0xD103, 0x410B, 0x0009, 0x410B, 0x0009, 0x000B, 0x0009, 0x0000, 0x000A, 0x8C01});
        const Function h = Cfg(twice).explore(B, twice.end());
        CHECK(h.calls.size() == 1 && h.icalls == std::vector<uint32_t>({B + 0x06}));
    }
    // Delay slots: a branch can't sit in one; a branch into one is recorded.
    {
        const BootFile nested = program({0xA000, 0xA000, 0x0009, 0x000B, 0x0009});  // bra ; bra
        const Function f = Cfg(nested).explore(B, nested.end());
        CHECK(f.bad_slots == std::vector<uint32_t>({B}) && !f.clean());
        // bf 0x04 ; bra 0x06 ; (04, the slot) nop ; (06) rts ; nop
        const BootFile into = program({0x8B00, 0xA000, 0x0009, 0x000B, 0x0009});
        const Function g = Cfg(into).explore(B, into.end());
        CHECK(g.slot_targets == std::vector<uint32_t>({B + 0x04}) && !g.clean());
        // The slot runs before the jump but after it reads its register:
        // jmp @r1 with "mov #0,r1" in the slot still goes where r1 pointed
        // (and a jump forward through a constant is a tail call).
        const BootFile order = program({0xD101, 0x412B, 0xE100, 0x0000, 0x000C, 0x8C01, 0x000B, 0x0009});
        const Function h = Cfg(order).explore(B, order.end());
        CHECK(h.clean() && h.insns.size() == 3 && h.tails.size() == 1 && h.tails.at(B + 0x02) == B + 0x0C);
    }
    // Reaching a word that isn't an instruction, and running off the image.
    {
        const BootFile junk = program({0x0009, 0xFFFF});
        CHECK(Cfg(junk).explore(B, junk.end()).bad == std::vector<uint32_t>({B + 2}));
        const BootFile off = program({0x0009, 0x0009});
        CHECK(Cfg(off).explore(B, off.end()).bad == std::vector<uint32_t>({B + 2}));
    }

    // A forward bra once PR is restored is a tail call; without that it is a branch.
    {
        // sts.l pr,@-r15 ; bra 0x0a ; lds.l @r15+,pr ; rts ; nop ; (0a) rts ; nop
        const BootFile tail = program({0x4F22, 0xA002, 0x4F26, 0x000B, 0x0009, 0x000B, 0x0009});
        const Function f = Cfg(tail).explore(B, tail.end());
        CHECK(f.tails.size() == 1 && f.tails.at(B + 0x02) == B + 0x0A && f.insns.size() == 3);
        const BootFile branch = program({0x4F22, 0xA002, 0x0009, 0x000B, 0x0009, 0x000B, 0x0009});
        const Function g = Cfg(branch).explore(B, branch.end());
        CHECK(g.tails.empty() && g.insns.size() == 5 && g.returns);
    }
    // A function known only from a pointer in data, a run of data that code
    // refers to, and candidates in it that must be turned down.
    {
        const BootFile boot2 = program({
            /* 00 main */ 0xD103,  // mov.l 0x10,r1     (the address of data at 0x30)
            /* 02 */ 0xB01D,       // bsr 0x40
            /* 04 */ 0x0009,       // nop
            /* 06 */ 0x000B,       // rts
            /* 08 */ 0x0009,       // nop
            /* 0a */ 0x0000, 0x0000, 0x0000,
            /* 10 */ 0x0030, 0x8C01,
            /* 14 F */ 0xE000,     // mov #0,r0         (only a pointer in the data names F)
            /* 16 */ 0x000B,       // rts
            /* 18 */ 0x0009,       // nop
            /* 1a */ 0x0000,
            /* 1c */ 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
            /* 30 */ 0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF,
            /* 38 */ 0x0014, 0x8C01,  // a pointer to F
            /* 3c */ 0xFFFF, 0xFFFF,
            /* 40 H */ 0x000B,     // rts
            /* 42 */ 0x0009,       // nop
        });
        Cfg cfg2(boot2);
        cfg2.run();
        const auto& fns2 = cfg2.functions();
        CHECK(fns2.size() == 3);
        CHECK(fns2.contains(B + 0x14) && fns2.at(B + 0x14).origin == Origin::DataPointer);
        CHECK(fns2.contains(B + 0x40) && fns2.at(B + 0x40).origin == Origin::Call);
        CHECK(cfg2.rejected() == std::vector<uint32_t>({B + 0x1C, B + 0x30}));
        const Coverage c2 = cfg2.coverage();
        CHECK(c2.lo == B && c2.hi == B + 0x44);
        CHECK(c2.code == 20 && c2.literal == 4 && c2.padding == 6 && c2.data == 38 && c2.data_runs == 1);
        CHECK(c2.data_references == 1 && c2.dead == 0 && c2.unknown == 0);
        CHECK(cfg2.trial(B + 0x1C).contains("would be rejected"));
        CHECK(cfg2.trial(B + 0x14).contains("already a function"));
    }
    // Bytes a function jumps over and nothing refers to are dead, not unknown.
    {
        // bra 0x0a ; nop ; six bytes of junk ; (0a) rts ; nop
        const BootFile over = program({0xA003, 0x0009, 0xFFFF, 0xFFFF, 0xFFFF, 0x000B, 0x0009});
        Cfg cfg3(over);
        cfg3.run();
        const Coverage c3 = cfg3.coverage();
        CHECK(cfg3.functions().size() == 1 && c3.code == 8 && c3.dead == 6 && c3.unknown == 0);
    }

    // Floating-point mode: FPSCR's PR, SZ and FR followed through the function.
    {
        using Modes = std::vector<std::pair<uint32_t, uint8_t>>;
        // A mode as fp_modes stores it: the bits, all three known.
        const auto all_known = [](unsigned bits) { return static_cast<uint8_t>(bits | kFpAll << 4); };
        const auto at = [](uint32_t pc, uint8_t mode) { return std::pair<uint32_t, uint8_t>(pc, mode); };
        const auto fp = [](std::initializer_list<unsigned> words) {
            const BootFile image = program(words);
            return Cfg(image).explore(0x8C010000u, image.end());
        };
        // The compiler's change of precision: sts fpscr ; or/and mask ; lds fpscr.
        const auto precision = [&](unsigned in_double) {
            return fp({
                /* 00 */ 0x006A,  // sts fpscr,r0
                /* 02 */ 0xD105,  // mov.l 0x18,r1      (0x00080000: PR)
                /* 04 */ 0x201B,  // or r1,r0
                /* 06 */ 0x406A,  // lds r0,fpscr
                /* 08 */ in_double,
                /* 0a */ 0x006A,  // sts fpscr,r0
                /* 0c */ 0xD103,  // mov.l 0x1c,r1      (0xFFF7FFFF)
                /* 0e */ 0x2019,  // and r1,r0
                /* 10 */ 0x406A,  // lds r0,fpscr
                /* 12 */ 0xF210,  // fadd fr1,fr2       (single again)
                /* 14 */ 0x000B, 0x0009, 0x0000, 0x0008, 0xFFFF, 0xFFF7,
            });
        };
        const Function f = precision(0xF210);  // fadd fr1,fr2
        CHECK(f.fp_modes == Modes({at(B + 0x08, all_known(kFpPr)), at(B + 0x12, all_known(0))}));
        CHECK(f.fp_unknown.empty() && f.fp_wrong.empty() && f.fp_leaks.empty() && f.changes_fp_mode);
        // fipr is single precision only: reached with PR set, it is wrong.
        const Function g = precision(0xF0ED);
        CHECK(g.fp_wrong == std::vector<uint32_t>({B + 0x08}) && g.fp_unknown.empty());
        // fcnvsd is double precision only: with PR set it is right.
        const Function g2 = precision(0xF0AD);
        CHECK(g2.fp_wrong.empty() && g2.fp_unknown.empty());

        // fschg ; fmov fr0,fr1 ; fschg ; rts ; nop
        const Function h = fp({0xF3FD, 0xF10C, 0xF3FD, 0x000B, 0x0009});
        CHECK(h.fp_modes == Modes({at(B + 0x02, all_known(kFpSz))}) && h.fp_leaks.empty() && h.changes_fp_mode);
        // Returning with the mode changed breaks the proof for every caller.
        CHECK(fp({0xF3FD, 0x000B, 0x0009}).fp_leaks == std::vector<uint32_t>({B + 0x02}));
        // So does calling out in a changed mode; after the call the mode is the entry mode again.
        const Function call = fp({0xFBFD, 0xB002, 0x0009, 0xF10C, 0x000B, 0x0009});  // frchg ; bsr ; nop ; fmov ; rts ; nop
        CHECK(call.fp_leaks == std::vector<uint32_t>({B + 0x02}));
        CHECK(call.fp_modes == Modes({at(B + 0x06, all_known(0))}));
        // A mode loaded from a register nothing is known about: unproven.
        const Function u = fp({0x446A, 0xF210, 0x000B, 0x0009});  // lds r4,fpscr ; fadd ; rts ; nop
        CHECK(u.fp_unknown == std::vector<uint32_t>({B + 0x02}) && u.fp_leaks == std::vector<uint32_t>({B + 0x04}));
        // Saved and restored around a change: sts.l fpscr,@-r15 ; fschg ; lds.l @r15+,fpscr ; fmov ; rts ; nop
        const Function sv = fp({0x4F62, 0xF3FD, 0x4F66, 0xF10C, 0x000B, 0x0009});
        CHECK(sv.fp_modes == Modes({at(B + 0x06, all_known(0))}) && sv.fp_leaks.empty() && sv.fp_unknown.empty());
        // A function with no floating point has nothing to prove.
        const Function none = fp({0xE000, 0x000B, 0x0009});
        CHECK(none.fp_modes.empty() && !none.changes_fp_mode && none.fp_leaks.empty());
    }

    // What an instruction writes.
    const auto writes = [](uint16_t w) { return sh4::gpr_writes(sh4::decode(w)); };
    CHECK(writes(0x6123) == 1u << 1);                // mov r2,r1
    CHECK(writes(0x6126) == (1u << 1 | 1u << 2));    // mov.l @r2+,r1
    CHECK(writes(0x2126) == 1u << 1);                // mov.l r2,@-r1
    CHECK(writes(0x2122) == 0);                      // mov.l r2,@r1
    CHECK(writes(0x3120) == 0);                      // cmp/eq r2,r1
    CHECK(writes(0x8801) == 0);                      // cmp/eq #1,r0
    CHECK(writes(0xC901) == 1);                      // and #1,r0
    CHECK(writes(0x8412) == 1);                      // mov.b @(2,r1),r0
    CHECK(writes(0x0127) == 0);                      // mul.l r2,r1
    CHECK(writes(0x012F) == (1u << 1 | 1u << 2));    // mac.l @r2+,@r1+
    CHECK(writes(0x4110) == 1u << 1);                // dt r1
    CHECK(writes(0x410B) == 0);                      // jsr @r1
    CHECK(writes(0x012A) == 1u << 1);                // sts pr,r1
    CHECK(writes(0x412A) == 0);                      // lds r1,pr
    CHECK(writes(0xF129) == 1u << 2);                // fmov @r2+,fr1
    CHECK(writes(0xFFFF) == 0);
}

// ---------------------------------------------------------------- lifter and runtime helpers

void test_lifter() {
    constexpr uint32_t B = 0x8C010000u;
    // mov.l L,r1 ; jsr @r1 ; nop ; bt.s 0x12 ; clrt ; mov #1,r0 ; bra 0x12 ; nop ; ... ; (12) rts ; nop ; (18) L
    const BootFile boot = program({
        /* 00 */ 0x4F22,  // sts.l pr,@-r15
        /* 02 */ 0xD105,  // mov.l 0x18,r1      (the address of the callee)
        /* 04 */ 0x410B,  // jsr @r1
        /* 06 */ 0x6143,  // mov r4,r1          (in the slot: after the target is read)
        /* 08 */ 0x8D03,  // bt.s 0x12
        /* 0a */ 0x0008,  // clrt               (in the slot: after the condition is read)
        /* 0c */ 0xE001,  // mov #1,r0
        /* 0e */ 0xA000,  // bra 0x12
        /* 10 */ 0x0009,  // nop
        /* 12 */ 0x4F26,  // lds.l @r15+,pr
        /* 14 */ 0x000B,  // rts
        /* 16 */ 0x6F56,  // mov.l @r5+,r15     (in the slot: after PR is read)
        /* 18 */ 0x001C, 0x8C01,
        /* 1c callee */ 0x000B,  // rts
        /* 1e */ 0x0009,
    });
    Cfg cfg(boot);
    cfg.run();
    CHECK(cfg.functions().size() == 2);
    const Lifter lifter(cfg);
    if (cfg.functions().contains(B)) {
        const LiftResult r = lifter.lift(cfg.functions().at(B));
        CHECK(r.ok);
        const std::string& code = r.code;
        CHECK(code.contains("void fn_8c010000(rt::Context& c) {"));
        // A push: the value stored, then the decrement.
        CHECK(code.contains("rt::st32(r15 - 4, c.pr); r15 -= 4;"));
        // The literal is a constant; the call is direct, sets PR, runs the slot, and checks the return.
        CHECK(code.contains("r1 = 0x8c01001cu;"));
        CHECK(code.contains("{ c.pr = 0x8c010008u; r1 = r4; QILIN_OUT fn_8c01001c(c); "
                            "rt::returned_to(c, 0x8c010008u, 0x8c010004u); QILIN_IN }"));
        // bt.s: the condition from before the slot.
        CHECK(code.contains("{ const bool take = t; t = 0; if (take) goto L_8c010012; }"));
        CHECK(code.contains("goto L_8c010012;\nL_8c010012:\n"));
        // rts: PR from before the slot, which here pops into r15.
        CHECK(code.contains("{ const uint32_t ret = c.pr; QILIN_PC(0x8c010016u); r15 = rt::ld32(r5); r5 += 4; "
                            "QILIN_OUT c.ret = ret; return; }"));
        // Registers written go back to the context; registers used are read again after a call.
        CHECK(code.contains("#define QILIN_OUT c.r[0] = r0; c.r[1] = r1; c.r[5] = r5; c.r[15] = r15; c.t = t;\n"));
        CHECK(code.contains("#define QILIN_IN r0 = c.r[0]; r1 = c.r[1]; r4 = c.r[4]; r5 = c.r[5]; r15 = c.r[15]; t = c.t;\n"));
        CHECK(!code.contains("    ;\n"));  // nops leave nothing behind
    }
    // The compiler's switch becomes a switch on the target, and a tail call a call and return.
    {
        const BootFile sw = program({0x0009, 0x0009, 0x6043, 0xE103, 0x3012, 0x890B, 0x4000, 0x6103, 0xC701, 0x001D, 0x0023, 0x0009,
                                     0x0006, 0x0008, 0x000C, 0xE001, 0x000B, 0x0009, 0xD301, 0x432B, 0x0009, 0x0000,
                                     0x0034, 0x8C01, 0x0000, 0x0000, 0x000B, 0x0009});
        Cfg c2(sw);
        c2.run();
        const LiftResult r = Lifter(c2).lift(c2.functions().at(B));
        CHECK(r.ok);
        CHECK(r.code.contains("{ const uint32_t target = 0x8c010018u + r0; switch (target) { "
                              "case 0x8c01001eu: goto L_8c01001e; case 0x8c010020u: goto L_8c010020; "
                              "case 0x8c010024u: goto L_8c010024; default: rt::bad_jump(0x8c010014u, target); } }"));
        CHECK(r.code.contains("QILIN_OUT fn_8c010034(c); return;"));
        CHECK(r.code.contains("if (t) goto L_8c010024;"));
    }
    // What the lifter refuses, with the reason.
    {
        const BootFile priv = program({0x0002, 0x000B, 0x0009});  // stc sr,r0
        Cfg c4(priv);
        c4.run();
        const LiftResult p = Lifter(c4).lift(c4.functions().at(B));
        CHECK(!p.ok && p.why == "privileged");
    }

    // ---- the helpers generated code calls ----
    uint32_t t = 1;
    CHECK(rt::addc(0xFFFFFFFFu, 0, t) == 0 && t == 1);
    CHECK(rt::addc(1, 2, t) == 4 && t == 0);
    CHECK(rt::subc(0, 0, t) == 0 && t == 0);
    t = 1;
    CHECK(rt::subc(0, 0, t) == 0xFFFFFFFFu && t == 1);
    CHECK(rt::subc(5, 3, t) == 1 && t == 0);
    CHECK(rt::addv(0x7FFFFFFFu, 1, t) == 0x80000000u && t == 1);
    CHECK(rt::addv(1, 1, t) == 2 && t == 0);
    CHECK(rt::subv(0x80000000u, 1, t) == 0x7FFFFFFFu && t == 1);
    CHECK(rt::subv(3, 5, t) == 0xFFFFFFFEu && t == 0);
    CHECK(rt::cmp_str(0x11223344u, 0x55663377u) == 1 && rt::cmp_str(0x11223344u, 0x44332211u) == 0);
    CHECK(rt::shad(0x80000000u, 0xFFFFFFFFu) == 0xC0000000u);   // right by 1, arithmetic
    CHECK(rt::shad(0x80000000u, 0xFFFFFFE0u) == 0xFFFFFFFFu);   // right by 32: all sign
    CHECK(rt::shad(1, 4) == 16 && rt::shad(1, 36) == 16);       // left by the low five bits
    CHECK(rt::shld(0x80000000u, 0xFFFFFFFFu) == 0x40000000u);
    CHECK(rt::shld(0x80000000u, 0xFFFFFFE0u) == 0);
    // div1: the manual's 32 / 16 unsigned division (shll16 ; div0u ; 16 x div1 ; rotcl ; extu.w).
    const auto divide = [](uint32_t dividend, uint32_t divisor) {
        rt::Context c;
        uint32_t tt = 0, rn = dividend;
        const uint32_t rm = divisor << 16;
        for (int i = 0; i < 16; ++i) rn = rt::div1(c, rn, rm, tt);
        return (rn << 1 | tt) & 0xFFFFu;
    };
    CHECK(divide(1000, 7) == 142 && divide(0, 5) == 0 && divide(65535, 1) == 65535 && divide(0x12345678u, 0x4321u) == 0x12345678u / 0x4321u);
    // mac.l saturates to 48 bits when S is set; mac.w saturates MACL.
    {
        rt::Context c;
        c.mach = 0x00007FFFu, c.macl = 0xFFFFFFFFu;
        rt::mac_l(c, 2, 3);
        CHECK(c.mach == 0x00008000u && c.macl == 5);
        c.mach = 0x00007FFFu, c.macl = 0xFFFFFFFFu, c.s = 1;
        rt::mac_l(c, 2, 3);
        CHECK(c.mach == 0x00007FFFu && c.macl == 0xFFFFFFFFu);
        c = {};
        c.s = 1, c.macl = 0x7FFFFFFFu;
        rt::mac_w(c, 2, 3);
        CHECK(c.macl == 0x7FFFFFFFu && c.mach == 1);
        c = {};
        c.macl = 0xFFFFFFFFu;
        rt::mac_w(c, 0xFFFFu, 1);  // -1 * 1 added to 0x00000000_FFFFFFFF
        CHECK(c.macl == 0xFFFFFFFEu && c.mach == 0);
    }
}

// ---------------------------------------------------------------- floating point

void test_floating_point() {
    using namespace qilin::rt;
    const auto bits = [](float f) { return std::bit_cast<uint32_t>(f); };
    // DN = 1: denormalised inputs are zero, and so are denormalised results.
    CHECK(fadd(0x00800000u, 0x00000001u) == 0x00800000u);
    CHECK(fadd(0x80000001u, 0x00000000u) == 0x00000000u);  // -0 + +0
    CHECK(fsub(0x00800001u, 0x00800000u) == 0);            // 2^-149: denormal
    CHECK(fmul(0x00000001u, bits(1073741824.0f)) == 0);   // denormal * 2^30: the input is zero
    CHECK(fcmp_eq(0x00000001u, 0x80000000u) == 1);          // a denormal equals zero
    // Any NaN result is the default NaN.
    CHECK(fadd(0x7F800000u, 0xFF800000u) == kDefaultNan);
    CHECK(fadd(0x7FC12345u, bits(1.0f)) == kDefaultNan);
    CHECK(fsqrt(bits(-1.0f)) == kDefaultNan);
    CHECK(fdiv(bits(1.0f), 0) == 0x7F800000u);
    CHECK(dadd(0x7FF0000000000000ull, 0xFFF0000000000000ull) == kDefaultNan64);
    CHECK(fcmp_gt(0x7FC00000u, bits(1.0f)) == 0);
    // fmac rounds once: (1 + 2^-23)^2 - (1 + 2^-22) = 2^-46.
    CHECK(fmac(0x3F800001u, 0x3F800001u, 0xBF800002u) == 0x28800000u);
    // Conversions.
    CHECK(float_s(0x7FFFFFFFu) == 0x4F000000u);
    CHECK(ftrc_s(bits(-2.7f)) == 0xFFFFFFFEu && ftrc_s(bits(3e9f)) == 0x7FFFFFFFu);
    CHECK(ftrc_s(bits(-3e9f)) == 0x80000000u && ftrc_s(0x7FC00000u) == 0x7FFFFFFFu && ftrc_s(0xFFC00000u) == 0x7FFFFFFFu);
    CHECK(fcnvds(std::bit_cast<uint64_t>(1.0 / 3.0)) == 0x3EAAAAABu);
    CHECK(fcnvsd(bits(0.5f)) == std::bit_cast<uint64_t>(0.5));
    CHECK(float_d(0x80000000u) == std::bit_cast<uint64_t>(-2147483648.0));
    // Double registers: DRn is FRn (high) and FRn+1.
    uint32_t bank[16] = {};
    set64(bank, 2, std::bit_cast<uint64_t>(1.5));
    CHECK(bank[2] == 0x3FF80000u && bank[3] == 0 && get64(bank, 2) == std::bit_cast<uint64_t>(1.5));
    // A 64-bit fmov puts the even register at the lower address (the hardware's order).
    {
        std::vector<uint8_t> memory(64, 0);
        uint8_t* const saved = g_mem;
        g_mem = memory.data();
        bank[4] = 0x11111111u, bank[5] = 0x22222222u;
        st_pair(8, bank, 4);
        CHECK(ld32(8) == 0x11111111u && ld32(12) == 0x22222222u);
        // The compiler's double constants are laid out that way: 0.301 as 3FD34395 810624DD.
        st32(16, 0x3FD34395u), st32(20, 0x810624DDu);
        ld_pair(16, bank, 6);
        CHECK(f64(get64(bank, 6)) == 0.301);
        g_mem = saved;
    }
    // fipr, ftrv, fsrra, fsca.
    for (unsigned i = 0; i < 8; ++i) bank[i] = bits(float(i + 1));
    CHECK(fipr(bank, 0, 4) == bits(70.0f));
    uint32_t matrix[16] = {};
    for (unsigned i = 0; i < 4; ++i) matrix[5 * i] = bits(1.0f);
    ftrv(matrix, bank, 4);
    CHECK(bank[4] == bits(5.0f) && bank[7] == bits(8.0f));
    CHECK(fsrra(bits(4.0f)) == bits(0.5f));
    fsca(0, bank, 0);
    CHECK(bank[0] == 0 && bank[1] == bits(1.0f));
    fsca(0x4000, bank, 0);  // a quarter turn
    CHECK(bank[0] == bits(1.0f) && bank[1] == 0);
    fsca(0x2000, bank, 0);  // an eighth
    CHECK(bank[0] == 0x3F3504F3u && bank[1] == 0x3F3504F3u);
    fsca(0x1C000, bank, 0);  // three quarters (bits above 16 ignored)
    CHECK(bank[0] == bits(-1.0f) && bank[1] == 0);
    // Loading FPSCR: round to nearest without exceptions enabled only.
    uint32_t fpscr = 0;
    lds_fpscr(fpscr, 0x00040000u, 0);
    CHECK(fpscr == 0x00040000u);
    bool stopped = false;
    try {
        lds_fpscr(fpscr, 0x00040001u, 0x8C010000u);  // round to zero
    } catch (const Stop& stop) {
        stopped = stop.kind == Stop::Unsupported && stop.value == 0x00040001u;
    }
    CHECK(stopped && fpscr == 0x00040000u);
}

void test_lifter_floating_point() {
    const auto lift = [](std::initializer_list<unsigned> words) {
        const BootFile image = program(words);
        Cfg cfg(image);
        cfg.run();
        return Lifter(cfg).lift(cfg.functions().at(0x8C010000u));
    };
    // Single precision in the entry mode.
    const LiftResult single = lift({0xF210, 0xF225, 0x000B, 0x0009});  // fadd fr1,fr2 ; fcmp/gt fr2,fr2 ; rts ; nop
    CHECK(single.ok && single.code.contains("c.fr[0][2] = rt::fadd(c.fr[0][2], c.fr[0][1]);"));
    CHECK(single.code.contains("t = rt::fcmp_gt(c.fr[0][2], c.fr[0][2]);"));
    // The compiler's change to double precision and back.
    const LiftResult dbl = lift({0x006A, 0xD105, 0x201B, 0x406A, 0xF420, 0x006A, 0xD103, 0x2019, 0x406A, 0xF210,
                                 0x000B, 0x0009, 0x0000, 0x0008, 0xFFFF, 0xFFF7});
    CHECK(dbl.ok);
    CHECK(dbl.code.contains("rt::set64(c.fr[0], 4, rt::dadd(rt::get64(c.fr[0], 4), rt::get64(c.fr[0], 2)));"));
    CHECK(dbl.code.contains("rt::lds_fpscr(c.fpscr, r0, 0x8c010006u);"));
    CHECK(dbl.code.contains("c.fr[0][2] = rt::fadd(c.fr[0][2], c.fr[0][1]);"));
    // fschg: 64-bit moves, DR for an even register and XD for an odd one.
    // fschg ; fmov dr2,xd4 ; fmov @r4,dr6 ; fmov xd2,@-r15 ; fschg ; rts ; nop
    const LiftResult pairs = lift({0xF3FD, 0xF52C, 0xF648, 0xFF3B, 0xF3FD, 0x000B, 0x0009});
    CHECK(pairs.ok);
    CHECK(pairs.code.contains("c.fpscr ^= rt::kFpscrSz;"));
    CHECK(pairs.code.contains("{ const uint32_t hi = c.fr[0][2], lo = c.fr[0][3]; c.fr[1][4] = hi; c.fr[1][5] = lo; }"));
    CHECK(pairs.code.contains("rt::ld_pair(r4, c.fr[0], 6);"));
    CHECK(pairs.code.contains("rt::st_pair(r15 - 8, c.fr[1], 2); r15 -= 8;"));
    // frchg: the banks swap. frchg ; fmov fr1,fr0 ; frchg ; rts ; nop
    const LiftResult banks = lift({0xFBFD, 0xF01C, 0xFBFD, 0x000B, 0x0009});
    CHECK(banks.ok && banks.code.contains("c.fr[1][0] = c.fr[1][1];") && banks.code.contains("c.fpscr ^= rt::kFpscrFr;"));
    // fipr, ftrv, fsca, fmac.
    const LiftResult vector = lift({0xF4ED, 0xF9FD, 0xF2FD, 0xF21E, 0x000B, 0x0009});
    CHECK(vector.ok);
    CHECK(vector.code.contains("c.fr[0][7] = rt::fipr(c.fr[0], 0, 4);"));
    CHECK(vector.code.contains("rt::ftrv(c.fr[1], c.fr[0], 8);"));
    CHECK(vector.code.contains("rt::fsca(c.fpul, c.fr[0], 2);"));
    CHECK(vector.code.contains("c.fr[0][2] = rt::fmac(c.fr[0][0], c.fr[0][1], c.fr[0][2]);"));
    // Refused: a mode the analysis can't prove, and leaving in another mode.
    const LiftResult unknown = lift({0x446A, 0xF210, 0x000B, 0x0009});  // lds r4,fpscr ; fadd ...
    CHECK(!unknown.ok && unknown.why == "floating-point mode not proven");
    const LiftResult leak = lift({0xF3FD, 0x000B, 0x0009});  // fschg ; rts
    CHECK(!leak.ok && leak.why == "leaves in a floating-point mode other than the entry mode");
}

// ---------------------------------------------------------------- GD-ROM

// A synthetic GD-ROM's high-density area, as 2048-byte sectors from sector
// 45000: IP.BIN, an ISO 9660 file system with a directory and three files.
// The boot file sits in a second data track after an audio track, as on
// discs with CD audio.
struct SyntheticDisc {
    static constexpr uint32_t kTrack3Sectors = 40;   // 45000-45039
    static constexpr uint32_t kTrack4Sectors = 10;   // 45040-45049, audio
    static constexpr uint32_t kTrack5Sectors = 20;   // 45050-45069
    static constexpr uint32_t kRoot = 45020, kSub = 45021, kReadme = 45022, kDeep = 45023, kBoot = 45055;

    std::vector<uint8_t> boot = pattern(5000);       // spans three sectors
    std::vector<uint8_t> readme = {'h', 'e', 'l', 'l', 'o'};
    std::vector<uint8_t> deep = pattern(2048);
    std::vector<uint8_t> area = std::vector<uint8_t>(size_t(70) * 2048, 0);

    uint8_t* at(uint32_t sector) { return area.data() + size_t(sector - 45000) * 2048; }

    static void both32(uint8_t* p, uint32_t v) {
        for (int i = 0; i < 4; ++i) p[i] = p[7 - i] = static_cast<uint8_t>(v >> (8 * i));
    }

    // Appends a directory record at `p`; returns its length.
    static size_t record(uint8_t* p, std::string_view name, uint32_t sector, uint32_t size, bool directory) {
        const size_t length = (33 + name.size() + 1) & ~size_t(1);
        p[0] = static_cast<uint8_t>(length);
        both32(p + 2, sector);
        both32(p + 10, size);
        p[25] = directory ? 2 : 0;
        p[32] = static_cast<uint8_t>(name.size());
        std::memcpy(p + 33, name.data(), name.size());
        return length;
    }

    SyntheticDisc() {
        const std::vector<uint8_t> ip = synthetic_ip_bin();
        std::memcpy(at(45000), ip.data(), ip.size());
        uint8_t* pvd = at(45016);
        pvd[0] = 1;
        std::memcpy(pvd + 1, "CD001", 5);
        pvd[128] = 0x00, pvd[129] = 0x08;  // logical block size 2048, little-endian half
        record(pvd + 156, std::string_view("\0", 1), kRoot, 2048, true);

        uint8_t* root = at(kRoot);
        size_t pos = record(root, std::string_view("\0", 1), kRoot, 2048, true);
        pos += record(root + pos, std::string_view("\1", 1), kRoot, 2048, true);
        pos += record(root + pos, "1ST_READ.BIN;1", kBoot, static_cast<uint32_t>(boot.size()), false);
        pos += record(root + pos, "README.;1", kReadme, static_cast<uint32_t>(readme.size()), false);
        record(root + pos, "DATA", kSub, 2048, true);

        uint8_t* sub = at(kSub);
        pos = record(sub, std::string_view("\0", 1), kSub, 2048, true);
        pos += record(sub + pos, std::string_view("\1", 1), kRoot, 2048, true);
        record(sub + pos, "DEEP.DAT;1", kDeep, static_cast<uint32_t>(deep.size()), false);

        std::memcpy(at(kReadme), readme.data(), readme.size());
        std::memcpy(at(kDeep), deep.data(), deep.size());
        std::memcpy(at(kBoot), boot.data(), boot.size());
    }
};

uint8_t to_bcd(uint32_t v) { return static_cast<uint8_t>((v / 10) << 4 | v % 10); }

// `sectors` sectors of user data starting at `first`, as a raw Mode 1 track.
std::vector<uint8_t> raw_track(const uint8_t* data, uint32_t first, uint32_t sectors) {
    std::vector<uint8_t> raw(size_t(sectors) * 2352, 0);
    for (uint32_t i = 0; i < sectors; ++i) {
        uint8_t* s = raw.data() + size_t(i) * 2352;
        std::memset(s + 1, 0xFF, 10);
        const uint32_t address = first + i + 150;
        s[12] = to_bcd(address / 75 / 60), s[13] = to_bcd(address / 75 % 60), s[14] = to_bcd(address % 75);
        s[15] = 1;
        if (data) std::memcpy(s + 16, data + size_t(i) * 2048, 2048);
    }
    return raw;
}

void write_file(const std::filesystem::path& p, std::span<const uint8_t> bytes) {
    std::ofstream f(p, std::ios::binary);
    f.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    CHECK(f.good());
}

void write_file(const std::filesystem::path& p, std::string_view text) {
    write_file(p, std::span(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
}

constexpr std::string_view kCue =
    "REM SINGLE-DENSITY AREA\r\n"
    "FILE \"disc (Track 1).bin\" BINARY\r\n  TRACK 01 MODE1/2352\r\n    INDEX 01 00:00:00\r\n"
    "FILE \"disc (Track 2).bin\" BINARY\r\n  TRACK 02 AUDIO\r\n    INDEX 00 00:00:00\r\n    INDEX 01 00:02:00\r\n"
    "REM HIGH-DENSITY AREA\r\n"
    "FILE \"disc (Track 3).bin\" BINARY\r\n  TRACK 03 MODE1/2352\r\n    INDEX 01 00:00:00\r\n"
    "FILE \"disc (Track 4).bin\" BINARY\r\n  TRACK 04 AUDIO\r\n    INDEX 00 00:00:00\r\n    INDEX 01 00:02:00\r\n"
    "FILE \"disc (Track 5).bin\" BINARY\r\n  TRACK 05 MODE1/2352\r\n    INDEX 00 00:00:00\r\n    INDEX 01 00:03:00\r\n";

void check_disc_contents(const GdRom& disc, const SyntheticDisc& s) {
    const auto ip_bin = disc.system_area();
    CHECK(ip_bin.has_value());
    if (ip_bin) {
        CHECK(ip_bin->size() == kIpBinSize);
        const auto ip = IpBin::parse(*ip_bin);
        CHECK(ip && ip->boot_file == "1ST_READ.BIN");
    }
    const auto files = disc.files();
    CHECK(files.has_value());
    if (!files) {
        std::println(stderr, "  {}", files.error());
        return;
    }
    CHECK(files->size() == 3);
    if (files->size() != 3) return;
    CHECK((*files)[0].path == "1ST_READ.BIN" && (*files)[0].sector == SyntheticDisc::kBoot);
    CHECK((*files)[1].path == "DATA/DEEP.DAT" && (*files)[1].size == 2048);
    CHECK((*files)[2].path == "README" && (*files)[2].size == 5);
    CHECK(disc.read_file((*files)[0]) == s.boot);
    CHECK(disc.read_file((*files)[1]) == s.deep);
    CHECK(disc.read_file((*files)[2]) == s.readme);
}

void test_gdrom() {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "qilin_msr_tests";
    fs::remove_all(dir);
    fs::create_directories(dir);
    SyntheticDisc s;
    const uint8_t* t3 = s.at(45000);
    const uint8_t* t5 = s.at(45050);

    // ---- Redump CUE, raw sectors ----
    write_file(dir / "disc (Track 1).bin", raw_track(nullptr, 0, 4));
    write_file(dir / "disc (Track 2).bin", std::vector<uint8_t>(size_t(6) * 2352, 0x11));
    write_file(dir / "disc (Track 3).bin", raw_track(t3, 45000, SyntheticDisc::kTrack3Sectors));
    write_file(dir / "disc (Track 4).bin", std::vector<uint8_t>(size_t(SyntheticDisc::kTrack4Sectors) * 2352, 0x22));
    write_file(dir / "disc (Track 5).bin", raw_track(t5, 45050, SyntheticDisc::kTrack5Sectors));
    write_file(dir / "disc.cue", kCue);
    {
        const auto disc = GdRom::open(dir / "disc.cue");
        CHECK(disc.has_value());
        if (disc) {
            CHECK(disc->tracks.size() == 5);
            CHECK(disc->tracks[0].start == 0 && disc->tracks[1].start == 4 && !disc->tracks[1].data);
            CHECK(disc->tracks[2].start == 45000 && disc->tracks[2].high_density && disc->tracks[2].data);
            CHECK(disc->tracks[3].start == 45040 && !disc->tracks[3].data);
            CHECK(disc->tracks[4].start == 45050 && disc->tracks[4].sectors == 20);
            check_disc_contents(*disc, s);
            const auto files = disc->files();
            if (files) CHECK(to_json(*disc, *IpBin::parse(*disc->system_area()), *files).contains("\"track\": 5"));
            // Reads outside data tracks fail, with the reason.
            std::vector<uint8_t> one(2048);
            CHECK(!disc->read(45045, 1, one));   // audio
            CHECK(!disc->read(45070, 1, one));   // past the end
            CHECK(!disc->read(44999, 1, one));   // between the areas
            std::vector<uint8_t> across(size_t(2) * 2048);
            CHECK(!disc->read(45039, 2, across));  // runs into the audio track
            CHECK(disc->read(45038, 2, across).has_value());
        }
    }

    // ---- the same disc as a GDI with cooked data tracks ----
    write_file(dir / "track03.iso", std::span(t3, size_t(SyntheticDisc::kTrack3Sectors) * 2048));
    write_file(dir / "track05.iso", std::span(t5, size_t(SyntheticDisc::kTrack5Sectors) * 2048));
    write_file(dir / "disc.gdi", "5\n"
                                 "1 0 4 2352 \"disc (Track 1).bin\" 0\n"
                                 "2 4 0 2352 \"disc (Track 2).bin\" 0\n"
                                 "3 45000 4 2048 track03.iso 0\n"
                                 "4 45040 0 2352 \"disc (Track 4).bin\" 0\n"
                                 "5 45050 4 2048 track05.iso 0\n");
    {
        const auto disc = GdRom::open(dir / "disc.gdi");
        CHECK(disc.has_value());
        if (disc) {
            CHECK(disc->tracks.size() == 5 && disc->tracks[4].start == 45050 && disc->tracks[4].sector_size == 2048);
            check_disc_contents(*disc, s);
        }
        else std::println(stderr, "  {}", disc.error());
    }

    // ---- images that must be refused ----
    const auto refused = [&](std::string_view name, std::string_view text) {
        write_file(dir / name, text);
        return !GdRom::open(dir / name).has_value();
    };
    std::string cue(kCue);
    const auto without = [&](std::string_view part) {
        std::string c = cue;
        const size_t at = c.find(part);
        CHECK(at != std::string::npos);
        if (at != std::string::npos) c.erase(at, part.size());
        return c;
    };
    cue = without("REM SINGLE-DENSITY AREA\r\n");
    CHECK(refused("no-remarks.cue", without("REM HIGH-DENSITY AREA\r\n")));
    cue = kCue;
    CHECK(refused("no-hd.cue", without("REM HIGH-DENSITY AREA\r\n")));
    CHECK(refused("missing-file.cue", "REM SINGLE-DENSITY AREA\nFILE \"nope.bin\" BINARY\n  TRACK 01 MODE1/2352\n"));
    CHECK(refused("junk.cue", std::string(kCue) + "SONGWRITER \"x\"\n"));
    CHECK(refused("short.gdi", "3\n1 0 4 2352 \"disc (Track 1).bin\" 0\n"));
    CHECK(refused("two-tracks.gdi", "2\n1 0 4 2352 \"disc (Track 1).bin\" 0\n2 4 0 2352 \"disc (Track 2).bin\" 0\n"));
    CHECK(!GdRom::open(dir / "disc.iso").has_value());
    CHECK(!GdRom::open(dir / "absent.cue").has_value());

    // A track file that isn't whole sectors.
    {
        std::vector<uint8_t> t1 = raw_track(nullptr, 0, 4);
        t1.pop_back();
        write_file(dir / "disc (Track 1).bin", t1);
        CHECK(!GdRom::open(dir / "disc.cue").has_value());
        write_file(dir / "disc (Track 1).bin", raw_track(nullptr, 0, 4));
    }
    // A raw sector whose header says it belongs elsewhere: the layout is wrong, so stop.
    {
        write_file(dir / "disc (Track 5).bin", raw_track(t5, 45051, SyntheticDisc::kTrack5Sectors));
        const auto disc = GdRom::open(dir / "disc.cue");
        CHECK(disc.has_value());
        if (disc) {
            const auto files = disc->files();
            CHECK(files.has_value());
            if (files) CHECK(!disc->read_file((*files)[0]).has_value());
        }
        // ... or that isn't Mode 1.
        std::vector<uint8_t> t5raw = raw_track(t5, 45050, SyntheticDisc::kTrack5Sectors);
        t5raw[size_t(5) * 2352 + 15] = 2;
        write_file(dir / "disc (Track 5).bin", t5raw);
        const auto disc2 = GdRom::open(dir / "disc.cue");
        if (disc2) {
            const auto files = disc2->files();
            if (files) CHECK(!disc2->read_file((*files)[0]).has_value());
        }
    }
    // A file name that would escape the output folder when extracted.
    {
        SyntheticDisc evil;
        uint8_t* sub = evil.at(SyntheticDisc::kSub);
        std::memset(sub, 0, 2048);
        SyntheticDisc::record(sub, "../X.DAT;1", SyntheticDisc::kDeep, 2048, false);
        write_file(dir / "disc (Track 3).bin", raw_track(evil.at(45000), 45000, SyntheticDisc::kTrack3Sectors));
        const auto disc = GdRom::open(dir / "disc.cue");
        CHECK(disc.has_value());
        if (disc) CHECK(!disc->files().has_value());
    }
    fs::remove_all(dir);
}

}  // namespace

int main() {
    test_sha256();
    test_decoder();
    test_ip_bin();
    test_boot_file();
    test_cfg();
    test_lifter();
    test_floating_point();
    test_lifter_floating_point();
    test_gdrom();
    if (failures) {
        std::println(stderr, "{} check(s) failed", failures);
        return 1;
    }
    std::println("all tests passed");
    return 0;
}
