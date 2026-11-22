// Runtime code: no std::format.
#include "drive.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <map>

#include "gdrom.hpp"

namespace msr {

namespace {

constexpr uint32_t kHighDensity = 45000;
constexpr uint32_t kSector = 2048;

// ---- the image: the disc reader over the player's dump ----
class ImageDrive final : public Drive {
public:
    explicit ImageDrive(const qilin::GdRom& disc) : disc_(disc) {}
    bool read(uint32_t sector, uint8_t* out) override {
        auto ok = disc_.read(sector, 1, std::span<uint8_t>(out, kSector));
        if (!ok) error_ = ok.error();
        return ok.has_value();
    }
    std::vector<DriveTrack> tracks() const override {
        std::vector<DriveTrack> t;
        for (const qilin::GdTrack& g : disc_.tracks) t.push_back({g.number, g.data, g.high_density, g.start, g.sectors});
        return t;
    }
    const qilin::GdRom* image() const override { return &disc_; }

private:
    const qilin::GdRom& disc_;
};

// ---- a folder, as an ISO 9660 file system built in memory ----
// Layout from sector 45000: IP.BIN (16 sectors), the primary volume
// descriptor, the terminator, the two path tables (one sector each), the
// directories (each in whole sectors, records never across a sector's end)
// and the files, each from a sector of its own. ECMA-119's structures; dates
// are 2000-11-30, the disc's own.
struct Entry {
    std::string name;  // ISO name: "NAME.EXT;1" for a file, "NAME" for a directory
    std::filesystem::path path;
    bool dir = false;
    uint32_t size = 0;    // bytes (a directory's: its sectors * 2048)
    uint32_t sector = 0;  // its extent
    std::vector<size_t> children;  // directories: indices into the entries
    size_t parent = 0;
    unsigned number = 0;  // directories: path table number (from 1)
};

void both16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8), p[2] = uint8_t(v >> 8), p[3] = uint8_t(v); }
void both32(uint8_t* p, uint32_t v) {
    for (int k = 0; k < 4; ++k) p[k] = uint8_t(v >> (8 * k)), p[7 - k] = uint8_t(v >> (8 * k));
}
void le32(uint8_t* p, uint32_t v) {
    for (int k = 0; k < 4; ++k) p[k] = uint8_t(v >> (8 * k));
}
void be32(uint8_t* p, uint32_t v) {
    for (int k = 0; k < 4; ++k) p[3 - k] = uint8_t(v >> (8 * k));
}
void le16(uint8_t* p, uint16_t v) { p[0] = uint8_t(v), p[1] = uint8_t(v >> 8); }
void be16(uint8_t* p, uint16_t v) { p[1] = uint8_t(v), p[0] = uint8_t(v >> 8); }
constexpr uint8_t kDate[7] = {100, 11, 30, 0, 0, 0, 0};  // years since 1900, month, day, h, m, s, GMT offset

size_t record_size(const std::string& name) { return (33 + name.size() + 1) & ~size_t{1}; }

class FolderDrive final : public Drive {
public:
    bool build(const std::filesystem::path& folder, std::string& why) {
        std::ifstream ip(folder / "IP.BIN", std::ios::binary);
        if (!ip) return why = "no IP.BIN in " + folder.string(), false;
        ip_.assign(std::istreambuf_iterator<char>(ip), {});
        if (ip_.size() != 16 * kSector) return why = "IP.BIN isn't 32 KB", false;
        Entry root;
        root.path = folder, root.dir = true;
        entries_.push_back(std::move(root));
        if (!scan(0, why)) return false;
        // Directories in path table order: breadth first.
        std::vector<size_t> order{0};
        for (size_t k = 0; k < order.size(); ++k)
            for (size_t c : entries_[order[k]].children)
                if (entries_[c].dir) order.push_back(c);
        for (size_t k = 0; k < order.size(); ++k) entries_[order[k]].number = static_cast<unsigned>(k + 1);
        dirs_ = order;
        build_path_tables();
        uint32_t next = kHighDensity + 16 + 2 + 2;
        for (size_t d : dirs_) {
            Entry& e = entries_[d];
            size_t used = 0, sectors = 1;
            for (const std::string& n : {std::string(1, '\0'), std::string(1, '\1')}) used += record_size(n);
            for (size_t c : e.children) {
                const size_t r = record_size(entries_[c].name);
                if (used + r > kSector) ++sectors, used = 0;
                used += r;
            }
            e.sector = next, e.size = static_cast<uint32_t>(sectors * kSector);
            next += static_cast<uint32_t>(sectors);
        }
        for (Entry& e : entries_)
            if (!e.dir) {
                e.sector = next;
                next += (e.size + kSector - 1) / kSector;
                if (e.size == 0) ++next;
                files_.emplace(e.sector, &e - entries_.data());
            }
        end_ = next;
        fill_path_table(path_l_, false);
        fill_path_table(path_m_, true);
        return true;
    }

