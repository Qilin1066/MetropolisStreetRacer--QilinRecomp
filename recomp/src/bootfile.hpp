// The two things a Dreamcast disc boots from: IP.BIN and the boot file.
//
// IP.BIN is the disc's system area: the first 16 sectors (32 KiB) of the
// high-density area. The console's boot ROM loads it at 0x8C008000. Its
// first 256 bytes are text fields describing the disc ("meta information"),
// one of which names the boot file. The rest is Sega's bootstrap code.
//
// The boot file (1ST_READ.BIN on almost every disc) is the game's program: a
// flat image with no header and no section table, loaded at 0x8C010000 and
// entered at its first byte. Code, literal pools and data are mixed; nothing
// in the file says which is which. On a GD-ROM it is stored as it is loaded
// (only discs made to boot from CD-R scramble it).
//
// Runtime code: no std::format (see CMakeLists.txt).
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace qilin {

// The Dreamcast's 16 MiB of RAM, as programs address it (the cached P1 view).
inline constexpr uint32_t kRamBase = 0x8C000000u;
inline constexpr uint32_t kRamSize = 16u << 20;
inline constexpr uint32_t kIpBinAddress = 0x8C008000u;
inline constexpr uint32_t kIpBinSize = 0x8000u;
inline constexpr uint32_t kBootFileAddress = 0x8C010000u;

// IP.BIN's meta information. Fields are fixed-width ASCII, padded with
// spaces; trailing spaces are removed here.
struct IpBin {
    std::string hardware_id;     // "SEGA SEGAKATANA"
    std::string maker_id;        // "SEGA ENTERPRISES"
    std::string device_info;     // checksum, medium and disc number: "1234 GD-ROM1/1"
    std::string area_symbols;    // regions the disc runs in: 'J', 'U', 'E'
    std::string peripherals;     // hexadecimal bit field, as written
    std::string product_number;  // "MK-51012"
    std::string version;         // "V1.000"
    std::string release_date;    // "YYYYMMDD"
    std::string boot_file;       // "1ST_READ.BIN"
    std::string publisher;
    std::string title;

    // Bit 0 of the peripherals field: the game runs on Windows CE. That is a
    // different system layer from Sega's Katana libraries, so it matters here.
    bool uses_windows_ce = false;
    // Bit 4: the game supports a VGA box.
    bool supports_vga = false;

    // `data` is IP.BIN, or at least its first 256 bytes.
    static std::expected<IpBin, std::string> parse(std::span<const uint8_t> data);
};

// Programs this port supports, recognised by SHA-256. The disc holds more
// than one: the boot file is a launcher, and the game is a second flat image
// it loads over itself at the same address (docs/disc-and-boot-analysis.md).
struct KnownBuild {
    std::string_view sha256;
    std::string_view file;            // its name on the disc
    std::string_view product_number;
    std::string_view version;
    std::string_view title;           // the disc's title and what this program is
    uint32_t setjmp = 0;              // its C library's setjmp, if known (0: none)
};
const KnownBuild* known_build(std::string_view sha256_hex);

// Thrown only for programming errors: reading outside the image.
struct BootFileError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class BootFile {
public:
    // Errors (unreadable file, empty, larger than RAM) come back as a
    // message, never a crash.
    static std::expected<BootFile, std::string> load(const std::filesystem::path& path);
    static std::expected<BootFile, std::string> parse(std::span<const uint8_t> file);

    std::string sha256;  // of the whole file, lower-case hex

    uint32_t base() const { return kBootFileAddress; }
    uint32_t entry() const { return kBootFileAddress; }
    uint32_t end() const { return base() + static_cast<uint32_t>(image_.size()); }
    std::span<const uint8_t> image() const { return image_; }
    bool mapped(uint32_t a, uint32_t n = 1) const { return a >= base() && uint64_t(a) + n <= end(); }

    // Little-endian reads at guest addresses.
    uint8_t u8(uint32_t a) const;
    uint16_t u16(uint32_t a) const;
    uint32_t u32(uint32_t a) const;

private:
    std::vector<uint8_t> image_;
};

inline uint16_t read_le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
inline uint32_t read_le32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}

}  // namespace qilin
