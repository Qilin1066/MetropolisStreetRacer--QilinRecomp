#include "gdrom.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <optional>
#include <sstream>
#include <system_error>

#include <libchdr/chd.h>

#include "bootfile.hpp"
#include "path.hpp"

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
    if (!f) return fail("cannot open " + utf8_path(path));
    std::vector<std::string> lines;
    for (std::string line; std::getline(f, line);) lines.push_back(trim(std::move(line)));
    if (f.bad()) return fail("cannot read " + utf8_path(path));
    return lines;
}

// A track's size in sectors, from its file.
std::expected<void, std::string> measure(GdTrack& t) {
    std::error_code ec;
    const std::string name = utf8_path(t.file);
    const uint64_t bytes = std::filesystem::file_size(t.file, ec);
    if (ec) return fail("cannot open " + name);
    if (bytes < t.file_offset) return fail(name + ": shorter than the track's offset in it");
    const uint64_t size = bytes - t.file_offset;
    if (size == 0 || size % t.sector_size)
        return fail(name + ": its size isn't a whole number of " + std::to_string(t.sector_size) +
                    "-byte sectors");
    if (size / t.sector_size > 600000) return fail(name + ": larger than a GD-ROM");
    t.sectors = static_cast<uint32_t>(size / t.sector_size);
    return {};
}

// Redump CUE: FILE "name" BINARY / TRACK nn MODE1/2352 | AUDIO / INDEX ...
std::expected<GdRom, std::string> open_cue(const std::filesystem::path& image) {
    auto lines = read_lines(image);
    if (!lines) return std::unexpected(lines.error());
    const std::string where = utf8_path(image);
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
            file = image.parent_path() / native_path(line.substr(open + 1, close - open - 1));
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
    const std::string where = utf8_path(image);
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
        t.file = image.parent_path() / native_path(name);
        t.high_density = t.start >= kHighDensityStart;
        if (auto ok = measure(t); !ok) return std::unexpected(ok.error());
        if (!disc.tracks.empty() && t.start < disc.tracks.back().end())
            return fail(where + ": track " + num + " starts before the track before it ends");
        disc.tracks.push_back(std::move(t));
    }
    return disc;
}

}  // namespace

// ---- .chd (libchdr), ported from Toy Commander--QilinRecomp's reader ----

// A .chd open for reading, with the hunk last read.
struct GdChd {
    std::FILE* stdio = nullptr;
    chd_file* chd = nullptr;
    uint32_t hunk_bytes = 0, unit_bytes = 0, hunks = 0, frames_per_hunk = 0;
    std::vector<uint8_t> hunk;
    uint32_t hunk_no = UINT32_MAX;

    GdChd() = default;
    GdChd(const GdChd&) = delete;
    GdChd& operator=(const GdChd&) = delete;
    ~GdChd() {
        if (chd) chd_close(chd);
        if (stdio) std::fclose(stdio);  // chd_open_file never closes it
    }

    std::expected<void, std::string> open(const std::filesystem::path& path) {
        const std::string name = utf8_path(path.filename());
#if defined(_WIN32)
        stdio = _wfopen(path.c_str(), L"rb");
#else
        stdio = std::fopen(path.c_str(), "rb");
#endif
        if (!stdio) return fail("cannot open " + utf8_path(path));
        if (const chd_error e = chd_open_file(stdio, CHD_OPEN_READ, nullptr, &chd); e != CHDERR_NONE)
            return fail(name + ": not a CHD libchdr can read (" + chd_error_string(e) + ")");
        const chd_header* h = chd_get_header(chd);
        hunk_bytes = h->hunkbytes, unit_bytes = h->unitbytes, hunks = h->totalhunks;
        // A CD's units are frames: a sector's 2352 bytes and 96 of subcode.
        if (unit_bytes != kRawSector + 96 || hunk_bytes == 0 || hunk_bytes % unit_bytes)
            return fail(name + ": not a CD or GD-ROM image (its units are " + std::to_string(unit_bytes) + " bytes, not 2448)");
        frames_per_hunk = hunk_bytes / unit_bytes;
        hunk.resize(hunk_bytes);
        return {};
    }