    bool read(uint32_t sector, uint8_t* out) override {
        std::memset(out, 0, kSector);
        if (sector < kHighDensity || sector >= end_) return error_ = "sector " + std::to_string(sector) + " isn't on the disc", false;
        const uint32_t k = sector - kHighDensity;
        if (k < 16) return std::memcpy(out, ip_.data() + size_t{k} * kSector, kSector), true;
        if (k == 16) return volume_descriptor(out), true;
        if (k == 17) return out[0] = 255, std::memcpy(out + 1, "CD001", 5), out[6] = 1, true;
        if (k == 18) return std::memcpy(out, path_l_.data(), path_l_.size()), true;
        if (k == 19) return std::memcpy(out, path_m_.data(), path_m_.size()), true;
        for (size_t d : dirs_) {
            const Entry& e = entries_[d];
            if (sector >= e.sector && sector < e.sector + e.size / kSector) return directory(e, sector - e.sector, out), true;
        }
        auto it = files_.upper_bound(sector);
        if (it == files_.begin()) return true;  // between structures: zeros
        const Entry& f = entries_[std::prev(it)->second];
        const uint64_t offset = uint64_t(sector - f.sector) * kSector;
        if (offset >= f.size) return true;
        std::ifstream in(f.path, std::ios::binary);
        in.seekg(static_cast<std::streamoff>(offset));
        in.read(reinterpret_cast<char*>(out), static_cast<std::streamsize>(std::min<uint64_t>(kSector, f.size - offset)));
        if (!in) return error_ = "cannot read " + f.path.string(), false;
        return true;
    }

