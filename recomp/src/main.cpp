// qilin: Metropolis Street Racer static recompiler (work in progress).
//
//   qilin disc    <image.cue|.gdi|.chd> [--json out.json]
//                                                       tracks, IP.BIN's fields and the file list (JSON)
//   qilin extract <image.cue|.gdi|.chd> --out <dir> [--file PATH]...
//                                                       IP.BIN and every file of the disc, into <dir>; with
//                                                       --file, only the files named (the player kit's steps)
//   qilin identify <image.cue|.gdi|.chd> --json [--build <title>]
//                                                       what the disc is, for the players' launcher: one JSON
//                                                       object on stdout (identify.hpp); exit 0 the supported
//                                                       build, 3 another (or an unknown version), 2 unreadable
//   qilin info    <1ST_READ.BIN> [--ip IP.BIN] [--json out.json]
//                                                       SHA-256, supported build, load address (JSON)
//   qilin insns   <1ST_READ.BIN> [-o out.txt]           every word of the boot file, decoded
//                                                       (for the binutils cross-check)
//   qilin disasm  <words.bin> [--base addr] [-o out.txt]
//                                                       a file of little-endian words, decoded (encoding sweep)
//   qilin profile <1ST_READ.BIN>                        instruction profile of the boot file
//   qilin cfg     <1ST_READ.BIN> [--json out.json]      function discovery + control flow, summary
//   qilin coverage <1ST_READ.BIN> [--list]              byte accounting of the code: code / literal / table /
//                                                       padding / unknown (exit 3 if any unknown)
//   qilin suspects <1ST_READ.BIN>                       functions with problems, for review
//   qilin armdis  <file> [--base addr]                  ARM (the sound CPU's) words, decoded: the decoder check
//   qilin armlift <driver> --out <dir>                  the sound CPU's driver (MANATEE.DRV) to C++
//   qilin hardware <1ST_READ.BIN> [--list]               the hardware the code names: literal-pool
//                                                       addresses in each device, by function
//   qilin fpmode  <1ST_READ.BIN> [--list]               floating-point mode (FPSCR.PR/SZ/FR) proof: what is
//                                                       unproven (exit 3 if anything is)
//   qilin lift    <MSR.BIN> --out <dir>                 lifts every function it can to C++ (for the
//                                                       differential test and, later, the game build)
//   qilin trial   <1ST_READ.BIN> --base addr            what exploring addr as a function finds, and
//                                                       whether weak evidence would be enough for it
//
// Listings are one line per word: address, word, text ("8c010000 d101 mov.l 0x8c010008,r1").
// The boot file is a flat image, so `insns` and `profile` cover data as well
// as code until function discovery tells them apart.
// Exit status is non-zero on any error; nothing fails silently.
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <set>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "bootfile.hpp"
#include "command_line.hpp"
#include "identify.hpp"
#include "path.hpp"
#include "cfg.hpp"
#include "arm/decoder.hpp"
#include "arm/lifter.hpp"
#include "sha256.hpp"
#include "decoder.hpp"
#include "gdrom.hpp"
#include "lifter.hpp"
#include "report.hpp"

#ifndef QILIN_VERSION
#define QILIN_VERSION "unknown"
#endif

using namespace qilin;

namespace {

struct Failure {
    std::string message;
};

int usage() {
    std::print(stderr,
               "qilin " QILIN_VERSION "\n"
               "usage:\n"
               "  qilin disc    <image.cue|.gdi|.chd> [--json out.json]\n"
               "  qilin extract <image.cue|.gdi|.chd> --out <dir> [--file PATH]...\n"
               "  qilin identify <image.cue|.gdi|.chd> --json [--build <title>]\n"
               "  qilin info    <1ST_READ.BIN> [--ip IP.BIN] [--json out.json]\n"
               "  qilin insns   <1ST_READ.BIN> [-o out.txt]\n"
               "  qilin disasm  <words.bin> [--base addr] [-o out.txt]\n"
               "  qilin profile <1ST_READ.BIN>\n"
               "  qilin cfg     <1ST_READ.BIN> [--json out.json]\n"
               "  qilin coverage <1ST_READ.BIN> [--list]\n"
               "  qilin suspects <1ST_READ.BIN>\n"
               "  qilin trial   <1ST_READ.BIN> --base addr\n"
               "  qilin armdis  <file> [--base addr]\n"
               "  qilin armlift <driver> --out <dir>\n"
               "  qilin hardware <1ST_READ.BIN> [--list]\n"
               "  qilin fpmode  <1ST_READ.BIN> [--list]\n"
               "  qilin lift    <MSR.BIN> --out <dir>\n"
               "  qilin --version\n");
    return 2;
}

struct Args {
    std::string command, input;
    std::optional<std::string> out, json, out_dir, ip;
    std::vector<std::string> files;  // extract --file: the disc's paths, '/' between folders
    uint32_t base = 0;
    bool list = false;
};

uint32_t parse_u32(std::string_view s) {
    int radix = 10;
    if (s.starts_with("0x") || s.starts_with("0X")) {
        s.remove_prefix(2);
        radix = 16;
    }
    uint32_t v = 0;
    const auto [end, ec] = std::from_chars(s.data(), s.data() + s.size(), v, radix);
    if (ec != std::errc{} || end != s.data() + s.size()) throw Failure{std::format("not a number: {}", s)};
    return v;
}

Args parse_args(std::span<char*> argv) {
    Args a;
    a.command = argv[1];
    for (size_t i = 2; i < argv.size(); ++i) {
        const std::string_view arg = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argv.size()) throw Failure{std::format("{} needs a value", arg)};
            return argv[++i];
        };
        if (arg == "-o") a.out = value();
        else if (arg == "--json") a.json = value();
        else if (arg == "--base") a.base = parse_u32(value());
        else if (arg == "--out") a.out_dir = value();
        else if (arg == "--ip") a.ip = value();
        else if (arg == "--file") a.files.push_back(value());
        else if (arg == "--list") a.list = true;
        else if (arg.starts_with("-")) throw Failure{std::format("unknown option {}", arg)};
        else if (a.input.empty()) a.input = arg;
        else throw Failure{std::format("unexpected argument {}", arg)};
    }
    if (a.input.empty()) throw Failure{"missing input file"};
    return a;
}

