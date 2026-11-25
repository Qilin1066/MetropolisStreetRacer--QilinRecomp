// Runtime code: no std::format.
#include "bios.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>

#include "drive.hpp"
#include "romfont.hpp"
#include "flash.hpp"
#include "guest.hpp"
#include "bus.hpp"
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

// ROMFONT (vector 0x8C0000B4), r1 the function: 0 the font's address, 1 lock it (for the
// caller's use), 2 unlock. The boot ROM's own font is Sega's and can't be shipped. The game draws
// all its dialog text, English included, from its 24x24 JIS X 0208 glyphs (0x8C0B3808, through
// Sega's font library at 0x8C1406B4). The font given is GNU Unifont's glyphs in the same layout
// (romfont.hpp; docs/runtime.md, "The boot ROM's font"). Past the glyphs, the library's tables
// of icons (from offset 0x7E900) are blank, and the first read there says so.
constexpr uint32_t kFontAddress = 0xA0100020u, kFontPhysical = 0x00100020u;
uint32_t font_read(uint32_t a, unsigned n) {
    const uint32_t p = a & 0x1FFFFFFFu;
    if (p >= kFontPhysical && p - kFontPhysical + n <= romfont::kGlyphBytes) {
        const uint8_t* b = romfont::image().data() + (p - kFontPhysical);
        uint32_t v = 0;
        for (unsigned i = 0; i < n; ++i) v |= uint32_t(b[i]) << (8 * i);
        return v;
    }
    static bool said = false;
    if (!said)
        rt::log("msr: the game reads the boot ROM at %08X, outside the font's glyphs (the icons there are blank)", a);
    said = true;
    return 0;
}
void font_write(uint32_t a, uint32_t v, unsigned) { throw rt::Stop{rt::Stop::Unsupported, a, v}; }  // ROM
void rom_font(rt::Context& c) {
    switch (c.r[1]) {
    case 0:
        c.r[0] = kFontAddress;
        return;
    case 1: c.r[0] = 0; return;
    case 2: return;
    default:
        rt::log("msr: ROMFONT with r1 = %u, which the boot ROM doesn't have", c.r[1]);
        throw rt::Stop{rt::Stop::Unsupported, c.pr, c.r[1]};
    }
}

// FLASHROM (vector 0x8C0000B8), r7 the function, over the console's flash memory
// (dreamcast/src/flash.cpp, a file in the data folder): 0 a partition's place and size (r4 the
// partition, two words at r5), 1 read (r4 offset, r5 to, r6 bytes), 2 write (r4 offset, r5
// from, r6 bytes), 3 erase the partition at offset r4. The calls return what the boot ROM's do:
// 0 or the bytes, -1 on a bad argument.
void flash(rt::Context& c) {
    switch (c.r[7]) {
    case 0: {
        uint32_t offset = 0, size = 0;
        if (!dreamcast::flash::info(c.r[4], offset, size)) {
            c.r[0] = static_cast<uint32_t>(-1);
            return;
        }
        rt::st32(c.r[5], offset), rt::st32(c.r[5] + 4, size);
        c.r[0] = 0;
        return;
    }
    case 1: c.r[0] = static_cast<uint32_t>(dreamcast::flash::read(c.r[4], c.r[5], c.r[6])); return;
    case 2: c.r[0] = static_cast<uint32_t>(dreamcast::flash::write(c.r[4], c.r[5], c.r[6])); return;
    case 3: c.r[0] = static_cast<uint32_t>(dreamcast::flash::erase(c.r[4])); return;
    default: unknown("FLASHROM (0x8C0000B8)", c);
    }
}

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
    kSeek = 27,     // {frame address, 0}: move the head there
    kDmaStream = 28,  // {frame address, sectors}: a streaming read (calls 6 and 7)
    kSubcode = 34,  // the sub-channel's data
    kVersion = 40,  // the drive's version string, to the address in the first parameter
};

struct Request {
    uint32_t id = 0, bytes = 0;
};
Request g_last;

