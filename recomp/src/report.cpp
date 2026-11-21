#include "report.hpp"

#include <format>

namespace qilin {

namespace {

std::string ip_json(const IpBin& ip, std::string_view indent) {
    std::string j = "{\n";
    const auto line = [&](std::string_view key, const std::string& value) {
        j += std::format("{}  \"{}\": {},\n", indent, key, json_string(value));
    };
    line("hardware_id", ip.hardware_id);
    line("maker_id", ip.maker_id);
    line("device_info", ip.device_info);
    line("area_symbols", ip.area_symbols);
    line("peripherals", ip.peripherals);
    line("product_number", ip.product_number);
    line("version", ip.version);
    line("release_date", ip.release_date);
    line("boot_file", ip.boot_file);
    line("publisher", ip.publisher);
    line("title", ip.title);
    j += std::format("{}  \"uses_windows_ce\": {},\n", indent, ip.uses_windows_ce);
    j += std::format("{}  \"supports_vga\": {}\n{}}}", indent, ip.supports_vga, indent);
    return j;
}

}  // namespace

std::string json_string(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        if (c == '"' || c == '\\') out += '\\', out += c;
        else if (static_cast<unsigned char>(c) < 0x20) out += std::format("\\u{:04x}", static_cast<unsigned>(c));
        else out += c;
    }
    return out + '"';
}

std::string to_json(const GdRom& disc, const IpBin& ip, std::span<const GdFile> files) {
    std::string j = "{\n  \"tracks\": [\n";
    for (size_t i = 0; i < disc.tracks.size(); ++i) {
        const GdTrack& t = disc.tracks[i];
        j += std::format("    {{\"number\": {}, \"type\": \"{}\", \"area\": \"{}\", \"start\": {}, \"sectors\": {}, "
                         "\"sector_size\": {}, \"file\": {}}}{}\n",
                         t.number, t.data ? "data" : "audio", t.high_density ? "high-density" : "single-density",
                         t.start, t.sectors, t.sector_size, json_string(t.file.filename().string()),
                         i + 1 < disc.tracks.size() ? "," : "");
    }
    j += "  ],\n  \"ip_bin\": " + ip_json(ip, "  ") + ",\n";
    uint64_t bytes = 0;
    j += "  \"files\": [\n";
    for (size_t i = 0; i < files.size(); ++i) {
        const GdFile& f = files[i];
        bytes += f.size;
        unsigned track = 0;
        for (const GdTrack& t : disc.tracks)
            if (f.sector >= t.start && f.sector < t.end()) track = t.number;
        j += std::format("    {{\"path\": {}, \"sector\": {}, \"size\": {}, \"track\": {}}}{}\n", json_string(f.path),
                         f.sector, f.size, track, i + 1 < files.size() ? "," : "");
    }
    j += std::format("  ],\n  \"file_count\": {},\n  \"file_bytes\": {}\n}}\n", files.size(), bytes);
    return j;
}

std::string to_json(const BootFile& boot, const IpBin* ip) {
    const KnownBuild* build = known_build(boot.sha256);
    std::string j = "{\n";
    j += std::format("  \"sha256\": \"{}\",\n", boot.sha256);
    if (build)
        j += std::format("  \"build\": {{\"product_number\": {}, \"version\": {}, \"title\": {}}},\n",
                         json_string(build->product_number), json_string(build->version), json_string(build->title));
    else
        j += "  \"build\": null,\n";
    j += std::format("  \"file_size\": {},\n", boot.image().size());
    j += std::format("  \"load_address\": \"0x{:08X}\",\n", boot.base());
    j += std::format("  \"entry\": \"0x{:08X}\",\n", boot.entry());
    j += std::format("  \"image_end\": \"0x{:08X}\"", boot.end());
    if (ip) j += ",\n  \"ip_bin\": " + ip_json(*ip, "  ");
    return j + "\n}\n";
}

}  // namespace qilin
