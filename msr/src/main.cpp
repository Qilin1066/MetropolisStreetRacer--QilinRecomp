// msr: Metropolis Street Racer, recompiled. The program players run.
//
// Bring-up stage (docs/runtime.md): it loads the game from the player's disc
// image into guest RAM as the console's boot ROM and the disc's launcher
// would, runs it, and stops with a report at the first thing the port
// doesn't provide yet.
//
// Runtime code: no std::format.
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "bios.hpp"
#include "drive.hpp"
#include "game.hpp"
#include "gdrom.hpp"
#include "guest.hpp"
#include "memory.hpp"
#include "onchip.hpp"
#include "interrupts.hpp"
#include "pvr.hpp"
#include "rtc.hpp"
#include "sh4.hpp"
#include "ta.hpp"
#include "sha256.hpp"
#include "system.hpp"

namespace rt = qilin::rt;

namespace {

constexpr uint32_t kIpBin = 0x8C008000u;     // where the boot ROM loads IP.BIN
constexpr uint32_t kProgram = 0x8C010000u;   // where the game is loaded and entered
constexpr uint32_t kStackTop = 0x8C00F400u;  // r15 on entry: start-up paints the stack below it

std::optional<std::vector<uint8_t>> read_whole(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

struct Disc {
    std::optional<qilin::GdRom> image;  // the player's disc image, when that's what was given
    std::unique_ptr<msr::Drive> drive;
    std::vector<uint8_t> program, ip_bin;
};

// The game and IP.BIN from the player's disc image (.cue or .gdi), or, for
// development, from a folder of the extracted disc (served as a disc too).
Disc read_disc(const std::filesystem::path& path) {
    Disc d;
    std::error_code ec;
    if (std::filesystem::is_directory(path, ec)) {
        auto program = read_whole(path / "MSR.BIN");
        if (!program) rt::fatal("%s: no MSR.BIN in this folder", path.string().c_str());
        d.program = std::move(*program);
        if (auto ip = read_whole(path / "IP.BIN")) d.ip_bin = std::move(*ip);
        std::string why;
        d.drive = msr::Drive::of_folder(path, why);
        if (!d.drive) rt::fatal("%s: %s", path.string().c_str(), why.c_str());
        return d;
    }
    auto disc = qilin::GdRom::open(path);
    if (!disc) rt::fatal("%s: %s", path.string().c_str(), disc.error().c_str());
    d.image = std::move(*disc);
    auto files = d.image->files();
    if (!files) rt::fatal("%s: %s", path.string().c_str(), files.error().c_str());
    for (const qilin::GdFile& f : *files)
        if (f.path == "MSR.BIN") {
            auto bytes = d.image->read_file(f);
            if (!bytes) rt::fatal("%s: MSR.BIN: %s", path.string().c_str(), bytes.error().c_str());
            d.program = std::move(*bytes);
        }
    if (d.program.empty()) rt::fatal("%s: no MSR.BIN on this disc (is it Metropolis Street Racer?)", path.string().c_str());
    auto ip = d.image->system_area();
    if (!ip) rt::fatal("%s: IP.BIN: %s", path.string().c_str(), ip.error().c_str());
    d.ip_bin = std::move(*ip);
    d.drive = msr::Drive::of_image(*d.image);
    return d;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: msr <disc image (.cue or .gdi)>\n");
        return 1;
    }
    Disc disc = read_disc(argv[1]);
    // The game this build was lifted from, and nothing else: lifted code is that file's.
    const std::string sha = qilin::sha256_hex(disc.program);
    if (sha != rt::g_lifted_sha256)
        rt::fatal("this disc's MSR.BIN (SHA-256 %s) isn't the one this build was made from (%s)", sha.c_str(),
                  rt::g_lifted_sha256);
    if (disc.program.size() > 0x8D000000u - kProgram) rt::fatal("MSR.BIN is larger than RAM");

    rt::GuestMemory memory;
    if (!memory.ok()) rt::fatal("can't map the guest's memory: %s", memory.error());
    rt::g_mem = memory.base();
    std::memcpy(rt::g_mem + kProgram, disc.program.data(), disc.program.size());
    if (!disc.ip_bin.empty()) std::memcpy(rt::g_mem + kIpBin, disc.ip_bin.data(), std::min<size_t>(disc.ip_bin.size(), 0x8000u));

    msr::map_onchip();
    dreamcast::map_pvr();
    dreamcast::ta::map();
    dreamcast::map_sh4_peripherals();
    msr::map_rtc();
    msr::install_bios(*disc.drive);
    // A GD-ROM DMA's end is the system bus's normal event 14 (0xA40), delivered at the next poll.
    msr::set_bus_event([](rt::Context&, uint32_t event) { dreamcast::raise_event((event - 0xA00u) / 0x20u + 12u); });

    dreamcast::start_interrupts();

    rt::g_guest_threads = true;
    rt::Context c;
    c.r[15] = kStackTop;
    c.fpscr = 0x00040000u;  // DN = 1: denormals flushed, as the console starts
    c.vbr = 0x8C000000u;    // the boot ROM's: start-up copies its exception code there (0x8C130918)
    try {
        rt::call_guest(c, kProgram);
    } catch (const rt::Stop& s) {
        rt::report(stderr, s, &c);
        rt::backtrace(stderr, c, kStackTop);
        return 2;
    }
    std::fprintf(stderr, "msr: the game returned to the boot ROM\n");
    return 0;
}