// A streaming read (command 28, CD_CMD_DMAREAD_STREAM): sent with {frame
// address, sectors} (the driver's 0x8C12E44A, called by the file system's
// stream reader 0x8C120C72 with a sector count). The data then comes out in
// pieces the driver asks for with call 6, {buffer, bytes} (0x8C12E466), each
// a DMA ending with event 0xA40; call 7 gives the bytes of the piece still to
// come. How the library reads the answers (its stream server 0x8C11F512,
// reader 0x8C120C72 and 0x8C120D50, closer 0x8C120ED6):
// - call 1 answering 3, streaming, makes the server ask call 7, whose word is
//   the stream's data not yet transferred (KallistiOS documents it as the
//   remaining amount): more than 0 marks the stream ready for a piece (its
//   state word 0x4A goes 0), none its end. The reader waits for that word to
//   leave 1 after opening the stream and after each piece it asks for;
// - call 1 answering 1, processing, with the fourth status word 1 is what the
//   closer waits for before it aborts the stream (the drive idle between
//   pieces).
// So call 1 answers 3 once after the stream opens and after each piece, until
// call 7 has been asked; 1 between pieces; 2, complete, once no data is left.
// Abort (call 8) ends it.
struct Stream {
    uint32_t id = 0;
    uint32_t sector = 0;     // the next one to read from the disc
    uint64_t left = 0;       // bytes still to come out of the stream
    uint64_t sent = 0;       // bytes handed out so far
    uint8_t buffer[2048];    // the sector being handed out
    uint32_t in_buffer = 0;  // bytes of it not yet handed out
    bool to_report = false;  // opened, or a piece moved, and call 7 not asked since
};
Stream g_stream;
uint32_t g_next_id = 1;
// The GD-ROM DMA's end (event 0xA40), raised as the DMA is done. Raising it only marks the
// event in the system bus's interrupt status (msr/src/main.cpp): the game takes it at its next
// poll with level 4 unmasked, after the call that started the DMA, as on the console. (It was
// held here until a later GDROM call with level 4 unmasked; the library's stream server makes
// its calls masked, so it never came.)
bool g_dma_end_pending = false;
bool g_dma_end_unreported = false;  // ended, and call 5 (gd_dma_end) not yet told
bool g_in_dma_callback = false;     // running the game's DMA-end callback (gd_dma_end)
void deliver_pending(rt::Context& c) {
    if (!g_dma_end_pending || g_in_dma_callback) return;
    g_dma_end_pending = false;
    g_dma_end_unreported = true;
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
    case kSeek: break;  // the drive seeks instantly
    case kToc: write_toc(c, rt::ld32(c.r[5]), rt::ld32(c.r[5] + 4)); break;
    case kPioRead:
    case kDmaRead:
        r.bytes = read_sectors(c, rt::ld32(c.r[5]), rt::ld32(c.r[5] + 4), rt::ld32(c.r[5] + 8));
        if (command == kDmaRead) g_dma_end_pending = true, deliver_pending(c);
        break;
    case kDmaStream: {
        const uint32_t frame = rt::ld32(c.r[5]), sectors = rt::ld32(c.r[5] + 4);
        if (frame < kFrameOffset || sectors == 0) gd_stop(c, "a stream from before the disc's start, or of nothing", frame);
        // A stream the game leaves unfinished (the file system streams ahead, then seeks
        // elsewhere) ends when the next one starts: the drive takes one streaming read at a time,
        // and a new command ends the one in progress. A later call naming the old one stops.
        g_stream.id = r.id, g_stream.sector = frame - kFrameOffset, g_stream.in_buffer = 0, g_stream.sent = 0;
        g_stream.left = uint64_t(sectors) * 2048;
        g_stream.to_report = true;
        break;
    }
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
    if (id != 0 && id == g_stream.id) {
        const uint32_t sent = static_cast<uint32_t>(g_stream.sent);
        rt::st32(status, 0), rt::st32(status + 4, 0), rt::st32(status + 8, sent), rt::st32(status + 12, 0);
        rt::st32(status + 12, 1);
        c.r[0] = !g_stream.left ? 2 : g_stream.to_report ? 3 : 1;
        if (!g_stream.left) g_stream.id = 0;
        return;
    }
    if (id == 0) {  // no request: nothing active (the stream server asks once its stream has ended)
        rt::st32(status, 0), rt::st32(status + 4, 0), rt::st32(status + 8, 0), rt::st32(status + 12, 0);
        c.r[0] = 0;
        return;
    }
    if (id != g_last.id) gd_stop(c, "the state of a request never sent", id);
    rt::st32(status, 0), rt::st32(status + 4, 0), rt::st32(status + 8, g_last.bytes), rt::st32(status + 12, 0);
    c.r[0] = 2;
}