    // Frame `frame`'s first `bytes` bytes into `out`.
    std::expected<void, std::string> read_frame(uint64_t frame, uint8_t* out, uint32_t bytes) {
        const uint64_t h = frame / frames_per_hunk;
        if (h >= hunks) return fail("frame " + std::to_string(frame) + " lies beyond the CHD's end");
        if (h != hunk_no) {
            hunk_no = UINT32_MAX;
            if (const chd_error e = chd_read(chd, static_cast<uint32_t>(h), hunk.data()); e != CHDERR_NONE)
                return fail("cannot read hunk " + std::to_string(h) + " of the CHD: " + chd_error_string(e));
            hunk_no = static_cast<uint32_t>(h);
        }
        std::memcpy(out, hunk.data() + (frame % frames_per_hunk) * unit_bytes, bytes);
        return {};
    }
};

namespace {

// A raw data sector's header address (BCD minutes, seconds, frames from the
// disc's start, 150 sectors before sector 0), if it holds a decodable one.
std::optional<uint32_t> header_address(const uint8_t* raw) {
    static constexpr uint8_t kSync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
    if (std::memcmp(raw, kSync, sizeof kSync) != 0 || (raw[15] != 1 && raw[15] != 2)) return std::nullopt;
    const auto bcd = [](uint8_t b) -> std::optional<uint32_t> {
        if ((b >> 4) > 9 || (b & 15) > 9) return std::nullopt;
        return (b >> 4) * 10u + (b & 15u);
    };
    const auto m = bcd(raw[12]), s = bcd(raw[13]), f = bcd(raw[14]);
    if (!m || !s || !f || *s >= 60 || *f >= 75) return std::nullopt;
    const uint32_t msf = (*m * 60 + *s) * 75 + *f;
    if (msf < 150) return std::nullopt;
    return msf - 150;
}

// A GD-ROM's .chd: its track list (GDROM_TRACK_METADATA_TAG, one entry a
// track: its type, frames, padding and pregap), each track's frames padded
// to a multiple of 4 in the file. Each raw data track is then placed where
// its sectors' headers say it is (the first readable header in its first
// 1,000 sectors, after any blank pregap).
std::expected<GdRom, std::string> open_chd(const std::filesystem::path& path) {
    const std::string name = utf8_path(path.filename());
    GdChd chd;
    if (auto r = chd.open(path); !r) return std::unexpected(r.error());
    GdRom disc;
    disc.chd = path;
    uint64_t frame = 0;  // in the CHD
    uint32_t next = 0;   // on the disc
    for (uint32_t i = 0;; ++i) {
        char meta[512] = {};
        uint32_t length = 0;
        if (chd_get_metadata(chd.chd, GDROM_TRACK_METADATA_TAG, i, meta, sizeof meta - 1, &length, nullptr, nullptr) != CHDERR_NONE) {
            if (i == 0) {
                const bool cd = chd_get_metadata(chd.chd, CDROM_TRACK_METADATA2_TAG, 0, meta, sizeof meta - 1, &length, nullptr, nullptr) == CHDERR_NONE ||
                                chd_get_metadata(chd.chd, CDROM_TRACK_METADATA_TAG, 0, meta, sizeof meta - 1, &length, nullptr, nullptr) == CHDERR_NONE;
                return fail(name + (cd ? ": a CD's image, not a GD-ROM's (make it from the .gdi or .cue with chdman createcd)"
                                       : ": no GD-ROM track list (not a disc image?)"));
            }
            break;
        }
        meta[std::min<size_t>(length, sizeof meta - 1)] = 0;
        unsigned n = 0, frames = 0, pad = 0, pregap = 0, postgap = 0;
        char type[32] = {}, subtype[32] = {}, pgtype[32] = {}, pgsub[32] = {};
        if (std::sscanf(meta, "TRACK:%u TYPE:%31s SUBTYPE:%31s FRAMES:%u PAD:%u PREGAP:%u PGTYPE:%31s PGSUB:%31s POSTGAP:%u", &n, type, subtype,
                        &frames, &pad, &pregap, pgtype, pgsub, &postgap) != 9)
            return fail(name + ": track " + std::to_string(i + 1) + "'s description is malformed");
        if (n != i + 1) return fail(name + ": tracks are not numbered in order");
        GdTrack t;
        t.number = n;
        const std::string_view kind = type;
        if (kind == "AUDIO") t.data = false, t.swapped = true, t.sector_size = kRawSector;  // CD audio is kept big-endian in a CHD
        else if (kind == "MODE1") t.data = true, t.sector_size = kSectorData;
        else if (kind == "MODE1_RAW") t.data = true, t.sector_size = kRawSector;
        else return fail(name + ": track " + std::to_string(n) + " is " + type + ", which is not supported");
        if (frames == 0 || frames > 600000) return fail(name + ": track " + std::to_string(n) + " has " + std::to_string(frames) + " frames");
        t.high_density = n >= 3;
        if (n == 3) next = kHighDensityStart;
        // A pregap is either in the track's frames ('V' types) or not stored.
        const bool stored = pgtype[0] == 'V';
        if (stored && pregap >= frames) return fail(name + ": track " + std::to_string(n) + "'s pregap is longer than it");
        t.start = next + (stored ? 0 : pregap);
        t.sectors = frames;
        t.chd_frame = static_cast<uint32_t>(frame);
        frame += uint64_t{frames} + pad;
        if (frame > uint64_t{chd.hunks} * chd.frames_per_hunk) return fail(name + ": its tracks run beyond its data");
        next = t.end() + postgap;
        disc.tracks.push_back(t);
    }
    for (GdTrack& t : disc.tracks) {
        if (!t.data || t.sector_size != kRawSector) continue;
        uint8_t raw[kRawSector];
        const uint32_t scan = std::min<uint32_t>(t.sectors, 1000);
        for (uint32_t k = 0; k < scan; ++k) {
            if (auto r = chd.read_frame(uint64_t{t.chd_frame} + k, raw, kRawSector); !r) return std::unexpected(r.error());
            const auto address = header_address(raw);
            if (!address) continue;
            if (*address < k) return fail(name + ": track " + std::to_string(t.number) + "'s sector headers are malformed");
            t.start = *address - k;
            break;
        }
    }
    for (size_t i = 1; i < disc.tracks.size(); ++i) {
        const GdTrack& a = disc.tracks[i - 1];
        const GdTrack& b = disc.tracks[i];
        if (a.high_density == b.high_density && b.start < a.end())
            return fail(name + ": tracks " + std::to_string(a.number) + " and " + std::to_string(b.number) + " overlap");
        if (b.end() > 600000) return fail(name + ": track " + std::to_string(b.number) + " runs past a GD-ROM's end");
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

namespace {
std::string lower_extension(const std::filesystem::path& p) {
    std::string ext = utf8_path(p.extension());
    std::ranges::transform(ext, ext.begin(), [](char c) { return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c; });
    return ext;
}
}  // namespace

bool GdRom::is_image(const std::filesystem::path& path) {
    const std::string ext = lower_extension(path);
    return ext == ".cue" || ext == ".gdi" || ext == ".chd";
}

std::expected<GdRom, std::string> GdRom::open(const std::filesystem::path& image) {
    const std::string ext = lower_extension(image);
    if (!is_image(image)) return fail(utf8_path(image) + ": not a .cue, .gdi or .chd disc image");
    std::expected<GdRom, std::string> disc = ext == ".cue" ? open_cue(image) : ext == ".gdi" ? open_gdi(image) : open_chd(image);
    if (!disc) return disc;
    const std::string where = utf8_path(image);
    if (disc->tracks.size() < 3) return fail(where + ": fewer than three tracks: not a GD-ROM");
    const GdTrack& first = disc->tracks[2];
    if (!first.high_density || first.start != kHighDensityStart || !first.data)
        return fail(where + ": track 3 isn't a data track at sector 45000: not a GD-ROM");
    if (disc->tracks[0].high_density || disc->tracks[1].high_density)
        return fail(where + ": tracks 1 and 2 aren't the single-density area: not a GD-ROM");
    return disc;
}

namespace {
// Positions `r` at sector `s` of the track holding it, opening the track's file only when it is
// another file and seeking only when the read isn't of the next sector.
enum class Seek { ok, nowhere, wrong_kind, cannot_open };
// A .chd's reader opens the image once and reads it by frame (read_stored).
Seek seek(const GdRom& disc, GdReader& r, uint32_t s, bool data) {
    const std::vector<GdTrack>& tracks = disc.tracks;
    if (!disc.chd.empty()) {
        if (r.track >= tracks.size() || s < tracks[r.track].start || s >= tracks[r.track].end()) {
            size_t found = SIZE_MAX;
            for (size_t k = 0; k < tracks.size(); ++k)
                if (s >= tracks[k].start && s < tracks[k].end()) found = k;
            if (found == SIZE_MAX) return Seek::nowhere;
            if (tracks[found].data != data) return Seek::wrong_kind;
            r.track = found;
        }
        if (!r.chd) {
            auto chd = std::make_shared<GdChd>();
            if (!chd->open(disc.chd)) return Seek::cannot_open;
            r.chd = std::move(chd);
        }
        return Seek::ok;
    }
    if (r.track >= tracks.size() || s < tracks[r.track].start || s >= tracks[r.track].end()) {
        size_t found = SIZE_MAX;
        for (size_t k = 0; k < tracks.size(); ++k)
            if (s >= tracks[k].start && s < tracks[k].end()) found = k;
        if (found == SIZE_MAX) return Seek::nowhere;
        if (tracks[found].data != data) return Seek::wrong_kind;
        if (r.track >= tracks.size() || tracks[r.track].file != tracks[found].file) {
            r.close();
            r.file = std::ifstream(tracks[found].file, std::ios::binary);
            if (!r.file) {
                r.close();
                r.track = found;  // for the caller's message
                return Seek::cannot_open;
            }
        }
        r.track = found;
        r.next = UINT32_MAX;
    }
    if (r.next != s) {
        const GdTrack& t = tracks[r.track];
        r.file.clear();
        r.file.seekg(static_cast<std::streamoff>(t.file_offset + uint64_t(s - t.start) * t.sector_size));
        r.next = s;
    }
    return Seek::ok;
}

unsigned track_number(const std::vector<GdTrack>& tracks, uint32_t s) {
    for (const GdTrack& t : tracks)
        if (s >= t.start && s < t.end()) return t.number;
    return 0;
}

// What a reader reads from: the .chd, or its track's file (UTF-8, for messages).
std::string source(const GdRom& disc, const GdReader& r) {
    if (!disc.chd.empty()) return utf8_path(disc.chd.filename());
    return r.track < disc.tracks.size() ? utf8_path(disc.tracks[r.track].file.filename()) : std::string("the image");
}

// Sector `s` as stored (its track's sector_size bytes) into `out`, from where seek put `r`.
std::expected<void, std::string> read_stored(const GdRom& disc, GdReader& r, uint32_t s, uint8_t* out) {
    const GdTrack& t = disc.tracks[r.track];
    if (r.chd) {
        if (auto ok = r.chd->read_frame(uint64_t{t.chd_frame} + (s - t.start), out, t.sector_size); !ok) {
            r.close();
            return fail("sector " + std::to_string(s) + ": " + ok.error());
        }
        return {};
    }
    r.file.read(reinterpret_cast<char*>(out), t.sector_size);
    if (!r.file) {
        const std::string file = source(disc, r);
        r.close();
        return fail("cannot read sector " + std::to_string(s) + " from " + file);
    }
    r.next = s + 1;
    return {};
}
}  // namespace

std::expected<std::vector<int16_t>, std::string> GdRom::read_audio(uint32_t sector, uint32_t count) const {
    std::vector<int16_t> out;
    out.reserve(size_t{count} * (kRawSector / 2));
    uint8_t raw[kRawSector];
    for (uint32_t n = 0; n < count; ++n, ++sector) {
        const std::string name = "sector " + std::to_string(sector);
        switch (seek(*this, audio_, sector, false)) {
        case Seek::ok: break;
        case Seek::nowhere: return fail(name + " is not on the disc");
        case Seek::wrong_kind: return fail(name + " is in data track " + std::to_string(track_number(tracks, sector)));
        case Seek::cannot_open: {
            const std::string file = chd.empty() ? utf8_path(tracks[audio_.track].file) : utf8_path(chd);
            audio_.close();
            return fail("cannot open " + file);
        }
        }
        const bool swapped = tracks[audio_.track].swapped;
        if (auto ok = read_stored(*this, audio_, sector, raw); !ok) return std::unexpected(ok.error());
        if (swapped)
            for (uint32_t k = 0; k < kRawSector; k += 2) out.push_back(static_cast<int16_t>(raw[k] << 8 | raw[k + 1]));
        else
            for (uint32_t k = 0; k < kRawSector; k += 2) out.push_back(static_cast<int16_t>(raw[k] | raw[k + 1] << 8));
    }
    return out;
}

std::expected<void, std::string> GdRom::read(uint32_t sector, uint32_t count, std::span<uint8_t> out) const {
    if (out.size() != uint64_t(count) * kSectorData) return fail("GdRom::read: buffer size doesn't match the count");
    uint8_t raw[kRawSector];
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t s = sector + i;
        switch (seek(*this, data_, s, true)) {
        case Seek::ok: break;
        case Seek::nowhere: return fail("sector " + std::to_string(s) + " isn't in any track of the image");
        case Seek::wrong_kind:
            return fail("sector " + std::to_string(s) + " is in audio track " + std::to_string(track_number(tracks, s)));
        case Seek::cannot_open: {
            const std::string file = chd.empty() ? utf8_path(tracks[data_.track].file) : utf8_path(chd);
            data_.close();
            return fail("cannot open " + file);
        }
        }
        const GdTrack& track = tracks[data_.track];
        uint8_t* dest = out.data() + size_t(i) * kSectorData;
        if (track.sector_size == kSectorData) {
            if (auto ok = read_stored(*this, data_, s, dest); !ok) return std::unexpected(ok.error());
            continue;
        }
        if (auto ok = read_stored(*this, data_, s, raw); !ok) return std::unexpected(ok.error());
        // A raw sector says what it is and where it is: check both,
        // which also proves the track layout worked out above.
        static constexpr uint8_t kSync[12] = {0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0};
        const std::string name = "sector " + std::to_string(s) + " (track " + std::to_string(track.number) + ")";
        std::string wrong;
        if (std::memcmp(raw, kSync, sizeof kSync) != 0) wrong = name + " has no sync pattern";
        else if (raw[15] != 1) wrong = name + " isn't a Mode 1 sector";
        else if (const uint32_t address = (from_bcd(raw[12]) * 60 + from_bcd(raw[13])) * 75 + from_bcd(raw[14]); address != s + 150)
            wrong = name + " carries the address of sector " + std::to_string(int64_t(address) - 150) +
                    ": the image's track layout isn't what it says";
        if (!wrong.empty()) {
            data_.close();
            return fail(wrong);
        }
        std::memcpy(dest, raw + kRawHeader, kSectorData);
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
