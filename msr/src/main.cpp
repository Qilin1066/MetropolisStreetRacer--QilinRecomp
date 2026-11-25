// msr: Metropolis Street Racer, recompiled. The program players run.
//
// Bring-up stage (docs/runtime.md): it loads the game from the player's disc
// image into guest RAM as the console's boot ROM and the disc's launcher
// would (the launcher is replaced natively: launcher.hpp), runs it, starts it
// afresh each time the game hands back to the launcher, and stops with a
// report at the first thing the port doesn't provide yet.
//
// Runtime code: no std::format.
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aica.hpp"
#include "bios.hpp"
#include "drive.hpp"
#include "game.hpp"
#include "gdrom.hpp"
#include "guest.hpp"
#include "memory.hpp"
#include "onchip.hpp"
#include "interrupts.hpp"
#include "console.hpp"
#include "frame_rate.hpp"
#include "launcher.hpp"
#include "movie.hpp"
#include "pvr.hpp"
#include "rtc.hpp"
#include "save_history.hpp"
#include "sound.hpp"
#include "sh4.hpp"
#include "ta.hpp"
#include "video.hpp"
#include "sha256.hpp"
#include "sound_cpu.hpp"
#include "system.hpp"

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace rt = qilin::rt;

namespace {

constexpr uint32_t kIpBin = 0x8C008000u;     // where the boot ROM loads IP.BIN
constexpr uint32_t kProgram = 0x8C010000u;   // where the game is loaded and entered
constexpr uint32_t kStackTop = 0x8C00F400u;  // r15 on entry: start-up paints the stack below it
constexpr uint32_t kRamEnd = 0x8D000000u;

// The game's image and IP.BIN in RAM, and RAM above the image cleared, as on a first start. Low
// RAM below the image (the boot ROM's vectors and SYSINFO) is the console's and stays.
void load_program(const std::vector<uint8_t>& program, const std::vector<uint8_t>& ip_bin) {
    std::memcpy(rt::g_mem + kProgram, program.data(), program.size());
    std::memset(rt::g_mem + kProgram + program.size(), 0, kRamEnd - kProgram - program.size());
    if (!ip_bin.empty()) std::memcpy(rt::g_mem + kIpBin, ip_bin.data(), std::min<size_t>(ip_bin.size(), 0x8000u));
}

std::optional<std::vector<uint8_t>> read_whole(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return std::nullopt;
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

struct Disc {
    std::optional<qilin::GdRom> image;  // the player's disc image, when that's what was given
    std::unique_ptr<msr::Drive> drive;
    std::vector<uint8_t> program, ip_bin, movie;  // movie: MOVIES/MSR.SFD, the launcher's intro
};

// A file in an extracted disc folder, its path's parts matched without regard to case (the
// disc's names are upper case; a copy's may not be).
std::optional<std::vector<uint8_t>> read_in_folder(const std::filesystem::path& folder, std::initializer_list<std::string_view> parts) {
    std::filesystem::path at = folder;
    for (const std::string_view part : parts) {
        std::error_code ec;
        bool found = false;
        for (const auto& e : std::filesystem::directory_iterator(at, ec)) {
            const std::string name = e.path().filename().string();
            if (std::ranges::equal(name, part, [](char x, char y) { return std::toupper(static_cast<unsigned char>(x)) == y; })) {
                at = e.path(), found = true;
                break;
            }
        }
        if (!found) return std::nullopt;
    }
    return read_whole(at);
}

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
        auto movie = read_in_folder(path, {"MOVIES", "MSR.SFD"});
        if (!movie) rt::fatal("%s: no MOVIES/MSR.SFD in this folder", path.string().c_str());
        d.movie = std::move(*movie);
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
        } else if (f.path == "MOVIES/MSR.SFD") {
            auto bytes = d.image->read_file(f);
            if (!bytes) rt::fatal("%s: MOVIES/MSR.SFD: %s", path.string().c_str(), bytes.error().c_str());
            d.movie = std::move(*bytes);
        }
    if (d.program.empty()) rt::fatal("%s: no MSR.BIN on this disc (is it Metropolis Street Racer?)", path.string().c_str());
    if (d.movie.empty()) rt::fatal("%s: no MOVIES/MSR.SFD on this disc", path.string().c_str());
    auto ip = d.image->system_area();
    if (!ip) rt::fatal("%s: IP.BIN: %s", path.string().c_str(), ip.error().c_str());
    d.ip_bin = std::move(*ip);
    d.drive = msr::Drive::of_image(*d.image);
    return d;
}