// Call 6: the next `bytes` of stream r4 to the buffer, {buffer, bytes} at r5, by DMA (ending
// with event 0xA40). Returns 0, started (here: done).
void gd_dma_transfer(rt::Context& c) {
    if (c.r[4] == 0 || c.r[4] != g_stream.id) gd_stop(c, "a transfer from a stream that isn't open", c.r[4]);
    uint32_t to = (rt::ld32(c.r[5]) & 0x1FFFFFFFu) | 0x80000000u;  // DMA addresses are physical
    uint32_t bytes = rt::ld32(c.r[5] + 4);
    if ((bytes & 3u) || (to & 3u)) gd_stop(c, "a stream transfer of an odd size or to an odd address", bytes);
    if (bytes > g_stream.left) {
        // The game's loader asks for the next piece from its DMA-end callback without looking at
        // what is left (0x8C02E114 -> 0x8C120D50), so after the stream's last piece it asks once
        // more. The request fails (-1; the library makes that -24) and the loader sees the end
        // through call 7.
        c.r[0] = static_cast<uint32_t>(-1);
        return;
    }
    while (bytes) {
        if (g_stream.in_buffer == 0) {
            if (!g_drive->read(g_stream.sector++, g_stream.buffer)) rt::fatal("GD-ROM: %s", g_drive->error().c_str());
            g_stream.in_buffer = 2048;
        }
        const uint32_t n = std::min(bytes, g_stream.in_buffer);
        const uint8_t* from = g_stream.buffer + (2048 - g_stream.in_buffer);
        for (uint32_t i = 0; i < n; i += 4) {
            uint32_t w;
            std::memcpy(&w, from + i, 4);
            rt::st32(to + i, w);
        }
        to += n, bytes -= n, g_stream.in_buffer -= n, g_stream.left -= n, g_stream.sent += n;
    }
    g_dma_end_pending = true;
    deliver_pending(c);
    g_stream.to_report = true;
    c.r[0] = 0;
}

// Call 7: the bytes of stream r4 not yet transferred, to the word at r5 (each piece moves at
// once, so none of it is in flight).
void gd_dma_check(rt::Context& c) {
    if (c.r[4] == 0 || c.r[4] != g_stream.id) gd_stop(c, "the progress of a stream that isn't open", c.r[4]);
    deliver_pending(c);
    rt::st32(c.r[5], static_cast<uint32_t>(g_stream.left));
    g_stream.to_report = false;
    c.r[0] = 0;
}

// Call 8: abandons request r4: a stream ends; anything else is already done.
void gd_abort(rt::Context& c) {
    deliver_pending(c);
    if (c.r[4] != 0 && c.r[4] == g_stream.id) g_stream = Stream{};
    c.r[0] = 0;
}

// Call 5 (Shinobi's gdGdcG1DmaEnd; KallistiOS's syscall_gdrom_dma_callback): the game's handler
// for the GD-ROM DMA's end (event 0xA40) reports it, with a function r4 and its argument r5; the
// system software then calls r4(r5). That callback is how the game's loader learns a stream's
// piece has arrived (0x8C02E114, setting the flag 0x8C02DD28 waits for). Called once for each
// DMA that has ended; other calls (the handler runs for other events too) do nothing.
void gd_dma_end(rt::Context& c) {
    const uint32_t fn = c.r[4], arg = c.r[5];
    c.r[0] = 0;
    if (!g_dma_end_unreported) return;
    g_dma_end_unreported = false;  // reported, with a function to call or none
    if (fn == 0) return;
    const uint32_t r4 = c.r[4], r5 = c.r[5];
    c.r[4] = arg;
    // The callback asks for the next piece (0x8C02E114 -> 0x8C120D50); that DMA ends after the
    // callback, as on the console (a piece takes time), so the handler takes its end on its next
    // pass and the pieces follow one another instead of nesting.
    g_in_dma_callback = true;
    try {
        rt::call_guest(c, fn);
    } catch (...) {
        g_in_dma_callback = false;
        throw;
    }
    g_in_dma_callback = false;
    deliver_pending(c);
    c.r[4] = r4, c.r[5] = r5, c.r[0] = 0;
}

