#include "gdrom.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>
#include <system_error>

#include "bootfile.hpp"

namespace qilin {

namespace {

constexpr uint32_t kRawSector = 2352;
constexpr uint32_t kRawHeader = 16;  // sync (12), address (3), mode (1)
constexpr uint32_t kIsoFirstDescriptor = 16;
constexpr size_t kMaxFiles = 100000;
constexpr unsigned kMaxDepth = 8;  // ISO 9660 level 1 and 2 allow 8 directory levels

std::unexpected<std::string> fail(std::string message) { return std::unexpected(std::move(message)); }

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::expected<uint32_t, std::string> number(const std::string& s, const std::string& what) {
    if (s.empty() || s.size() > 9) return fail(what + ": \"" + s + "\" isn't a number");
    uint32_t v = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return fail(what + ": \"" + s + "\" isn't a number");
        v = v * 10 + static_cast<uint32_t>(c - '0');
    }
    return v;
}

std::expected<std::vector<std::string>, std::string> read_lines(const std::filesystem::path& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return fail("cannot open " + path.string());
    std::vector<std::string> lines;
    for (std::string line; std::getline(f, line);) lines.push_back(trim(std::move(line)));
    if (f.bad()) return fail("cannot read " + path.string());
    return lines;
}

// A track's size in sectors, from its file.
std::expected<void, std::string> measure(GdTrack& t) {
    std::error_code ec;
    const uint64_t bytes = std::filesystem::file_size(t.file, ec);
    if (ec) return fail("cannot open " + t.file.string());
    if (bytes < t.file_offset) return fail(t.file.string() + ": shorter than the track's offset in it");
    const uint64_t size = bytes - t.file_offset;
    if (size == 0 || size % t.sector_size)
        return fail(t.file.string() + ": its size isn't a whole number of " + std::to_string(t.sector_size) +
                    "-byte sectors");
    if (size / t.sector_size > 600000) return fail(t.file.string() + ": larger than a GD-ROM");
    t.sectors = static_cast<uint32_t>(size / t.sector_size);
    return {};
}

// Redump CUE: FILE "name" BINARY / TRACK nn MODE1/2352 | AUDIO / INDEX ...
std::expected<GdRom, std::string> open_cue(const std::filesystem::path& image) {
    auto lines = read_lines(image);
    if (!lines) return std::unexpected(lines.error());
    const std::string where = image.string();
    GdRom disc;
    bool have_area = false, high_density = false, file_used = true;
    std::filesystem::path file;
    for (const std::string& line : *lines) {
        if (line.empty()) continue;
        if (line.starts_with("REM")) {
            const std::string remark = trim(line.substr(3));
            if (remark == "SINGLE-DENSITY AREA") have_area = true, high_density = false;
            else if (remark == "HIGH-DENSITY AREA") have_area = true, high_density = true;
            continue;
        }
        if (line.starts_with("FILE")) {
            const size_t open = line.find('"'), close = line.rfind('"');
            if (open == std::string::npos || close == open) return fail(where + ": FILE without a quoted name");
            if (trim(line.substr(close + 1)) != "BINARY") return fail(where + ": only BINARY files are supported");
            if (!file_used) return fail(where + ": a FILE without a TRACK");
            file = image.parent_path() / std::filesystem::path(line.substr(open + 1, close - open - 1));
            file_used = false;
            continue;
        }
        if (line.starts_with("TRACK")) {
            if (!have_area)
                return fail(where + ": no \"REM SINGLE-DENSITY AREA\" / \"REM HIGH-DENSITY AREA\" remarks: "
                                    "not a Redump GD-ROM cue sheet");
            if (file_used) return fail(where + ": more than one TRACK in a FILE (Redump has one file a track)");
            std::istringstream words(line);
            std::string keyword, num, mode;
            words >> keyword >> num >> mode;
            GdTrack t;
            auto n = number(num, where + ": track number");
            if (!n) return std::unexpected(n.error());
            t.number = *n;
            if (t.number != disc.tracks.size() + 1) return fail(where + ": tracks out of order");
            t.high_density = high_density;
            t.file = file;
            if (mode == "AUDIO") t.data = false, t.sector_size = kRawSector;
            else if (mode == "MODE1/2352") t.data = true, t.sector_size = kRawSector;
            else if (mode == "MODE1/2048") t.data = true, t.sector_size = kSectorData;
            else return fail(where + ": track " + num + " has unsupported mode " + mode);
            if (auto ok = measure(t); !ok) return std::unexpected(ok.error());
            // Files of an area follow one another on the disc.
            const GdTrack* previous = disc.tracks.empty() ? nullptr : &disc.tracks.back();
            if (previous && previous->high_density == high_density) t.start = previous->end();
            else t.start = high_density ? kHighDensityStart : 0;
            if (previous && previous->high_density && !high_density)
                return fail(where + ": single-density track after the high-density area");
            disc.tracks.push_back(std::move(t));
            file_used = true;
            continue;
        }
        if (line.starts_with("INDEX") || line.starts_with("PREGAP") || line.starts_with("POSTGAP") ||
            line.starts_with("FLAGS") || line.starts_with("CATALOG") || line.starts_with("ISRC"))
            continue;  // positions inside a track: not needed, the file holds the whole track
        return fail(where + ": unexpected line \"" + line + "\"");
    }
    if (!file_used) return fail(where + ": a FILE without a TRACK");
    return disc;
}

// GDI: the track count, then "number start type sector_size file offset" a track.
std::expected<GdRom, std::string> open_gdi(const std::filesystem::path& image) {
    auto lines = read_lines(image);
    if (!lines) return std::unexpected(lines.error());
    const std::string where = image.string();
    std::erase_if(*lines, [](const std::string& l) { return l.empty(); });
    if (lines->empty()) return fail(where + ": empty");
    auto count = number((*lines)[0], where + ": track count");
    if (!count) return std::unexpected(count.error());
    if (*count != lines->size() - 1) return fail(where + ": the track count doesn't match the lines that follow");
    GdRom disc;
    for (size_t i = 1; i < lines->size(); ++i) {
        const std::string& line = (*lines)[i];
        // The file name may be quoted and hold spaces: take four fields from
        // the left, one from the right, and the name is what's between.
        std::istringstream words(line);
        std::string num, start, type, sector_size;
        words >> num >> start >> type >> sector_size;
        std::string rest;
        std::getline(words, rest);
        rest = trim(std::move(rest));
        const size_t last_space = rest.find_last_of(" \t");
        if (last_space == std::string::npos) return fail(where + ": malformed track line \"" + line + "\"");
        std::string name = trim(rest.substr(0, last_space));
        const std::string offset = rest.substr(last_space + 1);
        if (name.size() >= 2 && name.front() == '"' && name.back() == '"') name = name.substr(1, name.size() - 2);
        if (name.empty()) return fail(where + ": malformed track line \"" + line + "\"");

        GdTrack t;
        const auto n = number(num, where + ": track number");
        const auto s = number(start, where + ": track start");
        const auto ty = number(type, where + ": track type");
        const auto ss = number(sector_size, where + ": sector size");
        const auto off = number(offset, where + ": file offset");
        for (const auto* v : {&n, &s, &ty, &ss, &off})
            if (!*v) return std::unexpected(v->error());
        t.number = *n;
        if (t.number != disc.tracks.size() + 1) return fail(where + ": tracks out of order");
        t.start = *s;
        if (*ty != 0 && *ty != 4) return fail(where + ": track " + num + " has unknown type " + type);
        t.data = *ty == 4;
        if (*ss != kRawSector && *ss != kSectorData)
            return fail(where + ": track " + num + " has unsupported sector size " + sector_size);
        if (!t.data && *ss != kRawSector) return fail(where + ": audio track " + num + " isn't 2352 bytes a sector");
        t.sector_size = *ss;
        t.file_offset = *off;
        t.file = image.parent_path() / std::filesystem::path(name);
        t.high_density = t.start >= kHighDensityStart;
        if (auto ok = measure(t); !ok) return std::unexpected(ok.error());
        if (!disc.tracks.empty() && t.start < disc.tracks.back().end())
            return fail(where + ": track " + num + " starts before the track before it ends");
        disc.tracks.push_back(std::move(t));
    }
    return disc;
}

unsigned from_bcd(uint8_t b) { return (b >> 4) * 10u + (b & 15u); }

// One directory record of an ISO 9660 directory.
struct Record {
    std::string name;
    uint32_t sector = 0, size = 0;
    bool directory = false;
};

// Names are ISO 9660 d-characters plus what Dreamcast discs use in practice.
// Anything else (a path separator above all) is refused: names become paths
// on the host when the disc is extracted.
bool safe_name(const std::string& name) {
    if (name.empty() || name == "." || name == "..") return false;
    return std::ranges::all_of(name, [](char c) {
        return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' ||
               c == '.' || c == '!' || c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' || c == '(' ||
               c == ')' || c == '@' || c == '^' || c == '`' || c == '{' || c == '}' || c == '~' || c == '+' ||
               c == ',' || c == '=' || c == '[' || c == ']' || c == ' ';
    });
}

std::expected<void, std::string> walk(const GdRom& disc, uint32_t sector, uint32_t size, const std::string& prefix,
                                      unsigned depth, std::vector<GdFile>& out) {
    if (depth > kMaxDepth) return fail("ISO 9660: directories nested deeper than the format allows: " + prefix);
    if (size == 0 || size % kSectorData || size > (4u << 20))
        return fail("ISO 9660: directory \"" + prefix + "\" has an implausible size");
    std::vector<uint8_t> dir(size);
    if (auto ok = disc.read(sector, size / kSectorData, dir); !ok) return std::unexpected(ok.error());
    std::vector<Record> subdirectories;
    for (size_t pos = 0; pos < dir.size();) {
        const size_t length = dir[pos];
        if (length == 0) {  // records don't cross sectors: the rest of this one is padding
            pos = (pos / kSectorData + 1) * kSectorData;
            continue;
        }
        const size_t in_sector = kSectorData - pos % kSectorData;
        if (length < 34 || length > in_sector) return fail("ISO 9660: malformed record in directory \"" + prefix + "\"");
        const uint8_t* r = dir.data() + pos;
        const size_t name_length = r[32];
        if (33 + name_length > length) return fail("ISO 9660: malformed record in directory \"" + prefix + "\"");
        pos += length;
        if (name_length == 1 && (r[33] == 0 || r[33] == 1)) continue;  // "." and ".."
        Record rec;
        rec.sector = read_le32(r + 2);
        rec.size = read_le32(r + 10);
        rec.directory = r[25] & 2;
        if (r[25] & 0x80) return fail("ISO 9660: a file in \"" + prefix + "\" has more than one extent");
        if (r[26] || r[27]) return fail("ISO 9660: an interleaved file in \"" + prefix + "\"");
        rec.name.assign(reinterpret_cast<const char*>(r) + 33, name_length);
        if (const size_t semi = rec.name.find(';'); semi != std::string::npos) rec.name.resize(semi);
        if (!rec.directory && rec.name.ends_with('.')) rec.name.pop_back();
        if (!safe_name(rec.name)) return fail("ISO 9660: a name in \"" + prefix + "\" isn't usable as a file name");
        if (rec.directory) {
            subdirectories.push_back(std::move(rec));
        } else {
            if (out.size() >= kMaxFiles) return fail("ISO 9660: more files than a disc can hold");
            out.push_back({prefix + rec.name, rec.sector, rec.size});
        }
    }
    for (const Record& d : subdirectories)
        if (auto ok = walk(disc, d.sector, d.size, prefix + d.name + "/", depth + 1, out); !ok)
            return std::unexpected(ok.error());
    return {};
}

}  // namespace

std::expected<GdRom, std::string> GdRom::open(const std::filesystem::path& image) {
    std::string ext = image.extension().string();
    std::ranges::transform(ext, ext.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; });
    if (ext != ".cue" && ext != ".gdi") return fail(image.string() + ": not a .cue or .gdi disc image");
    std::expected<GdRom, std::string> disc = ext == ".cue" ? open_cue(image) : open_gdi(image);
    if (!disc) return disc;
    const std::string where = image.string();
    if (disc->tracks.size() < 3) return fail(where + ": fewer than three tracks: not a GD-ROM");
    const GdTrack& first = disc->tracks[2];
    if (!first.high_density || first.start != kHighDensityStart || !first.data)
        return fail(where + ": track 3 isn't a data track at sector 45000: not a GD-ROM");
    if (disc->tracks[0].high_density || disc->tracks[1].high_density)
        return fail(where + ": tracks 1 and 2 aren't the single-density area: not a GD-ROM");
    return disc;
}