// Bring-up: every five seconds of guest time, a line saying where the game is (the instruction
// of its last access outside the CPU's registers) and how many pictures it has shown, so a
// run that shows nothing still says whether the game is running and where.
void progress() {
    static uint64_t blanks = 0;
    if (++blanks % 300 != 0) return;
#ifdef QILIN_BRINGUP
    const uint32_t pc = rt::g_pc;
    std::fprintf(stderr, "msr: %llu s: at %08X (in fn_%08X), %llu pictures shown\n",
                 static_cast<unsigned long long>(blanks / 60), pc, rt::function_containing(pc),
                 static_cast<unsigned long long>(dreamcast::video::pictures_shown()));
#else
    std::fprintf(stderr, "msr: %llu s: %llu pictures shown\n", static_cast<unsigned long long>(blanks / 60),
                 static_cast<unsigned long long>(dreamcast::video::pictures_shown()));
#endif
    std::fflush(stderr);
}

#ifdef _WIN32
// A crash in native code (no stop report): its code and, in bring-up builds, the game's last
// noted instruction, before Windows ends the program.
LONG WINAPI crashed(EXCEPTION_POINTERS* e) {
#ifdef QILIN_BRINGUP
    std::fprintf(stderr, "msr: crashed: exception %08lX at host %p; the game was at %08X\n",
                 e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress, rt::g_pc);
#else
    std::fprintf(stderr, "msr: crashed: exception %08lX at host %p\n", e->ExceptionRecord->ExceptionCode,
                 e->ExceptionRecord->ExceptionAddress);
#endif
    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: msr <disc image (.cue or .gdi), or a folder of the extracted disc>\n");
        return 1;
    }
#ifdef _WIN32
    SetUnhandledExceptionFilter(crashed);
#endif
    dreamcast::set_command_line(argc, argv);  // for the menu's "Restore and restart"
    Disc disc = read_disc(argv[1]);
    // The game this build was lifted from, and nothing else: lifted code is that file's.
    const std::string sha = qilin::sha256_hex(disc.program);
    if (sha != rt::g_lifted_sha256)
        rt::fatal("this disc's MSR.BIN (SHA-256 %s) isn't the one this build was made from (%s)", sha.c_str(),
                  rt::g_lifted_sha256);
    if (disc.program.size() > kRamEnd - kProgram) rt::fatal("MSR.BIN is larger than RAM");

    rt::GuestMemory memory;
    if (!memory.ok()) rt::fatal("can't map the guest's memory: %s", memory.error());
    rt::g_mem = memory.base();
    load_program(disc.program, disc.ip_bin);

    msr::map_onchip();
    dreamcast::map_pvr();
    dreamcast::ta::map();
    dreamcast::map_sh4_peripherals();
    msr::map_rtc();
    msr::sound_cpu::install();  // the driver runs lifted, not through the AUDIO64 stand-in
    dreamcast::map_aica(disc.image ? &*disc.image : nullptr);
    dreamcast::video::start();
    msr::play_intro(std::move(disc.movie));  // the launcher's cold start, before the game's sound starts
    dreamcast::sound::start();
    msr::install_bios(*disc.drive);
    msr::install_launcher();
    msr::install_console_settings();
    msr::install_frame_rate();
    // A GD-ROM DMA's end is the system bus's normal event 14 (0xA40), delivered at the next poll.
    msr::set_bus_event([](rt::Context&, uint32_t event) { dreamcast::raise_event((event - 0xA00u) / 0x20u + 12u); });

    dreamcast::video::on_blank(progress);
    dreamcast::start_interrupts();

    rt::g_guest_threads = true;
    for (;;) {
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
        } catch (const msr::StartGame& start) {
            // The game handed back to the launcher, which starts it again with these arguments.
            rt::end_guest_threads();
            msr::restart_bios();
            load_program(disc.program, disc.ip_bin);
            if (!msr::write_argument_block({rt::g_mem + msr::kArgumentBlock, msr::kArgumentBlockSize}, start.args))
                rt::fatal("msr: the arguments for the game's restart don't fit its argument block");
            continue;
        }
        std::fprintf(stderr, "msr: the game returned to the boot ROM\n");
        return 0;
    }
}
