// GD-ROM disc images: tracks, sectors and the ISO 9660 file system.
//
// A GD-ROM has two areas. The single-density area (tracks 1 and 2) is an
// ordinary CD session with a warning track. The high-density area starts at
// sector 45000 and holds the game: a data track, optionally audio tracks,
// and optionally a final data track. Its first 16 sectors are IP.BIN; an
// ISO 9660 file system follows, whose sector numbers count from the start of
// the disc, not of the area.
//
// Two image formats are read:
//  - Redump's CUE sheet with one BIN file per track. The sheet marks the
//    areas with "REM SINGLE-DENSITY AREA" and "REM HIGH-DENSITY AREA"; a
//    sheet without them is refused. Each BIN holds its track's pregap, so the
//    files of an area follow one another on the disc without gaps.
//  - GDI: a text file with one line per track, giving its start sector.
//
// Data tracks may be raw (2352 bytes a sector, Mode 1: checked on every
// read) or cooked (2048 bytes a sector).
//
// Runtime code: no std::format (see CMakeLists.txt).
#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>

namespace qilin {

inline constexpr uint32_t kHighDensityStart = 45000;  // first sector of the high-density area
inline constexpr uint32_t kSectorData = 2048;         // user data in a Mode 1 sector
inline constexpr uint32_t kSystemAreaSectors = 16;    // IP.BIN

struct GdTrack {
    unsigned number = 0;
    bool data = false;          // a data track (as opposed to audio)
    bool high_density = false;
    uint32_t start = 0;         // sector of the first one in the file
    uint32_t sectors = 0;
    uint32_t sector_size = 0;   // bytes a sector in the file: 2352 or 2048
    uint64_t file_offset = 0;   // where the track starts in the file
    std::filesystem::path file;

    uint32_t end() const { return start + sectors; }
};

struct GdFile {
    std::string path;     // "1ST_READ.BIN", "SOUND/TOKYO.AFS": '/' separated, version suffix removed
    uint32_t sector = 0;  // first sector
    uint32_t size = 0;    // bytes
};

// A track's file kept open between reads, positioned at the sector after the last read, so a
// run of reads one sector at a time (the drive's) is one open and no seeks. A copy of a disc
// starts with none open.
struct GdReader {
    std::ifstream file;
    size_t track = SIZE_MAX;  // the open file's track (an index into GdRom::tracks); SIZE_MAX: none
    uint32_t next = 0;        // the sector the file is positioned at
    GdReader() = default;
    GdReader(const GdReader&) {}
    GdReader& operator=(const GdReader&) {
        close();
        return *this;
    }
    void close() {
        file = std::ifstream();
        track = SIZE_MAX;
    }
};

class GdRom {
public:
    // Opens a .cue or .gdi (by its extension). Errors come back as a message.
    static std::expected<GdRom, std::string> open(const std::filesystem::path& image);

    std::vector<GdTrack> tracks;  // in disc order

    // The user data of `count` sectors starting at `sector`: 2048 bytes each
    // into `out`. Fails if a sector isn't in a data track, or isn't a Mode 1
    // sector (raw tracks).
    std::expected<void, std::string> read(uint32_t sector, uint32_t count, std::span<uint8_t> out) const;

    // `count` sectors of an audio track from `sector`: 588 frames each of
    // 16-bit left and right samples (interleaved, CD audio's 44.1 kHz). An
    // error for a sector outside an audio track.
    std::expected<std::vector<int16_t>, std::string> read_audio(uint32_t sector, uint32_t count) const;

    // IP.BIN: the 16 sectors at the start of the high-density area.
    std::expected<std::vector<uint8_t>, std::string> system_area() const;

    // Every file of the high-density area's ISO 9660 file system, sorted by path.
    std::expected<std::vector<GdFile>, std::string> files() const;
    std::expected<std::vector<uint8_t>, std::string> read_file(const GdFile& file) const;

private:
    // One for data and one for audio: read is the game's thread's, read_audio the sound
    // mixer's. Neither is shared between threads.
    mutable GdReader data_, audio_;
};

}  // namespace qilin