std::expected<void, std::string> GdRom::read(uint32_t sector, uint32_t count, std::span<uint8_t> out) const {
    if (out.size() != uint64_t(count) * kSectorData) return fail("GdRom::read: buffer size doesn't match the count");
    const GdTrack* track = nullptr;
    std::ifstream f;
    std::vector<uint8_t> raw(kRawSector);
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t s = sector + i;
        if (!track || s >= track->end()) {
            track = nullptr;
            for (const GdTrack& t : tracks)
                if (s >= t.start && s < t.end()) track = &t;
            if (!track) return fail("sector " + std::to_string(s) + " isn't in any track of the image");
            if (!track->data)
                return fail("sector " + std::to_string(s) + " is in audio track " + std::to_string(track->number));
            f = std::ifstream(track->file, std::ios::binary);
            if (!f) return fail("cannot open " + track->file.string());
            f.seekg(static_cast<std::streamoff>(track->file_offset + uint64_t(s - track->start) * track->sector_size));
        }
        uint8_t* dest = out.data() + size_t(i) * kSectorData;
        if (track->sector_size == kSectorData) {
            f.read(reinterpret_cast<char*>(dest), kSectorData);
        } else {
            f.read(reinterpret_cast<char*>(raw.data()), kRawSector);
            if (f) {
                // A raw sector says what it is and where it is: check both,
                // which also proves the track layout worked out above.
                static constexpr uint8_t kSync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
                const std::string name = "sector " + std::to_string(s) + " (track " + std::to_string(track->number) + ")";
                if (std::memcmp(raw.data(), kSync, sizeof kSync) != 0) return fail(name + " has no sync pattern");
                if (raw[15] != 1) return fail(name + " isn't a Mode 1 sector");
                const uint32_t address = (from_bcd(raw[12]) * 60 + from_bcd(raw[13])) * 75 + from_bcd(raw[14]);
                if (address != s + 150)
                    return fail(name + " carries the address of sector " + std::to_string(int64_t(address) - 150) +
                                ": the image's track layout isn't what it says");
                std::memcpy(dest, raw.data() + kRawHeader, kSectorData);
            }
        }
        if (!f) return fail("cannot read sector " + std::to_string(s) + " from " + track->file.string());
    }
    return {};
}

