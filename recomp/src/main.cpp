// qilin: Metropolis Street Racer static recompiler (work in progress).
//
//   qilin disc    <image.cue|.gdi> [--json out.json]   tracks, IP.BIN's fields and the file list (JSON)
//   qilin extract <image.cue|.gdi> --out <dir>          IP.BIN and every file of the disc, into <dir>
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
#include "cfg.hpp"
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
               "  qilin disc    <image.cue|.gdi> [--json out.json]\n"
               "  qilin extract <image.cue|.gdi> --out <dir>\n"
               "  qilin info    <1ST_READ.BIN> [--ip IP.BIN] [--json out.json]\n"
               "  qilin insns   <1ST_READ.BIN> [-o out.txt]\n"
               "  qilin disasm  <words.bin> [--base addr] [-o out.txt]\n"
               "  qilin profile <1ST_READ.BIN>\n"
               "  qilin cfg     <1ST_READ.BIN> [--json out.json]\n"
               "  qilin coverage <1ST_READ.BIN> [--list]\n"
               "  qilin suspects <1ST_READ.BIN>\n"
               "  qilin trial   <1ST_READ.BIN> --base addr\n"
               "  qilin fpmode  <1ST_READ.BIN> [--list]\n"
               "  qilin lift    <MSR.BIN> --out <dir>\n"
               "  qilin --version\n");
    return 2;
}

struct Args {
    std::string command, input;
    std::optional<std::string> out, json, out_dir, ip;
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
    std::ifstream f(path, std::ios::binary);
    if (!f) throw Failure{std::format("cannot open {}", path)};
    std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    if (f.bad()) throw Failure{std::format("cannot read {}", path)};
    return bytes;
}

void write_text(const std::string& path, const std::string& text) {
    std::ofstream f(path, std::ios::binary);
    if (!(f << text) || !f.flush()) throw Failure{std::format("cannot write {}", path)};
}

