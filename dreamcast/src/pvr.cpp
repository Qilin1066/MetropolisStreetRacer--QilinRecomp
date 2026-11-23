// The PowerVR2's and the system bus's registers, 0x005F6800-0x005F9FFF
// (physical). For now a register file: what is written is kept and read
// back, but for the registers that identify the hardware. Registers with
// behaviour (starting a render, the list initialisation, the display, the
// interrupt status, DMA) get it as the bring-up reaches them.
#include "pvr.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "bus.hpp"
#include "guest.hpp"
#include "interrupts.hpp"
#include "maple.hpp"
#include "sh4.hpp"
#include "ta.hpp"
#include "video.hpp"

namespace dreamcast {

namespace rt = qilin::rt;

namespace {

constexpr uint32_t kLo = 0x005F6800u, kHi = 0x005FA000u;
uint32_t g_regs[(kHi - kLo) / 4];

constexpr uint32_t kId = 0x005F8000u;        // PVR: ID, the retail chip's
constexpr uint32_t kRevision = 0x005F8004u;  // PVR: revision, the retail chip's (17)

uint32_t& reg(uint32_t a) { return g_regs[(a - kLo) / 4]; }

// The system bus's interrupt status: normal events (0x005F6900), external
// (0x005F6904) and errors (0x005F6908). The hardware sets their bits; writing
// ones clears them.
constexpr uint32_t kStatusNormal = 0x005F6900u, kStatusExternal = 0x005F6904u, kStatusError = 0x005F6908u;

uint32_t read(uint32_t a, unsigned n) {
    if (n != 4 || (a & 3u)) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    switch (a) {
    case kId: return 0x17FD11DBu;
    case kRevision: return 0x11u;
    default: return reg(a);
    }
}

void vram_block(uint32_t a, const uint8_t* data, unsigned n);
uint32_t vram_read(uint32_t a, unsigned n);

// The tile accelerator's texture path, 0x11000000-0x11FFFFFF (and through
// channel 2's DMA): the lower half goes to video memory by the path LMMODE0
// (0x005F6884) chooses, the upper half by LMMODE1 (0x005F6888); 0 is the
// 64-bit path (0x04000000), 1 the 32-bit one (0x05000000). Kamui 2 sets them.
uint32_t texture_path(uint32_t a) {
    const uint32_t mode = reg((a & 0x00800000u) ? 0x005F6888u : 0x005F6884u) & 1u;
    return (mode ? 0x05000000u : 0x04000000u) | (a & 0x007FFFFFu);
}

// TA_LIST_INIT (0x005F8144): bit 31 starts a new scene's lists.
// STARTRENDER (0x005F8014): renders the scene the lists built. For now the
// renderer (video.cpp) draws the scene and the render ends at once: the
// ISP's, the TSP's and the video's ends (normal events 1, 2 and 0) are raised.
constexpr uint32_t kTaListInit = 0x005F8144u, kStartRender = 0x005F8014u;
void start_render() {
    video::render();
    raise_event(0), raise_event(1), raise_event(2);
}

// Channel 2's DMA, from RAM to the graphics chip: C2DSTAT (0x005F6800) is
// where to, C2DLEN (0x005F6804) how many bytes, and writing 1 to C2DST
// (0x005F6808) starts it, with the SH-4's channel 2 programmed for 32-byte
// blocks from SAR2 (CHCR2 = 0x12C1, DMAOR's DME set). The transfer is done
// at once; then C2DST reads 0, channel 2 shows it ended, and the normal
// event 19 (channel 2's DMA end) is raised.
constexpr uint32_t kC2Dstat = 0x005F6800u, kC2Dlen = 0x005F6804u, kC2Dst = 0x005F6808u;
void dma_to_chip() {
    const Dma2 ch = dma_channel2();
    const uint32_t to = reg(kC2Dstat), bytes = reg(kC2Dlen);
    if (ch.chcr != 0x12C1u || !(ch.dmaor & 1u) || bytes == 0 || bytes % 32 || ch.dmatcr * 32 != bytes)
        throw rt::Stop{rt::Stop::Unsupported, 0, ch.chcr};
    // Where it goes: 0x10000000-0x10FFFFFF the tile accelerator (its
    // parameter stream, then the YUV converter), 0x11000000-0x11FFFFFF
    // texture memory through the 64-bit path, then both again.
    const uint32_t area = to & 0x03800000u;
    const bool to_ta = area == 0, to_texture = (area & 0x03000000u) == 0x01000000u;
    if (!to_ta && !to_texture) throw rt::Stop{rt::Stop::Unsupported, 0, to};
    uint8_t block[32];
    for (uint32_t k = 0; k < bytes; k += 32) {
        for (uint32_t i = 0; i < 32; i += 4) {
            const uint32_t w = rt::ld32(ch.sar + k + i);
            std::memcpy(block + i, &w, 4);
        }
        if (to_ta) ta::feed(block, 32);
        else vram_block(texture_path(to + k), block, 32);
    }
    end_dma_channel2(bytes);
    reg(kC2Dst) = 0;
    raise_event(19);
}

// PVR-DMA, between RAM and video memory (Kamui 2's texture loads): PDSTAP
// (0x005F7C00) the video memory address, PDSTAR (0x005F7C04) the RAM address,
// PDLEN (0x005F7C08) bytes, PDDIR (0x005F7C0C) 0 to video memory and 1 from
// it, PDTSEL (0x005F7C10) 0 for a start by PDST, PDEN (0x005F7C14), and PDST
// (0x005F7C18) starts it. Done at once; then PDST reads 0 and normal event 11
// (PVR-DMA end) is raised. Anything else stops.
constexpr uint32_t kPdstap = 0x005F7C00u, kPdstar = 0x005F7C04u, kPdlen = 0x005F7C08u, kPddir = 0x005F7C0Cu,
                   kPdtsel = 0x005F7C10u, kPden = 0x005F7C14u, kPdst = 0x005F7C18u;
void pvr_dma() {
    const uint32_t to = reg(kPdstap), ram = reg(kPdstar), bytes = reg(kPdlen);
    const bool from_vram = reg(kPddir) & 1u;
    if (!(reg(kPden) & 1u) || (reg(kPdtsel) & 1u) || bytes == 0 || bytes % 32 || (to & 0x1C000000u) != 0x04000000u)
        throw rt::Stop{rt::Stop::Unsupported, 0, to};
    if (!rt::in_ram(0xA0000000u | (ram & 0x1FFFFFFFu)) || !rt::in_ram(0xA0000000u | ((ram + bytes - 1) & 0x1FFFFFFFu)))
        throw rt::Stop{rt::Stop::Unsupported, 0, ram};
    uint8_t* mem = rt::g_mem + (0x8C000000u | (ram & 0x00FFFFFFu));
    if (from_vram)
        for (uint32_t k = 0; k < bytes; k += 4) {
            const uint32_t w = vram_read(to + k, 4);
            std::memcpy(mem + k, &w, 4);
        }
    else
        vram_block(to, mem, bytes);
    reg(kPdst) = 0;
    raise_event(11);
}

// G2 DMA, channels 0-3 at 0x005F7800 + 0x20 n: STAG (+0x00) the G2 bus
// address (the sound chip's memory and registers), STAR (+0x04) the RAM
// address, LEN (+0x08) bytes (bit 31: enable cleared at the end), DIR (+0x0C)
// 0 from RAM and 1 to it, TSEL (+0x10) the starts allowed besides ST (Kamui's
// sound library writes 5), EN (+0x14), ST (+0x18) starts it. Only a start by
// ST is done: the hardware starts TSEL allows never come in the port. Done at once, a word at a time through the bus; then ST
// reads 0 and normal event 15 + n (its end) is raised. Anything else stops.
constexpr uint32_t kG2Lo = 0x005F7800u, kG2Hi = 0x005F7880u;
void g2_dma(uint32_t base) {
    const uint32_t g2 = reg(base), ram = reg(base + 4), len = reg(base + 8);
    const uint32_t bytes = len & 0x7FFFFFFFu;
    const bool to_ram = reg(base + 0x0C) & 1u;
    if (!(reg(base + 0x14) & 1u) || bytes == 0 || bytes % 32 || (g2 & 0x1FFFFFFFu) < 0x00700000u ||
        (g2 & 0x1FFFFFFFu) + bytes > 0x01000000u) {
        std::fprintf(stderr, "G2 DMA %u: STAG %08X STAR %08X LEN %08X DIR %u TSEL %u EN %u: a transfer the port doesn't do\n",
                (base - kG2Lo) / 0x20u, g2, ram, len, reg(base + 0x0C), reg(base + 0x10), reg(base + 0x14));
        throw rt::Stop{rt::Stop::Unsupported, 0, g2};
    }
    if (!rt::in_ram(0xA0000000u | (ram & 0x1FFFFFFFu)) || !rt::in_ram(0xA0000000u | ((ram + bytes - 1) & 0x1FFFFFFFu)))
        throw rt::Stop{rt::Stop::Unsupported, 0, ram};
    const uint32_t from = 0xA0000000u | ((to_ram ? g2 : ram) & 0x1FFFFFFFu);
    const uint32_t dest = 0xA0000000u | ((to_ram ? ram : g2) & 0x1FFFFFFFu);
    for (uint32_t k = 0; k < bytes; k += 4) rt::st32(dest + k, rt::ld32(from + k));
    if (len & 0x80000000u) reg(base + 0x14) = 0;
    reg(base + 0x18) = 0;
    raise_event(15 + (base - kG2Lo) / 0x20u);
}

void write(uint32_t a, uint32_t v, unsigned n) {
    if (n != 4 || (a & 3u)) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    if (a >= kG2Lo && a < kG2Hi && (a & 0x1Fu) == 0x18u) {  // a G2 channel's ST
        reg(a) = v & 1u;
        if (v & 1u) g2_dma(a - 0x18u);
        return;
    }
    switch (a) {
    case kPdst:
        reg(a) = v & 1u;
        if (v & 1u) pvr_dma();
        return;
    case kC2Dst:
        reg(a) = v & 1u;
        if (v & 1u) dma_to_chip();
        return;
    case kTaListInit:
        reg(a) = v & 0x7FFFFFFFu;  // reads back without the start bit
        if (v & 0x80000000u) ta::list_init();
        return;
    case 0x005F6C18u:  // MDST: the Maple bus's DMA (maple.cpp)
        reg(a) = v & 1u;
        if (v & 1u) maple::start();
        return;
    case kStartRender:
        reg(a) = v;
        start_render();
        return;
    case kStatusNormal:
    case kStatusExternal:
    case kStatusError: reg(a) &= ~v; return;
    default: reg(a) = v; return;
    }
}

// Video memory: 8 MB as two 4 MB banks. The 32-bit view (0x05000000, and
// 0x07000000) has the banks one after the other; the 64-bit view
// (0x04000000, and 0x06000000), which the texture hardware reads, has them
// interleaved every 4 bytes. Kept in the 32-bit view's order.
uint8_t g_vram[8u << 20];
uint32_t vram_offset(uint32_t a, unsigned n) {
    const uint32_t off = a & 0x007FFFFFu;
    uint32_t linear;
    if (a & 0x01000000u) linear = off;  // the 32-bit view
    else linear = ((off & 4u) << 20) | ((off >> 1) & 0x003FFFFCu) | (off & 3u);  // the 64-bit view
    if ((linear & 0x3FFFFFu) + n > 0x400000u && !(a & 0x01000000u)) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    if (linear + n > sizeof g_vram) throw rt::Stop{rt::Stop::Unsupported, 0, a};
    return linear;
}
uint32_t vram_read(uint32_t a, unsigned n) {
    uint32_t v = 0;
    std::memcpy(&v, g_vram + vram_offset(a, n), n);
    return v;
}
// When each 4 KB page was last written (vram64_generation).
constexpr uint32_t kPageShift = 12;
uint64_t g_page_written[sizeof g_vram >> kPageShift];
uint64_t g_writes = 0;
void vram_write(uint32_t a, uint32_t v, unsigned n) {
    const uint32_t at = vram_offset(a, n);
    std::memcpy(g_vram + at, &v, n);
    g_page_written[at >> kPageShift] = ++g_writes;
}
void vram_block(uint32_t a, const uint8_t* data, unsigned n) {
    ++g_writes;
    for (unsigned k = 0; k < n; k += 4) {
        const uint32_t at = vram_offset(a + k, 4);
        std::memcpy(g_vram + at, data + k, 4);
        g_page_written[at >> kPageShift] = g_writes;
    }
}

// The tile accelerator's texture path (texture_path): written, never read.
uint32_t texture_read(uint32_t a, unsigned) { throw rt::Stop{rt::Stop::Unsupported, 0, a}; }
void texture_write(uint32_t a, uint32_t v, unsigned n) { vram_write(texture_path(a), v, n); }
void texture_block(uint32_t a, const uint8_t* data, unsigned n) { vram_block(texture_path(a), data, n); }

}  // namespace

void map_pvr() {
    rt::map({kLo, kHi, "PowerVR2 and system bus registers", read, write});
    rt::map({0x04000000u, 0x08000000u, "video memory", vram_read, vram_write, vram_block});
    rt::map({0x11000000u, 0x12000000u, "tile accelerator texture path", texture_read, texture_write, texture_block});
}

const uint8_t* vram() { return g_vram; }
// The texture hardware's addresses wrap at the end of the 8 MB: a texture
// declared taller than what is used (a 640-wide stride texture 512 lines
// high near the end of video memory, at the title) reads on from the start,
// as on the console.
void vram64_read(uint32_t offset, uint8_t* out, uint32_t n) {
    if (offset >= sizeof g_vram || n > sizeof g_vram) throw rt::Stop{rt::Stop::Unsupported, 0, offset};
    for (uint32_t k = 0; k < n; ++k) {
        const uint32_t off = (offset + k) & (sizeof g_vram - 1);
        out[k] = g_vram[((off & 4u) << 20) | ((off >> 1) & 0x003FFFFCu) | (off & 3u)];
    }
}
uint64_t vram64_generation(uint32_t offset, uint32_t n) {
    if (offset >= sizeof g_vram || n > sizeof g_vram) throw rt::Stop{rt::Stop::Unsupported, 0, offset};
    if (n > sizeof g_vram - offset) {  // wraps (vram64_read)
        const uint32_t first_part = static_cast<uint32_t>(sizeof g_vram) - offset;
        return std::max(vram64_generation(offset, first_part), vram64_generation(0, n - first_part));
    }
    // The range is half as long in each bank.
    const uint32_t first = (offset >> 1) >> kPageShift, last = ((offset + n + 7u) >> 1) >> kPageShift;
    constexpr uint32_t kBankPages = (4u << 20) >> kPageShift;
    uint64_t newest = 0;
    for (uint32_t p = first; p <= last && p < kBankPages; ++p) newest = std::max({newest, g_page_written[p], g_page_written[kBankPages + p]});
    return newest;
}

uint32_t pvr_register(uint32_t a) { return reg(a & 0x1FFFFFFFu); }
const uint32_t* pvr_palette() { return &reg(0x005F9000u); }
void set_pvr_register(uint32_t a, uint32_t v) { write(a & 0x1FFFFFFFu, v, 4); }
void raise_event(unsigned bit) {
    reg(kStatusNormal) |= 1u << bit;
    interrupt_pending();
}
void raise_external_event(unsigned bit) {
    reg(kStatusExternal) |= 1u << bit;
    interrupt_pending();
}

}  // namespace dreamcast