// The value of a std::expected, or a Failure carrying its error.
template <typename T>
T must(std::expected<T, std::string> result, const std::string& context = {}) {
    if (!result) throw Failure{context.empty() ? result.error() : std::format("{}: {}", context, result.error())};
    return std::move(*result);
}

std::vector<uint8_t> read_file(const std::string& path) {
    std::ifstream f(native_path(path), std::ios::binary);
    if (!f) throw Failure{std::format("cannot open {}", path)};
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    if (f.bad()) throw Failure{std::format("cannot read {}", path)};
    return bytes;
}

void write_text(const std::string& path, const std::string& text) {
    std::ofstream f(native_path(path), std::ios::binary);
    if (!(f << text) || !f.flush()) throw Failure{std::format("cannot write {}", path)};
}

// stdout, or the -o file.
class Output {
public:
    explicit Output(const std::optional<std::string>& path) {
        if (!path) return;
#ifdef _WIN32
        file_ = _wfopen(native_path(*path).c_str(), L"w");
#else
        file_ = std::fopen(path->c_str(), "w");
#endif
        if (!file_) throw Failure{std::format("cannot write {}", *path)};
    }
    ~Output() {
        if (file_) std::fclose(file_);
    }
    Output(const Output&) = delete;
    Output& operator=(const Output&) = delete;
    std::FILE* get() const { return file_ ? file_ : stdout; }

private:
    std::FILE* file_ = nullptr;
};

void list_words(std::FILE* out, std::span<const uint8_t> bytes, uint32_t base) {
    size_t i = 0;
    for (; i + 2 <= bytes.size(); i += 2) {
        const uint32_t pc = base + static_cast<uint32_t>(i);
        const sh4::Insn in = sh4::decode(read_le16(bytes.data() + i));
        std::println(out, "{:08x} {:04x} {}", pc, in.word, sh4::disasm(in, pc));
    }
    if (i < bytes.size())  // an odd byte at the end: not a word, but not dropped either
        std::println(out, "{:08x} {:02x} .byte 0x{:02x}", base + static_cast<uint32_t>(i), bytes[i], bytes[i]);
}

struct OpenDisc {
    GdRom disc;
    std::vector<uint8_t> ip_bin;
    IpBin ip;
    std::vector<GdFile> files;
};

OpenDisc open_disc(const std::string& path) {
    OpenDisc d;
    d.disc = must(GdRom::open(native_path(path)));
    d.ip_bin = must(d.disc.system_area(), path);
    d.ip = must(IpBin::parse(d.ip_bin), path);
    d.files = must(d.disc.files(), path);
    return d;
}

int cmd_disc(const Args& a) {
    const OpenDisc d = open_disc(a.input);
    const std::string json = to_json(d.disc, d.ip, d.files);
    if (a.json) write_text(*a.json, json);
    else std::print("{}", json);
    return 0;
}