// Call 4: the drive's state into r4's two words: 1 (paused: a disc in, idle) and 0x80 (a GD-ROM).
void gd_check_drive(rt::Context& c) {
    rt::st32(c.r[4], 1), rt::st32(c.r[4] + 4, 0x80);
    c.r[0] = 0;
}

void gdrom_call(rt::Context& c);

// QILIN_TRACE_GDROM=1: every GDROM call, with its arguments and result, from the caller. For
// finding what the game waits for. The same call repeated with the same result is counted, not
// logged again.
void gdrom(rt::Context& c) {
    static const bool trace = [] {
        const char* t = std::getenv("QILIN_TRACE_GDROM");
        return t && *t && *t != '0';
    }();
    if (!trace) return gdrom_call(c);
    const uint32_t r4 = c.r[4], r5 = c.r[5], r7 = c.r[7], from = c.pr;
    const uint32_t w0 = r5 && rt::in_ram(r5) ? rt::ld32(r5) : 0, w1 = r5 && rt::in_ram(r5) ? rt::ld32(r5 + 4) : 0;
    gdrom_call(c);
    static uint32_t last[5] = {}, repeats = 0;
    const uint32_t now[5] = {r7, r4, w0, c.r[0], from};
    if (std::equal(now, now + 5, last)) {
        ++repeats;
        return;
    }
    static uint32_t lines = 0;
    if (++lines > 20000) return;  // enough to see what the game waits for, without filling the disc
    if (repeats) rt::log("msr: GDROM:   (the call before, %u times more)", repeats);
    repeats = 0;
    std::copy(now, now + 5, last);
    rt::log("msr: GDROM: call %u r4 %08X @r5 {%08X %08X} -> %08X, from %08X", r7, r4, w0, w1, c.r[0], from);
}

void gdrom_call(rt::Context& c) {
    if (static_cast<int32_t>(c.r[6]) == -1) unknown("MISC (0x8C0000BC, r6 = -1)", c);
    if (c.r[6] != 0) unknown("GDROM (0x8C0000BC) with r6 other than 0 or -1", c);
    if (!g_drive) rt::fatal("GD-ROM: no disc");
    switch (c.r[7]) {
    case 0: return gd_send_command(c);
    case 1: return gd_check_command(c);
    case 2: return deliver_pending(c);  // the main loop: commands are done as sent
    case 3: return;                     // initialise: nothing queued after it
    case 4: return gd_check_drive(c);
    case 5: return gd_dma_end(c);
    case 6: return gd_dma_transfer(c);
    case 7: return gd_dma_check(c);
    case 8: return gd_abort(c);
    case 9: deliver_pending(c), g_stream = Stream{}, c.r[0] = 0; return;  // reset: a stream ends
    default: unknown("GDROM (0x8C0000BC)", c);
    }
}
// SYSTEM (vector 0x8C0000E0): the console's reset and its own menus, r4 saying which. The game
// leaves through it (the soft reset, A + B + X + Y + Start, or its exit): the program ends.
void system(rt::Context& c) {
    rt::log("msr: the game hands back to the console (SYSTEM, r4 = %d): exiting", static_cast<int>(c.r[4]));
    std::exit(0);
}

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

void restart_bios() {
    g_dma_end_unreported = false;
    g_last = Request{};
    g_stream = Stream{};
    g_dma_end_pending = false;
}

void install_bios(Drive& drive) {
    g_drive = &drive;
    rt::provide(kCalls);
    // The font's part of the boot ROM (its 2 MB at 0x00000000; the font about 0.5 MB from 0x100020).
    rt::map({0x00100000u, 0x00200000u, "boot ROM font (Unifont)", font_read, font_write});
    set_vector(0x8C0000B0u, kSysinfo);
    set_vector(0x8C0000B4u, kRomFont);
    set_vector(0x8C0000B8u, kFlash);
    set_vector(0x8C0000BCu, kGdrom);
    set_vector(0x8C0000E0u, kSystem);
}

}  // namespace msr