    std::vector<DriveTrack> tracks() const override {
        // The single-density area's two tracks (data, then the warning's audio) aren't served;
        // they are listed so the table of contents has the disc's shape.
        return {{1, true, false, 0, 300}, {2, false, false, 300, 4000}, {3, true, true, kHighDensity, end_ - kHighDensity}};
    }

private:
    bool scan(size_t dir, std::string& why) {
        std::error_code ec;
        std::vector<std::filesystem::directory_entry> list;
        for (const auto& de : std::filesystem::directory_iterator(entries_[dir].path, ec)) list.push_back(de);
        if (ec) return why = "cannot list " + entries_[dir].path.string(), false;
        std::vector<Entry> kids;
        for (const auto& de : list) {
            std::string name = de.path().filename().string();
            std::transform(name.begin(), name.end(), name.begin(), [](char ch) { return static_cast<char>(std::toupper(static_cast<unsigned char>(ch))); });
            if (dir == 0 && name == "IP.BIN") continue;  // the system area, not a file
            Entry e;
            e.name = name, e.path = de.path(), e.dir = de.is_directory();
            if (!e.dir) {
                const auto size = de.file_size(ec);
                if (ec || size > 0xFFFFFFFFu) return why = "cannot size " + de.path().string(), false;
                e.size = static_cast<uint32_t>(size);
                e.name += ";1";
            }
            kids.push_back(std::move(e));
        }
        std::sort(kids.begin(), kids.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
        for (Entry& e : kids) {
            e.parent = dir;
            entries_.push_back(std::move(e));
            entries_[dir].children.push_back(entries_.size() - 1);
        }
        for (size_t c : std::vector<size_t>(entries_[dir].children))
            if (entries_[c].dir && !scan(c, why)) return false;
        return true;
    }

    void build_path_tables() {
        for (size_t d : dirs_) {
            const Entry& e = entries_[d];
            const std::string id = d == 0 ? std::string(1, '\0') : e.name;
            const uint16_t parent = static_cast<uint16_t>(d == 0 ? 1 : entries_[e.parent].number);
            for (int m = 0; m < 2; ++m) {
                std::vector<uint8_t>& t = m ? path_m_ : path_l_;
                const size_t at = t.size();
                t.resize(at + 8 + ((id.size() + 1) & ~size_t{1}));
                t[at] = static_cast<uint8_t>(id.size());
                if (m) be32(&t[at + 2], 0), be16(&t[at + 6], parent);
                else le32(&t[at + 2], 0), le16(&t[at + 6], parent);
                std::memcpy(&t[at + 8], id.data(), id.size());
            }
        }
    }
    // The directories' sectors are known only once laid out: the path tables are completed then.
    void fill_path_table(std::vector<uint8_t>& t, bool m) const {
        size_t at = 0;
        for (size_t d : dirs_) {
            if (m) be32(&t[at + 2], entries_[d].sector);
            else le32(&t[at + 2], entries_[d].sector);
            at += 8 + ((t[at] + 1) & ~size_t{1});
        }
    }

    void record(uint8_t* p, const Entry& e, const std::string& name) const {
        const size_t n = record_size(name);
        p[0] = static_cast<uint8_t>(n);
        both32(p + 2, e.sector);
        both32(p + 10, e.size);
        std::memcpy(p + 18, kDate, 7);
        p[25] = e.dir ? 2 : 0;
        both16(p + 28, 1);
        p[32] = static_cast<uint8_t>(name.size());
        std::memcpy(p + 33, name.data(), name.size());
    }

    void directory(const Entry& e, uint32_t index, uint8_t* out) const {
        size_t sector = 0, used = 0;
        const auto put = [&](const Entry& of, const std::string& name) {
            const size_t r = record_size(name);
            if (used + r > kSector) ++sector, used = 0;
            if (sector == index) record(out + used, of, name);
            used += r;
        };
        put(e, std::string(1, '\0'));
        put(entries_[e.parent], std::string(1, '\1'));
        for (size_t c : e.children) put(entries_[c], entries_[c].name);
    }

    void volume_descriptor(uint8_t* out) const {
        out[0] = 1;
        std::memcpy(out + 1, "CD001", 5);
        out[6] = 1;
        std::memset(out + 8, ' ', 32);
        std::memset(out + 40, ' ', 32);
        std::memcpy(out + 40, "MSR", 3);
        both32(out + 80, end_);
        both16(out + 120, 1);
        both16(out + 124, 1);
        both16(out + 128, kSector);
        both32(out + 132, static_cast<uint32_t>(path_l_.size()));
        le32(out + 140, kHighDensity + 18);
        be32(out + 148, kHighDensity + 19);
        record(out + 156, entries_[0], std::string(1, '\0'));
        std::memset(out + 190, ' ', 623);
        out[881] = 1;
    }

    std::vector<uint8_t> ip_;
    std::vector<Entry> entries_;
    std::vector<size_t> dirs_;
    std::vector<uint8_t> path_l_, path_m_;
    std::map<uint32_t, size_t> files_;  // first sector -> entry
    uint32_t end_ = 0;
};

}  // namespace

std::unique_ptr<Drive> Drive::of_image(const qilin::GdRom& disc) { return std::make_unique<ImageDrive>(disc); }

std::unique_ptr<Drive> Drive::of_folder(const std::filesystem::path& folder, std::string& why) {
    auto d = std::make_unique<FolderDrive>();
    if (!d->build(folder, why)) return nullptr;
    return d;
}

}  // namespace msr