int cmd_extract(const Args& a) {
    if (!a.out_dir) throw Failure{"extract needs --out <dir>"};
    const OpenDisc d = open_disc(a.input);
    const std::filesystem::path dir = native_path(*a.out_dir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) throw Failure{std::format("cannot create {}", utf8_path(dir))};

    const auto write = [](const std::filesystem::path& p, auto&& fill) {
        std::ofstream f(p, std::ios::binary);
        if (!f) throw Failure{std::format("cannot write {}", utf8_path(p))};
        fill(f);
        if (!f.flush()) throw Failure{std::format("cannot write {}", utf8_path(p))};
    };
    // --file: only the files named (each must be on the disc), and not IP.BIN.
    std::vector<GdFile> chosen;
    for (const std::string& want : a.files) {
        const auto it = std::ranges::find(d.files, want, &GdFile::path);
        if (it == d.files.end()) throw Failure{std::format("{} isn't on this disc", want)};
        chosen.push_back(*it);
    }
    const std::vector<GdFile>& files = a.files.empty() ? d.files : chosen;
    if (a.files.empty())
        write(dir / "IP.BIN", [&](std::ofstream& f) {
            f.write(reinterpret_cast<const char*>(d.ip_bin.data()), static_cast<std::streamsize>(d.ip_bin.size()));
        });

    bool boot_file_seen = !a.files.empty();
    uint64_t bytes = 0;
    constexpr uint32_t kChunk = 512;  // sectors a read: 1 MiB
    std::vector<uint8_t> buffer(size_t(kChunk) * kSectorData);
    for (const GdFile& file : files) {
        // "IP.BIN" is what the system area is written as; a disc file of that name would be lost.
        if (file.path == "IP.BIN") throw Failure{"the disc has a file named IP.BIN in its root"};
        if (file.path == d.ip.boot_file) boot_file_seen = true;
        const std::filesystem::path target = dir / std::filesystem::path(file.path);
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) throw Failure{std::format("cannot create {}", utf8_path(target.parent_path()))};
        write(target, [&](std::ofstream& f) {
            uint32_t sector = file.sector;
            for (uint64_t left = file.size; left;) {
                const auto sectors = static_cast<uint32_t>(std::min<uint64_t>(kChunk, (left + kSectorData - 1) / kSectorData));
                const std::span<uint8_t> chunk(buffer.data(), size_t(sectors) * kSectorData);
                if (auto ok = d.disc.read(sector, sectors, chunk); !ok)
                    throw Failure{std::format("{}: {}", file.path, ok.error())};
                const uint64_t n = std::min<uint64_t>(left, chunk.size());
                f.write(reinterpret_cast<const char*>(chunk.data()), static_cast<std::streamsize>(n));
                left -= n;
                sector += sectors;
            }
        });
        bytes += file.size;
    }
    if (!boot_file_seen)
        throw Failure{std::format("IP.BIN names the boot file {}, which isn't in the disc's root", d.ip.boot_file)};
    if (!a.files.empty()) {
        std::println("{} files, {} bytes, written to {}", files.size(), bytes, utf8_path(dir));
        return 0;
    }
    std::println("{} files, {} bytes, and IP.BIN written to {}", d.files.size(), bytes, utf8_path(dir));
    std::println("boot file: {}", utf8_path(dir / d.ip.boot_file));
    return 0;
}

int cmd_info(const Args& a) {
    const BootFile boot = must(BootFile::load(native_path(a.input)), a.input);
    std::optional<IpBin> ip;
    if (a.ip) ip = must(IpBin::parse(read_file(*a.ip)), *a.ip);
    const std::string json = to_json(boot, ip ? &*ip : nullptr);
    if (a.json) write_text(*a.json, json);
    else std::print("{}", json);
    return 0;
}

int cmd_insns(const Args& a) {
    const BootFile boot = must(BootFile::load(native_path(a.input)), a.input);
    Output out(a.out);
    list_words(out.get(), boot.image(), boot.base());
    return 0;
}

int cmd_disasm(const Args& a) {
    const std::vector<uint8_t> bytes = read_file(a.input);
    if (bytes.size() % 2) throw Failure{std::format("{}: size is not a multiple of 2", a.input)};
    Output out(a.out);
    list_words(out.get(), bytes, a.base);
    return 0;
}

int cmd_profile(const Args& a) {
    const BootFile boot = must(BootFile::load(native_path(a.input)), a.input);
    std::map<sh4::Op, uint64_t> counts;
    const std::span<const uint8_t> image = boot.image();
    const uint64_t words = image.size() / 2;
    for (size_t i = 0; i + 2 <= image.size(); i += 2) ++counts[sh4::decode(read_le16(image.data() + i)).op];
    std::vector<std::pair<sh4::Op, uint64_t>> sorted(counts.begin(), counts.end());
    std::ranges::sort(sorted, [](const auto& x, const auto& y) { return x.second > y.second; });
    std::println("{} words in the boot file (code and data alike), {} distinct instruction forms", words,
                 sorted.size() - (counts.contains(sh4::Op::invalid) ? 1 : 0));
    uint64_t floating = 0, privileged = 0, pc_relative = 0, delayed = 0;
    for (const auto& [op, n] : sorted) {
        const sh4::Spec& s = sh4::spec(op);
        if (s.flags & sh4::kFloat) floating += n;
        if (s.flags & sh4::kPrivileged) privileged += n;
        if (s.flags & sh4::kPcRelative) pc_relative += n;
        if (s.flags & sh4::kDelayed) delayed += n;
        // Forms that share a mnemonic are told apart by an example.
        const auto example = static_cast<uint16_t>(s.match | (0x1234 & ~s.mask));
        std::println("{:>10} {:6.3f}%  {}", n, 100.0 * double(n) / double(words),
                     op == sh4::Op::invalid ? std::string("(not an instruction)")
                                            : std::format("{:<8} e.g. {}", s.name,
                                                          sh4::disasm(sh4::decode(example), boot.base())));
    }
    std::println("floating point {}, privileged {}, PC-relative {}, delayed branches {}", floating, privileged,
                 pc_relative, delayed);
    return 0;
}

