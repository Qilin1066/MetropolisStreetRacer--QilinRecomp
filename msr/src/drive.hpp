// The GD-ROM drive's disc, as the boot ROM's GDROM calls read it (bios.cpp):
// sectors by their absolute number, and the tracks for the table of
// contents. Two kinds:
//  - the player's disc image (.cue or .gdi), read in place: what the game
//    runs from;
//  - for development, a folder of the disc's extracted files (qilin
//    extract's output), served as an ISO 9660 file system built in memory
//    at the high-density area's start, with IP.BIN in its first 16 sectors.
//    The files' sectors differ from the disc's; the game finds its files
//    through the file system, so that doesn't change what it reads. The
//    single-density area and any audio tracks aren't there.
//
// Runtime code: no std::format.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace qilin {
class GdRom;
}

namespace msr {

struct DriveTrack {
    unsigned number;
    bool data;
    bool high_density;
    uint32_t start;    // its first sector (the disc's numbering)
    uint32_t sectors;
    uint32_t end() const { return start + sectors; }
};

class Drive {
public:
    virtual ~Drive() = default;
    // One data sector's 2048 bytes into `out`; false (with `error()`) if the
    // sector isn't a data sector the disc has.
    virtual bool read(uint32_t sector, uint8_t* out) = 0;
    virtual std::vector<DriveTrack> tracks() const = 0;
    // The disc image, for CD audio (sound.cpp), or nullptr.
    virtual const qilin::GdRom* image() const { return nullptr; }
    const std::string& error() const { return error_; }

    static std::unique_ptr<Drive> of_image(const qilin::GdRom& disc);
    // A folder of extracted files: nullptr (with `why`) if it can't be served.
    static std::unique_ptr<Drive> of_folder(const std::filesystem::path& folder, std::string& why);

protected:
    std::string error_;
};

}  // namespace msr