// stdout, or the -o file.
class Output {
public:
    explicit Output(const std::optional<std::string>& path) {
        if (!path) return;
        file_ = std::fopen(path->c_str(), "w");
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
    d.disc = must(GdRom::open(path));
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
    const std::filesystem::path dir(*a.out_dir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) throw Failure{std::format("cannot create {}", dir.string())};

    const auto write = [](const std::filesystem::path& p, auto&& fill) {
        std::ofstream f(p, std::ios::binary);
        if (!f) throw Failure{std::format("cannot write {}", p.string())};
        fill(f);
        if (!f.flush()) throw Failure{std::format("cannot write {}", p.string())};
    };
    write(dir / "IP.BIN", [&](std::ofstream& f) {
        f.write(reinterpret_cast<const char*>(d.ip_bin.data()), static_cast<std::streamsize>(d.ip_bin.size()));
    });

    bool boot_file_seen = false;
    uint64_t bytes = 0;
    constexpr uint32_t kChunk = 512;  // sectors a read: 1 MiB
    std::vector<uint8_t> buffer(size_t(kChunk) * kSectorData);
    for (const GdFile& file : d.files) {
        // "IP.BIN" is what the system area is written as; a disc file of that name would be lost.
        if (file.path == "IP.BIN") throw Failure{"the disc has a file named IP.BIN in its root"};
        if (file.path == d.ip.boot_file) boot_file_seen = true;
        const std::filesystem::path target = dir / std::filesystem::path(file.path);
        std::filesystem::create_directories(target.parent_path(), ec);
        if (ec) throw Failure{std::format("cannot create {}", target.parent_path().string())};
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
    std::println("{} files, {} bytes, and IP.BIN written to {}", d.files.size(), bytes, dir.string());
    std::println("boot file: {}", (dir / d.ip.boot_file).string());
    return 0;
}

int cmd_info(const Args& a) {
    const BootFile boot = must(BootFile::load(a.input), a.input);
    std::optional<IpBin> ip;
    if (a.ip) ip = must(IpBin::parse(read_file(*a.ip)), *a.ip);
    const std::string json = to_json(boot, ip ? &*ip : nullptr);
    if (a.json) write_text(*a.json, json);
    else std::print("{}", json);
    return 0;
}

int cmd_insns(const Args& a) {
    const BootFile boot = must(BootFile::load(a.input), a.input);
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
    const BootFile boot = must(BootFile::load(a.input), a.input);
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
    const BootFile boot = must(BootFile::load(a.input), a.input);
    Cfg cfg(boot);
    cfg.run();
    std::print("{}", cfg.summary());
    if (a.json) write_text(*a.json, cfg.to_json());
    return 0;
}

int cmd_coverage(const Args& a) {
    const BootFile boot = must(BootFile::load(a.input), a.input);
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
    const BootFile boot = must(BootFile::load(a.input), a.input);
    Cfg cfg(boot);
    cfg.run();
    std::print("{}", cfg.trial(a.base));
    return 0;
}

// Lifts every function it can to C++ in --out: functions.hpp (declarations),
// lifted_NNN.cpp, stubs.cpp (functions not lifted yet: calling one stops),
// table.cpp (the address -> function table for indirect calls) and
// lift_report.txt (what wasn't lifted, and why).
int cmd_lift(const Args& a) {
    if (!a.out_dir) throw Failure{"lift needs --out <dir>"};
    const BootFile boot = must(BootFile::load(a.input), a.input);
    if (!known_build(boot.sha256)) throw Failure{std::format("{}: not a supported program (SHA-256 {})", a.input, boot.sha256)};
    Cfg cfg(boot);
    cfg.run();
    const Lifter lifter(cfg);
    const std::filesystem::path dir(*a.out_dir);
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    if (ec) throw Failure{std::format("cannot create {}", dir.string())};
    // Stale chunks from an earlier run would be compiled too.
    for (const auto& entry : std::filesystem::directory_iterator(dir))
        if (entry.path().filename().string().starts_with("lifted_")) std::filesystem::remove(entry.path());

    constexpr size_t kPerFile = 250;
    constexpr std::string_view kHead = "// Generated by qilin lift. Derived from the game: do not commit.\n";
    std::string decls = std::format("{}#pragma once\n#include \"guest.hpp\"\nnamespace qilin::gen {{\n", kHead);
    std::string table = std::format("{}#include \"functions.hpp\"\nnamespace qilin::rt {{\n"
                                    "extern const FnEntry g_functions[] = {{\n", kHead);
    std::string stubs = std::format("{}#include \"functions.hpp\"\nnamespace qilin::gen {{\n", kHead);
    std::string chunk, report;
    std::set<std::pair<uint32_t, uint32_t>> folded;  // literals compiled as constants
    std::map<std::string, size_t> reasons;
    size_t lifted = 0, in_chunk = 0, chunks = 0, instructions = 0;
    const auto flush = [&] {
        if (chunk.empty()) return;
        write_text((dir / std::format("lifted_{:03}.cpp", chunks++)).string(),
                   std::format("{}#include \"functions.hpp\"\nnamespace qilin::gen {{\n{}}}  // namespace qilin::gen\n", kHead, chunk));
        chunk.clear();
        in_chunk = 0;
    };
    for (const auto& [start, f] : cfg.functions()) {
        const std::string name = fn_name(start);
        decls += std::format("void {}(rt::Context& c);\n", name);
        const LiftResult result = lifter.lift(f);
        table += std::format("    {{0x{:08x}u, gen::{}, {}, {}}},\n", start, name, result.any_mode ? "true" : "false",
                             result.keeps_mode || !result.ok ? "true" : "false");
        if (result.ok) {
            folded.insert(result.folded.begin(), result.folded.end());
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
    write_text((dir / "functions.hpp").string(), decls + "}  // namespace qilin::gen\n");
    write_text((dir / "table.cpp").string(),
               table + std::format("}};\nextern const size_t g_function_count = {};\n}}  // namespace qilin::rt\n",
                                   cfg.functions().size()));
    write_text((dir / "stubs.cpp").string(), stubs + "}  // namespace qilin::gen\n");
    // The literals compiled as constants, sorted: checked builds stop on a store into one.
    std::string lits = std::format("{}#include \"guest.hpp\"\nnamespace qilin::rt {{\nextern const Region g_folded[] = {{\n", kHead);
    for (const auto& [addr, size] : folded) lits += std::format("    {{0x{:08x}u, 0x{:08x}u}},\n", addr, addr + size);
    write_text((dir / "literals.cpp").string(),
               lits + std::format("}};\nextern const size_t g_folded_count = {};\n}}  // namespace qilin::rt\n", folded.size()));
    write_text((dir / "lift_report.txt").string(), report);

    const size_t total = cfg.functions().size();
    std::println("{} of {} functions lifted ({}), {} instructions, into {} files in {}", lifted, total,
                 percent(lifted, total, 1), instructions, chunks, dir.string());
    std::vector<std::pair<std::string, size_t>> sorted(reasons.begin(), reasons.end());
    std::ranges::sort(sorted, [](const auto& x, const auto& y) { return x.second > y.second; });
    for (const auto& [why, n] : sorted) std::println("{:>6}  {}", n, why);
    return 0;
}

int cmd_fpmode(const Args& a) {
    const BootFile boot = must(BootFile::load(a.input), a.input);
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
    const BootFile boot = must(BootFile::load(a.input), a.input);
    Cfg cfg(boot);
    cfg.run();
    for (const Suspect& s : cfg.suspects()) std::println("{:08x} {}", s.start, s.why);
    std::println("{} suspects; rejected candidates: {}", cfg.suspects().size(), cfg.rejected().size());
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
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