int cmd_cfg(const Args& a) {
    const BootFile boot = must(BootFile::load(native_path(a.input)), a.input);
    Cfg cfg(boot);
    cfg.run();
    std::print("{}", cfg.summary());
    if (a.json) write_text(*a.json, cfg.to_json());
    return 0;
}

int cmd_coverage(const Args& a) {
    const BootFile boot = must(BootFile::load(native_path(a.input)), a.input);
    Cfg cfg(boot);
    cfg.run();
    const Coverage c = cfg.coverage();
    const uint64_t bytes = c.hi - c.lo;
    std::println("{:08x}-{:08x}: {} bytes: code {} ({}), literal {}, table {}, padding {}, data {} in {} runs, "
                 "dead {}, unknown {} ({}) in {} spans",
                 c.lo, c.hi, bytes, c.code, percent(c.code, bytes, 3), c.literal, c.table, c.padding, c.data,
                 c.data_runs, c.dead, c.unknown, percent(c.unknown, bytes, 3), c.unknown_spans.size());
    std::println("outside it: {} bytes before, {} after (not classified)", c.before, c.after);
    if (a.list)
        for (const auto& [lo, hi] : c.unknown_spans) std::println("  {:08x}-{:08x} ({} bytes)", lo, hi, hi - lo);
    return c.unknown ? 3 : 0;
}

int cmd_trial(const Args& a) {
    const BootFile boot = must(BootFile::load(native_path(a.input)), a.input);
    Cfg cfg(boot);
    cfg.run();
    std::print("{}", cfg.trial(a.base));
    return 0;
}

// Lifts every function it can to C++ in --out: functions.hpp (declarations),
// lifted_NNN.cpp, stubs.cpp (functions not lifted yet: calling one stops),
// table.cpp (the address -> function table for indirect calls) and
// lift_report.txt (what wasn't lifted, and why).
// rt::kFrameSteps (recomp/runtime/guest.hpp): the per-picture steps rt::frame_step keeps.
constexpr size_t rt_frame_steps = 16;