std::expected<std::vector<uint8_t>, std::string> GdRom::system_area() const {
    std::vector<uint8_t> ip(size_t(kSystemAreaSectors) * kSectorData);
    if (auto ok = read(kHighDensityStart, kSystemAreaSectors, ip); !ok) return std::unexpected(ok.error());
    return ip;
}

std::expected<std::vector<GdFile>, std::string> GdRom::files() const {
    std::vector<uint8_t> pvd(kSectorData);
    if (auto ok = read(kHighDensityStart + kIsoFirstDescriptor, 1, pvd); !ok) return std::unexpected(ok.error());
    if (pvd[0] != 1 || std::memcmp(pvd.data() + 1, "CD001", 5) != 0)
        return fail("ISO 9660: no primary volume descriptor at sector " +
                    std::to_string(kHighDensityStart + kIsoFirstDescriptor));
    if (read_le16(pvd.data() + 128) != kSectorData) return fail("ISO 9660: logical blocks aren't 2048 bytes");
    const uint8_t* root = pvd.data() + 156;
    if (root[0] < 34 || !(root[25] & 2)) return fail("ISO 9660: malformed root directory record");
    std::vector<GdFile> out;
    if (auto ok = walk(*this, read_le32(root + 2), read_le32(root + 10), "", 1, out); !ok)
        return std::unexpected(ok.error());
    std::ranges::sort(out, {}, &GdFile::path);
    for (size_t i = 1; i < out.size(); ++i)
        if (out[i].path == out[i - 1].path) return fail("ISO 9660: two files are named " + out[i].path);
    return out;
}

std::expected<std::vector<uint8_t>, std::string> GdRom::read_file(const GdFile& file) const {
    const auto sectors = static_cast<uint32_t>((uint64_t(file.size) + kSectorData - 1) / kSectorData);
    std::vector<uint8_t> data(size_t(sectors) * kSectorData);
    if (sectors)
        if (auto ok = read(file.sector, sectors, data); !ok) return std::unexpected(ok.error() + " (" + file.path + ")");
    data.resize(file.size);
    return data;
}

}  // namespace qilin
