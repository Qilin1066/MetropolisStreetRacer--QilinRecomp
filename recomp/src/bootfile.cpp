#include "bootfile.hpp"

#include <array>
#include <fstream>
#include <iterator>

#include "sha256.hpp"

namespace qilin {

namespace {

// The North American disc, MK-51012 V1.010, dated 2000-11-30. Hashes
// recorded from the Redump image in v0.3.0; docs/disc-and-boot-analysis.md
// has what was checked.
constexpr std::array<KnownBuild, 2> kKnownBuilds{{
    {"b5515a96ba7e67dc2dfe08b6adf7d99a4d6c95e7e9fb015fe1f70dffa59af0e9", "1ST_READ.BIN", "MK-51012", "V1.010",
     "Metropolis Street Racer (USA): launcher"},
    {"7780f031bc7124dd68e2b411daf5e6fb636589ab4ea9b36427531d95517d9888", "MSR.BIN", "MK-51012", "V1.010",
     "Metropolis Street Racer (USA): game"},
}};

std::unexpected<std::string> fail(std::string message) { return std::unexpected(std::move(message)); }

// A fixed-width field: printable ASCII only, trailing spaces removed.
std::expected<std::string, std::string> field(std::span<const uint8_t> data, size_t offset, size_t size,
                                              std::string_view name) {
    std::string s(reinterpret_cast<const char*>(data.data()) + offset, size);
    for (char c : s)
        if (c < 0x20 || c > 0x7E) return fail("IP.BIN: the " + std::string(name) + " field isn't text");
    while (!s.empty() && s.back() == ' ') s.pop_back();
    return s;
}

}  // namespace

std::expected<IpBin, std::string> IpBin::parse(std::span<const uint8_t> data) {
    if (data.size() < 0x100) return fail("IP.BIN: shorter than its 256 bytes of meta information");
    IpBin ip;
    struct Slot {
        std::string IpBin::* member;
        size_t offset, size;
        std::string_view name;
    };
    static constexpr Slot kSlots[] = {
        {&IpBin::hardware_id, 0x00, 16, "hardware ID"},
        {&IpBin::maker_id, 0x10, 16, "maker ID"},
        {&IpBin::device_info, 0x20, 16, "device information"},
        {&IpBin::area_symbols, 0x30, 8, "area symbols"},
        {&IpBin::peripherals, 0x38, 8, "peripherals"},
        {&IpBin::product_number, 0x40, 10, "product number"},
        {&IpBin::version, 0x4A, 6, "version"},
        {&IpBin::release_date, 0x50, 16, "release date"},
        {&IpBin::boot_file, 0x60, 16, "boot file"},
        {&IpBin::publisher, 0x70, 16, "publisher"},
        {&IpBin::title, 0x80, 128, "title"},
    };
    for (const Slot& slot : kSlots) {
        auto text = field(data, slot.offset, slot.size, slot.name);
        if (!text) return std::unexpected(text.error());
        ip.*slot.member = std::move(*text);
    }
    if (ip.hardware_id != "SEGA SEGAKATANA")
        return fail("IP.BIN: the hardware ID is \"" + ip.hardware_id + "\", not \"SEGA SEGAKATANA\"");
    if (ip.boot_file.empty()) return fail("IP.BIN: no boot file named");

    uint32_t bits = 0;
    if (ip.peripherals.empty()) return fail("IP.BIN: the peripherals field is empty");
    for (char c : ip.peripherals) {
        unsigned digit;
        if (c >= '0' && c <= '9') digit = static_cast<unsigned>(c - '0');
        else if (c >= 'A' && c <= 'F') digit = static_cast<unsigned>(c - 'A') + 10;
        else if (c >= 'a' && c <= 'f') digit = static_cast<unsigned>(c - 'a') + 10;
        else return fail("IP.BIN: the peripherals field \"" + ip.peripherals + "\" isn't hexadecimal");
        bits = bits << 4 | digit;
    }
    ip.uses_windows_ce = bits & 1;
    ip.supports_vga = bits & 0x10;
    return ip;
}

const KnownBuild* known_build(std::string_view sha256_hex) {
    for (const KnownBuild& b : kKnownBuilds)
        if (b.sha256 == sha256_hex) return &b;
    return nullptr;
}

std::expected<BootFile, std::string> BootFile::load(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail("cannot open " + path.string());
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
    if (f.bad()) return fail("cannot read " + path.string());
    return parse(bytes);
}

std::expected<BootFile, std::string> BootFile::parse(std::span<const uint8_t> file) {
    if (file.empty()) return fail("not a boot file: empty");
    if (file.size() > kRamSize - (kBootFileAddress - kRamBase))
        return fail("not a boot file: it doesn't fit in the Dreamcast's RAM above 0x8C010000");
    BootFile boot;
    boot.image_.assign(file.begin(), file.end());
    boot.sha256 = sha256_hex(file);
    return boot;
}

uint8_t BootFile::u8(uint32_t a) const {
    if (!mapped(a)) throw BootFileError("read outside the boot file");
    return image_[a - base()];
}

uint16_t BootFile::u16(uint32_t a) const {
    if (!mapped(a, 2)) throw BootFileError("read outside the boot file");
    return read_le16(image_.data() + (a - base()));
}

uint32_t BootFile::u32(uint32_t a) const {
    if (!mapped(a, 4)) throw BootFileError("read outside the boot file");
    return read_le32(image_.data() + (a - base()));
}

}  // namespace qilin