int cmd_lift(const Args& a) {
    if (!a.out_dir) throw Failure{"lift needs --out <dir>"};
    const BootFile boot = must(BootFile::load(native_path(a.input)), a.input);
    const KnownBuild* build = known_build(boot.sha256);
    if (!build) throw Failure{std::format("{}: not a supported program (SHA-256 {})", a.input, boot.sha256)};
    Cfg cfg(boot);
    cfg.run();
    const Lifter lifter(cfg, build->setjmp, build->idle, build->frame_steps);
    for (const uint32_t w : build->wrapped)
        if (!cfg.functions().contains(w)) throw Failure{std::format("the wrapped function {:08x} (KnownBuild::wrapped) isn't a function", w)};
    // Each per-picture step must be an ftrc in some function, and fit rt::frame_step's table.
    if (build->frame_steps.size() > rt_frame_steps)
        throw Failure{std::format("{} per-picture steps (KnownBuild::frame_steps); rt::frame_step keeps {}", build->frame_steps.size(), rt_frame_steps)};
    for (const uint32_t at : build->frame_steps) {
        if (!std::ranges::any_of(cfg.functions(), [&](const auto& e) { return std::ranges::binary_search(e.second.insns, at); }))
            throw Failure{std::format("the per-picture step at {:08x} (KnownBuild::frame_steps) is in no function", at)};
        if (cfg.insn(at).op != sh4::Op::ftrc)
            throw Failure{std::format("the per-picture step at {:08x} (KnownBuild::frame_steps) isn't an ftrc", at)};
    }
    // Each wait loop's head must be in some function (the lifter checks it is a loop's head there).
    for (const uint32_t head : build->idle)
        if (!std::ranges::any_of(cfg.functions(), [&](const auto& e) { return std::ranges::binary_search(e.second.insns, head); }))
            throw Failure{std::format("the wait loop at {:08x} (KnownBuild::idle) is in no function", head)};
    const std::filesystem::path dir = native_path(*a.out_dir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) throw Failure{std::format("cannot create {}", utf8_path(dir))};
    // Stale chunks from an earlier run would be compiled too.
    for (const auto& entry : std::filesystem::directory_iterator(dir))
        if (utf8_path(entry.path().filename()).starts_with("lifted_")) std::filesystem::remove(entry.path());

    constexpr size_t kPerFile = 250;
    constexpr std::string_view kHead = "// Generated by qilin lift. Derived from the game: do not commit.\n";
    std::string decls = std::format("{}#pragma once\n#include \"guest.hpp\"\nnamespace qilin::gen {{\n", kHead);
    std::string table = std::format("{}#include \"functions.hpp\"\nnamespace qilin::rt {{\n"
                                    "extern const FnEntry g_functions[] = {{\n", kHead);
    std::string stubs = std::format("{}#include \"functions.hpp\"\nnamespace qilin::gen {{\n", kHead);
    std::string chunk, report;
    std::set<std::pair<uint32_t, uint32_t>> folded;  // literals compiled as constants
    std::map<uint32_t, std::string> jump_sites;      // setjmp call returns, and the function each is in
    std::map<std::string, size_t> reasons;
    size_t lifted = 0, replaced = 0, in_chunk = 0, chunks = 0, instructions = 0;
    const auto flush = [&] {
        if (chunk.empty()) return;
        write_text(utf8_path(dir / std::format("lifted_{:03}.cpp", chunks++)),
                   std::format("{}#include \"functions.hpp\"\nnamespace qilin::gen {{\n{}}}  // namespace qilin::gen\n", kHead, chunk));
        chunk.clear();
        in_chunk = 0;
    };
    for (const auto& [start, f] : cfg.functions()) {
        const std::string name = fn_name(start);
        decls += std::format("void {}(rt::Context& c);\n", name);
        if (std::ranges::contains(build->replaced, start)) {
            // Native code in the port (KnownBuild::replaced): entered in any mode, leaves it as it was.
            table += std::format("    {{0x{:08x}u, gen::{}, true, true}},\n", start, name);
            chunk += std::format("void {}(rt::Context& c) {{ rt::replaced(c, 0x{:08x}u); }}\n\n", name, start);
            ++replaced;
            if (++in_chunk == kPerFile) flush();
            continue;
        }
        LiftResult result = lifter.lift(f);
        if (std::ranges::contains(build->wrapped, start)) {
            // KnownBuild::wrapped: the lifted body under its own name, the function a call to the wrapper.
            if (!result.ok) throw Failure{std::format("the wrapped function {:08x} isn't lifted: {}", start, result.why)};
            const std::string head = std::format("void {}(rt::Context& c) {{", name);
            const size_t at = result.code.find(head);
            if (at == std::string::npos) throw Failure{std::format("internal: no head for the wrapped function {:08x}", start)};
            result.code.replace(at, head.size(), std::format("void {}_body(rt::Context& c) {{", name));
            result.code += std::format("void {}(rt::Context& c) {{ rt::wrapped(c, 0x{:08x}u, {}_body); }}\n", name, start, name);
            decls += std::format("void {}_body(rt::Context& c);\n", name);
        }
        table += std::format("    {{0x{:08x}u, gen::{}, {}, {}}},\n", start, name, result.any_mode ? "true" : "false",
                             result.keeps_mode || !result.ok ? "true" : "false");
        if (result.ok) {
            folded.insert(result.folded.begin(), result.folded.end());
            for (const uint32_t r : result.jump_sites) jump_sites.emplace(r, name);
            chunk += result.code + "\n";
            ++lifted;
            instructions += f.insns.size();
            if (++in_chunk == kPerFile) flush();
        } else {
            stubs += std::format("void {}(rt::Context&) {{ rt::not_lifted(0x{:08x}u); }}\n", name, start);
            report += std::format("{:08x} at {:08x}: {}\n", start, result.fail_at, result.why);
            // Group "not lifted yet: x" and the like by their text; internal errors by theirs.
            ++reasons[result.why];
        }
    }
    flush();
    write_text(utf8_path(dir / "functions.hpp"), decls + "}  // namespace qilin::gen\n");
    write_text(utf8_path(dir / "table.cpp"),
               table + std::format("}};\nextern const size_t g_function_count = {};\nextern const char g_lifted_sha256[] = \"{}\";\n",
                                   cfg.functions().size(), boot.sha256) +
                   [&] {
                       // Where a longjmp may come back to (sorted), with a placeholder when there are none.
                       std::string s = "extern const JumpSiteEntry g_jump_sites[] = {\n";
                       for (const auto& [r, fn] : jump_sites) s += std::format("    {{0x{:08x}u, gen::{}}},\n", r, fn);
                       if (jump_sites.empty()) s += "    {0u, nullptr},\n";
                       return s + std::format("}};\nextern const size_t g_jump_site_count = {};\n}}  // namespace qilin::rt\n",
                                              jump_sites.size());
                   }());
    write_text(utf8_path(dir / "stubs.cpp"), stubs + "}  // namespace qilin::gen\n");
    // The literals compiled as constants, sorted: checked builds stop on a store into one.
    std::string lits = std::format("{}#include \"guest.hpp\"\n#ifdef QILIN_CHECKED_MEMORY  // checked builds only\nnamespace qilin::rt {{\nextern const Region g_folded[] = {{\n", kHead);
    for (const auto& [addr, size] : folded) lits += std::format("    {{0x{:08x}u, 0x{:08x}u}},\n", addr, addr + size);
    write_text(utf8_path(dir / "literals.cpp"),
               lits + std::format("}};\nextern const size_t g_folded_count = {};\n}}  // namespace qilin::rt\n#endif\n", folded.size()));
    write_text(utf8_path(dir / "lift_report.txt"), report);

    const size_t total = cfg.functions().size();
    std::println("{} of {} functions lifted ({}), {} instructions, into {} files in {}", lifted, total,
                 percent(lifted, total, 1), instructions, chunks, utf8_path(dir));
    if (replaced) std::println("{} replaced by the port's native code", replaced);
    std::vector<std::pair<std::string, size_t>> sorted(reasons.begin(), reasons.end());
    std::ranges::sort(sorted, [](const auto& x, const auto& y) { return x.second > y.second; });
    for (const auto& [why, n] : sorted) std::println("{:>6}  {}", n, why);
    return 0;
}

