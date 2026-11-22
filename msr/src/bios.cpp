// Runtime code: no std::format.
#include "bios.hpp"

#include <array>
#include <cstring>

#include "drive.hpp"
#include "guest.hpp"
#include "system.hpp"

namespace msr {

namespace rt = qilin::rt;

namespace {

// Where each vector's calls land: in the boot ROM's area (P1), where no lifted function is.
constexpr uint32_t kSysinfo = 0x80000100u, kRomFont = 0x80000110u, kFlash = 0x80000120u, kGdrom = 0x80000130u,
                   kSystem = 0x80000140u;

[[noreturn]] void unknown(const char* vector, rt::Context& c) {
    rt::log("msr: the game makes a boot ROM call the port doesn't provide: %s, r7 = %d (r4 %08X, r5 %08X, r6 %08X), "
            "returning to %08X",
            vector, static_cast<int>(c.r[7]), c.r[4], c.r[5], c.r[6], c.pr);
    throw rt::Stop{rt::Stop::Unsupported, c.pr, c.r[7]};
}

// SYSINFO (vector 0x8C0000B0): r7 = 0 prepares 0x8C000068-0x8C00007F from the
// console's flash memory; r7 = 3 returns where the console's 8-byte ID is.
// What the game reads there (start-up, 0x8C11A57C): byte 12, the television
// standard as an ASCII digit ('0' NTSC, '1' PAL, '2' PAL-M, '3' PAL-N), which
// it prints as cblBroadcast. This console is an NTSC one, as the disc is
// (North America); its ID is zero.
constexpr uint32_t kSysinfoArea = 0x8C000068u;
void sysinfo(rt::Context& c) {
    switch (c.r[7]) {
    case 0: {
        uint8_t area[24] = {};
        area[12] = '0';  // NTSC
        std::memcpy(rt::g_mem + kSysinfoArea, area, sizeof area);
        c.r[0] = 0;
        return;
    }
    case 3: c.r[0] = kSysinfoArea; return;
    default: unknown("SYSINFO (0x8C0000B0)", c);
    }
}

void rom_font(rt::Context& c) { unknown("ROMFONT (0x8C0000B4)", c); }
void flash(rt::Context& c) { unknown("FLASHROM (0x8C0000B8)", c); }

// ---- GDROM (vector 0x8C0000BC, r6 = 0): the drive, r7 the function ----
// From Toy Commander--QilinRecomp's replacement of the same calls (its
// gdrom_bios.cpp), kept in step: the drive does each command as it is sent,
// so a request is complete when first asked about.
Drive* g_drive = nullptr;
void (*g_raise_bus_event)(rt::Context&, uint32_t) = nullptr;

enum Command : uint32_t {
    kPioRead = 16,  // {frame address, sectors, buffer, 0}: 2048-byte sectors to the buffer
    kDmaRead = 17,  // the same, through the system bus's GD-ROM DMA, which ends with event 0xA40
    kToc = 19,      // {area (0 single density, 1 high density), buffer}: the area's table of contents
    kPlay = 20,     // CD audio by track
    kPlay2 = 21,    // CD audio by frame
    kPause = 22,
    kRelease = 23,
    kInit = 24,     // the drive's initialisation
    kSubcode = 34,  // the sub-channel's data
    kVersion = 40,  // the drive's version string, to the address in the first parameter
};

struct Request {
    uint32_t id = 0, bytes = 0;
};
Request g_last;
uint32_t g_next_id = 1;
// The GD-ROM DMA's end (event 0xA40), raised by a DMA read and delivered at the next call that
// can take it: on the console it comes a moment after the read is sent, never inside that call.
bool g_dma_end_pending = false;
void deliver_pending(rt::Context& c) {
    if (!g_dma_end_pending || ((c.sr >> 4) & 15u) >= 4) return;  // level 4 masked: later
    g_dma_end_pending = false;
    if (!g_raise_bus_event) throw rt::Stop{rt::Stop::Unsupported, c.pr, 0xA40u};
    g_raise_bus_event(c, 0xA40u);
}

// Frame addresses count from the disc's start, 150 before sector 0 (the lead-in).
constexpr uint32_t kFrameOffset = 150;

[[noreturn]] void gd_stop(rt::Context& c, const char* what, uint32_t value) {
    rt::log("msr: GDROM: %s (%08X), returning to %08X", what, value, c.pr);
    throw rt::Stop{rt::Stop::Unsupported, c.pr, value};
}

uint32_t read_sectors(rt::Context& c, uint32_t frame, uint32_t count, uint32_t to) {
    if (frame < kFrameOffset || count == 0) gd_stop(c, "a read from before the disc's start, or of nothing", frame);
    uint8_t data[2048];
    for (uint32_t k = 0; k < count; ++k) {
        if (!g_drive->read(frame - kFrameOffset + k, data)) rt::fatal("GD-ROM: %s", g_drive->error().c_str());
        for (uint32_t i = 0; i < 2048; i += 4) {
            uint32_t w;
            std::memcpy(&w, data + i, 4);
            rt::st32(to + k * 2048 + i, w);
        }
    }
    return count * 2048;
}

// The table of contents of one area: 99 words for tracks 1-99 (control << 28 | 1 << 24 | the
// frame address of the track's start; control 4 for data, 0 for audio; 0xFFFFFFFF for a track not
// in the area), then the first and last tracks (control << 28 | 1 << 24 | number << 16) and the
// lead-out (the last track's control | the frame address after it).
void write_toc(rt::Context& c, uint32_t area, uint32_t to) {
    if (area > 1) gd_stop(c, "a table of contents of an area that doesn't exist", area);
    uint32_t words[102];
    for (uint32_t& w : words) w = 0xFFFFFFFFu;
    const auto tracks = g_drive->tracks();
    const DriveTrack* first = nullptr;
    const DriveTrack* last = nullptr;
    const auto control = [](const DriveTrack& t) { return (t.data ? 4u : 0u) << 28 | 1u << 24; };
    for (const DriveTrack& t : tracks) {
        if (t.high_density != (area == 1) || t.number < 1 || t.number > 99) continue;
        words[t.number - 1] = control(t) | (t.start + kFrameOffset);
        if (!first) first = &t;
        last = &t;
    }
    if (!first) gd_stop(c, "a table of contents of an empty area", area);
    words[99] = control(*first) | first->number << 16;
    words[100] = control(*last) | last->number << 16;
    words[101] = control(*last) | (last->end() + kFrameOffset);
    for (uint32_t k = 0; k < 102; ++k) rt::st32(to + 4 * k, words[k]);
}

void gd_send_command(rt::Context& c) {
    const uint32_t command = c.r[4];
    Request r{g_next_id++, 0};
    switch (command) {
    case kInit: break;
    case kToc: write_toc(c, rt::ld32(c.r[5]), rt::ld32(c.r[5] + 4)); break;
    case kPioRead:
    case kDmaRead:
        r.bytes = read_sectors(c, rt::ld32(c.r[5]), rt::ld32(c.r[5] + 4), rt::ld32(c.r[5] + 8));
        if (command == kDmaRead) g_dma_end_pending = true;
        break;
    case kVersion: {
        static constexpr char kText[] = "GDC Version 1.00";
        const uint32_t to = rt::ld32(c.r[5]);
        for (uint32_t k = 0; k + 1 < sizeof kText; ++k) rt::st8(to + k, static_cast<uint8_t>(kText[k]));
        break;
    }
    case kPlay: case kPlay2: case kPause: case kRelease: case kSubcode:
        gd_stop(c, "a CD audio command, which the port doesn't play yet", command);
    default: gd_stop(c, "a command the port doesn't know", command);
    }
    if (g_next_id == 0) g_next_id = 1;
    g_last = r;
    c.r[0] = r.id;
}

// Call 1: the state of request r4, four words at r5 (error, its detail, bytes transferred, a
// fourth copied with the bytes). Returns 2: complete.
void gd_check_command(rt::Context& c) {
    deliver_pending(c);
    const uint32_t id = c.r[4], status = c.r[5];
    if (id == 0 || id != g_last.id) gd_stop(c, "the state of a request never sent", id);
    rt::st32(status, 0), rt::st32(status + 4, 0), rt::st32(status + 8, g_last.bytes), rt::st32(status + 12, 0);
    c.r[0] = 2;
}

// Call 4: the drive's state into r4's two words: 1 (paused: a disc in, idle) and 0x80 (a GD-ROM).
void gd_check_drive(rt::Context& c) {
    rt::st32(c.r[4], 1), rt::st32(c.r[4] + 4, 0x80);
    c.r[0] = 0;
}

void gdrom(rt::Context& c) {
    if (static_cast<int32_t>(c.r[6]) == -1) unknown("MISC (0x8C0000BC, r6 = -1)", c);
    if (c.r[6] != 0) unknown("GDROM (0x8C0000BC) with r6 other than 0 or -1", c);
    if (!g_drive) rt::fatal("GD-ROM: no disc");
    switch (c.r[7]) {
    case 0: return gd_send_command(c);
    case 1: return gd_check_command(c);
    case 2: return deliver_pending(c);  // the main loop: commands are done as sent
    case 3: return;                     // initialise: nothing queued after it
    case 4: return gd_check_drive(c);
    case 5: c.r[0] = 0; return;         // the DMA's end, reported: nothing to do
    case 8: deliver_pending(c), c.r[0] = 0; return;  // abort: nothing is in progress
    case 9: deliver_pending(c), c.r[0] = 0; return;  // reset: nothing is in progress
    default: unknown("GDROM (0x8C0000BC)", c);
    }
}
void system(rt::Context& c) { unknown("SYSTEM (0x8C0000E0)", c); }

constexpr std::array<rt::Provided, 5> kCalls{{
    {kSysinfo, "boot ROM: SYSINFO", sysinfo},
    {kRomFont, "boot ROM: ROMFONT", rom_font},
    {kFlash, "boot ROM: FLASHROM", flash},
    {kGdrom, "boot ROM: GDROM and MISC", gdrom},
    {kSystem, "boot ROM: SYSTEM", system},
}};

void set_vector(uint32_t vector, uint32_t target) { std::memcpy(rt::g_mem + vector, &target, 4); }

}  // namespace

void set_bus_event(void (*raise)(rt::Context& c, uint32_t event)) { g_raise_bus_event = raise; }

void install_bios(Drive& drive) {
    g_drive = &drive;
    rt::provide(kCalls);
    set_vector(0x8C0000B0u, kSysinfo);
    set_vector(0x8C0000B4u, kRomFont);
    set_vector(0x8C0000B8u, kFlash);
    set_vector(0x8C0000BCu, kGdrom);
    set_vector(0x8C0000E0u, kSystem);
}

}  // namespace msr