// The Dreamcast's devices, by physical address (below P4: the low 29 bits,
// whatever the segment) or by P4 address. From the console's and the SH7750's
// published memory maps.
struct HwDevice {
    uint32_t lo, hi;
    const char* name;
};
constexpr HwDevice kHwDevices[] = {
    {0x00000000u, 0x00200000u, "boot ROM"},
    {0x00200000u, 0x00220000u, "flash memory"},
    {0x005F6800u, 0x005F6C00u, "system bus (DMA, interrupt control)"},
    {0x005F6C00u, 0x005F7000u, "Maple bus"},
    {0x005F7000u, 0x005F7400u, "GD-ROM (G1 ATA)"},
    {0x005F7400u, 0x005F7800u, "G1 bus control"},
    {0x005F7800u, 0x005F7C00u, "G2 bus control (DMA)"},
    {0x005F7C00u, 0x005F8000u, "PowerVR interface (TA DMA)"},
    {0x005F8000u, 0x005FA000u, "PowerVR2 core registers"},
    {0x00600000u, 0x00700000u, "modem"},
    {0x00700000u, 0x00710000u, "AICA registers"},
    {0x00710000u, 0x00710010u, "AICA real-time clock"},
    {0x00800000u, 0x00A00000u, "sound RAM"},
    {0x01000000u, 0x04000000u, "expansion (G2)"},
    {0x04000000u, 0x05000000u, "video RAM, 64-bit path"},
    {0x05000000u, 0x06000000u, "video RAM, 32-bit path"},
    {0x10000000u, 0x10800000u, "tile accelerator FIFO"},
    {0x10800000u, 0x11000000u, "TA YUV converter"},
    {0x11000000u, 0x12000000u, "TA direct texture path"},
    {0xE0000000u, 0xE4000000u, "store queues"},
    {0xF0000000u, 0xF8000000u, "cache and TLB arrays"},
    {0xFF000000u, 0xFF200000u, "CCN (cache, MMU, QACR)"},
    {0xFF200000u, 0xFF400000u, "UBC (break controller)"},
    {0xFF800000u, 0xFFA00000u, "BSC (bus state controller)"},
    {0xFFA00000u, 0xFFC00000u, "DMAC"},
    {0xFFC00000u, 0xFFC80000u, "CPG (clock, standby)"},
    {0xFFC80000u, 0xFFD00000u, "SH-4 RTC"},
    {0xFFD00000u, 0xFFD80000u, "INTC"},
    {0xFFD80000u, 0xFFE00000u, "TMU (timers)"},
    {0xFFE00000u, 0xFFE80000u, "SCI"},
    {0xFFE80000u, 0xFFF00000u, "SCIF (serial)"},
};
// A literal that looks like a device address: in P1 or P2 (how the libraries
// name devices), in P4, or a physical address in system space (P0 above the
// boot ROM and below the expansion area). Small numbers and floats aren't.
const HwDevice* hw_device(uint32_t v) {
    const uint32_t segment = v & 0xE0000000u;
    const bool plausible = v >= 0xE0000000u || segment == 0x80000000u || segment == 0xA0000000u ||
                           (segment == 0 && v >= 0x005F0000u && v < 0x00A00000u);
    if (!plausible) return nullptr;
    const uint32_t a = v >= 0xE0000000u ? v : v & 0x1FFFFFFFu;
    for (const HwDevice& d : kHwDevices)
        if (a >= d.lo && a < d.hi) return &d;
    return nullptr;
}

int cmd_hardware(const Args& a) {
    const BootFile boot = must(BootFile::load(native_path(a.input)), a.input);
    Cfg cfg(boot);
    cfg.run();
    // device -> register (literal value) -> functions naming it
    std::map<const HwDevice*, std::map<uint32_t, std::set<uint32_t>>> uses;
    for (const auto& [start, f] : cfg.functions())
        for (const auto& [addr, size] : f.literals)
            if (size == 4)
                if (const uint32_t v = boot.u32(addr); const HwDevice* d = hw_device(v)) uses[d][v].insert(start);
    std::println("devices named by literal-pool addresses in code (a base is often named, the registers by offset):");
    for (const HwDevice& d : kHwDevices) {
        const auto it = uses.find(&d);
        if (it == uses.end()) continue;
        std::set<uint32_t> fns;
        for (const auto& [v, s] : it->second) fns.insert(s.begin(), s.end());
        std::println("  {:<38} {:4} addresses, {:4} functions ({:08x}-{:08x})", d.name, it->second.size(), fns.size(),
                     *fns.begin(), *fns.rbegin());
        if (a.list)
            for (const auto& [v, s] : it->second) {
                std::string list;
                for (const uint32_t fn : s) list += std::format(" {:08x}", fn);
                std::println("      {:08x}:{}", v, list);
            }
    }
    return 0;
}

// Each 32-bit word of a file as the ARM decoder reads it: address, word, the
// mnemonic and the registers it names (tools/check/verify_arm.py compares
// these with binutils).
int cmd_armdis(const Args& a) {
    std::ifstream f(native_path(a.input), std::ios::binary);
    if (!f) throw Failure{std::format("cannot read {}", a.input)};
    const std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(f)), {});
    const uint32_t base = a.base;
    for (size_t k = 0; k + 4 <= bytes.size(); k += 4) {
        const uint32_t w = uint32_t(bytes[k]) | uint32_t(bytes[k + 1]) << 8 | uint32_t(bytes[k + 2]) << 16 | uint32_t(bytes[k + 3]) << 24;
        const uint32_t pc = base + static_cast<uint32_t>(k);
        const arm::Insn in = arm::decode(w);
        static constexpr const char* kCond[16] = {"eq", "ne", "cs", "cc", "mi", "pl", "vs", "vc",
                                                  "hi", "ls", "ge", "lt", "gt", "le", "al", "nv"};
        std::println("{:08x} {:08x} {} {} {} {}{}", pc, w, arm::name(in.op), kCond[in.cond], in.s ? 1 : 0,
                     arm::describe(in, pc), in.armv4 ? " (ARMv4)" : "");
    }
    return 0;
}

// The sound CPU's driver as C++ (docs/sound.md): arm_functions.hpp, arm_lifted_NN.cpp and
// arm_table.cpp (every function, sorted, and the driver's SHA-256), into --out.
int cmd_armlift(const Args& a) {
    if (!a.out_dir) throw Failure{"armlift needs --out <dir>"};
    std::ifstream f(native_path(a.input), std::ios::binary);
    if (!f) throw Failure{std::format("cannot read {}", a.input)};
    const std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), {});
    arm::ArmLifter lifter(arm::Image::of_driver(file));
    lifter.run();
    const std::filesystem::path dir = native_path(*a.out_dir);
    std::filesystem::create_directories(dir);
    for (const auto& entry : std::filesystem::directory_iterator(dir))
        if (utf8_path(entry.path().filename()).starts_with("arm_")) std::filesystem::remove(entry.path());
    constexpr std::string_view kHead = "// Generated by qilin armlift. Derived from the game: do not commit.\n";
    std::string decls = std::format("{}#pragma once\n#include \"arm_guest.hpp\"\nnamespace qilin::armgen {{\nusing arm_rt::Context;\n", kHead);
    std::string table = std::format("{}#include \"arm_functions.hpp\"\nnamespace qilin::arm_rt {{\nextern const FnEntry g_functions[] = {{\n", kHead);
    std::string chunk;
    size_t chunks = 0, in_chunk = 0, bad = 0, insns = 0;
    const auto flush = [&] {
        if (chunk.empty()) return;
        write_text(utf8_path(dir / std::format("arm_lifted_{:02}.cpp", chunks++)),
                   std::format("{}#include \"arm_functions.hpp\"\nnamespace qilin::armgen {{\nnamespace rt = qilin::arm_rt;\n{}}}  // namespace qilin::armgen\n", kHead, chunk));
        chunk.clear();
        in_chunk = 0;
    };
    for (const auto& [start, fn] : lifter.functions()) {
        decls += std::format("void arm_{:06x}(Context& c);\n", start);
        table += std::format("    {{0x{:06x}u, armgen::arm_{:06x}}},\n", start, start);
        chunk += lifter.emit(fn) + "\n";
        bad += fn.bad.size();
        insns += fn.insns.size();
        if (++in_chunk == 100) flush();
    }
    flush();
    write_text(utf8_path(dir / "arm_functions.hpp"), decls + "}  // namespace qilin::armgen\n");
    write_text(utf8_path(dir / "arm_table.cpp"),
               table + std::format("}};\nextern const unsigned g_function_count = {};\nextern const char g_driver_sha256[] = \"{}\";\n}}  // namespace qilin::arm_rt\n",
                                   lifter.functions().size(), sha256_hex(file)));
    std::println("{} functions, {} instructions, {} literal words, {} stops where the code reaches something this core doesn't run, into {}",
                 lifter.functions().size(), insns, lifter.literals().size(), bad, utf8_path(dir));
    return 0;
}

int cmd_fpmode(const Args& a) {
    const BootFile boot = must(BootFile::load(native_path(a.input)), a.input);
    Cfg cfg(boot);
    cfg.run();
    const FpSummary t = cfg.fp_summary();
    std::println("{} functions use floating point; {} change the mode", t.functions, t.changers);
    std::println("{} mode-dependent instructions: {} with the mode unproven, {} in the wrong precision", t.instructions,
                 t.unknown, t.wrong);
    std::println("{} calls and jumps made in a mode other than the entry mode", t.leaks);
    std::println("{} functions with any of these", t.unproven_functions);
    if (a.list)
        for (const auto& [start, f] : cfg.functions()) {
            for (const uint32_t pc : f.fp_unknown)
                std::println("{:08x} mode unproven at {:08x}: {}", start, pc, sh4::disasm(cfg.insn(pc), pc));
            for (const uint32_t pc : f.fp_wrong)
                std::println("{:08x} wrong precision at {:08x}: {}", start, pc, sh4::disasm(cfg.insn(pc), pc));
            for (const uint32_t pc : f.fp_leaks)
                std::println("{:08x} calls out in another mode at {:08x}: {}", start, pc, sh4::disasm(cfg.insn(pc), pc));
            if (f.fp_exit_seen && (f.fp_exit_known != kFpAll || f.fp_exit != cfg.entry_mode())) {
                std::string mode;
                for (const auto& [bit, name] : {std::pair{kFpPr, "PR"}, std::pair{kFpSz, "SZ"}, std::pair{kFpFr, "FR"}})
                    mode += std::format(" {}={}", name, !(f.fp_exit_known & bit) ? "?" : (f.fp_exit & bit) ? "1" : "0");
                std::println("{:08x} returns in another mode:{}", start, mode);
            }
        }
    return t.unproven_functions ? 3 : 0;
}

int cmd_suspects(const Args& a) {
    const BootFile boot = must(BootFile::load(native_path(a.input)), a.input);
    Cfg cfg(boot);
    cfg.run();
    for (const Suspect& s : cfg.suspects()) std::println("{:08x} {}", s.start, s.why);
    std::println("{} suspects; rejected candidates: {}", cfg.suspects().size(), cfg.rejected().size());
    return 0;
}

// The launcher's contract (identify.hpp): JSON whatever happens, and only
// exit codes 0, 3 and 2.
int cmd_identify(int argc, char** argv) {
    std::optional<std::string> path, build;
    bool json = false;
    std::string problem;
    for (int i = 2; i < argc; ++i) {
        const std::string_view a = argv[i];
        if (a == "--json") json = true;
        else if (a == "--build" && i + 1 < argc) build = argv[++i];
        else if (!a.starts_with("-") && !path) path = std::string(a);
        else problem = std::format("identify: unexpected argument {}", a);
    }
    if (!path) problem = "identify takes one disc image";
    if (!json) problem = "identify needs --json";
    Identity id;
    if (!problem.empty()) {
        id.message = problem;
    } else {
        try {
            id = identify(native_path(*path), build.value_or(""));
        } catch (const std::exception& e) {
            id = Identity{};
            id.message = std::format("The recompiler failed: {}.", e.what());
        }
    }
    std::print("{}", to_json(id));
    std::fflush(stdout);
    return id.code;
}

}  // namespace

int main(int argc, char** argv) {
    // Windows: the arguments as UTF-8, from the UTF-16 command line (paths may be any text).
    const qilin::CommandLine command_line(argc, argv);
    argc = command_line.count();
    argv = command_line.values();
    if (argc >= 2 && std::string_view(argv[1]) == "identify") return cmd_identify(argc, argv);
    if (argc == 2 && std::string_view(argv[1]) == "--version") {
        std::println("qilin " QILIN_VERSION);
        return 0;
    }
    if (argc < 3) return usage();
    try {
        const Args a = parse_args(std::span(argv, static_cast<size_t>(argc)));
        if (a.command == "disc") return cmd_disc(a);
        if (a.command == "extract") return cmd_extract(a);
        if (a.command == "info") return cmd_info(a);
        if (a.command == "insns") return cmd_insns(a);
        if (a.command == "disasm") return cmd_disasm(a);
        if (a.command == "profile") return cmd_profile(a);
        if (a.command == "cfg") return cmd_cfg(a);
        if (a.command == "coverage") return cmd_coverage(a);
        if (a.command == "suspects") return cmd_suspects(a);
        if (a.command == "trial") return cmd_trial(a);
        if (a.command == "fpmode") return cmd_fpmode(a);
        if (a.command == "hardware") return cmd_hardware(a);
        if (a.command == "armdis") return cmd_armdis(a);
        if (a.command == "armlift") return cmd_armlift(a);
        if (a.command == "lift") return cmd_lift(a);
        return usage();
    } catch (const Failure& f) {
        std::println(stderr, "qilin: {}", f.message);
        return 1;
    } catch (const std::exception& e) {
        std::println(stderr, "qilin: {}", e.what());
        return 1;
    }
}
