// `qilin identify` (identify.hpp), after Toy Commander--QilinRecomp's.
#include "identify.hpp"

#include <algorithm>
#include <stdexcept>
#include <system_error>

#include "bootfile.hpp"
#include "gdrom.hpp"
#include "path.hpp"
#include "sha256.hpp"

namespace qilin {

namespace fs = std::filesystem;

namespace {

// "V1.010" as "1.010".
std::string plain_version(std::string_view v) {
    if (!v.empty() && (v.front() == 'V' || v.front() == 'v')) v.remove_prefix(1);
    return std::string(v);
}

// "Metropolis Street Racer (USA), MK-51012 version 1.010"
std::string describe(const SupportedDisc& d) {
    return std::string(d.title) + ", " + std::string(d.product) + " version " + plain_version(d.version);
}

Identity answer(int code, std::string message) {
    Identity i;
    i.code = code;
    i.ok = code == 0;
    i.message = std::move(message);
    return i;
}

std::string json_string(std::string_view s) {
    std::string out = "\"";
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') {
            out += '\\';
            out += c;
        } else if (c == '\n') {
            out += "\\n";
        } else if (c == '\r') {
            out += "\\r";
        } else if (c == '\t') {
            out += "\\t";
        } else if (u < 0x20) {
            static constexpr char kHex[] = "0123456789abcdef";
            out += "\\u00";
            out += kHex[u >> 4];
            out += kHex[u & 15];
        } else {
            out += c;  // UTF-8 as it is
        }
    }
    return out + "\"";
}

// The known build whose file is `file` and product `product` (bootfile.cpp's table): its SHA-256.
std::string_view sha_of(std::string_view file, std::string_view product) {
    for (const KnownBuild& b : known_builds())
        if (b.file == file && b.product_number == product) return b.sha256;
    throw std::logic_error("no known build for the supported disc's program");
}

}  // namespace

std::span<const SupportedDisc> supported_discs() {
    // The North American disc (bootfile.cpp): MSR.BIN is the game, lifted;
    // 1ST_READ.BIN, the launcher, is replaced natively and not lifted.
    static const SupportedDisc kDiscs[] = {
        {"Metropolis Street Racer (USA)", "MK-51012", "V1.010", "MSR.BIN", sha_of("MSR.BIN", "MK-51012")},
    };
    return kDiscs;
}

Identity identify(const fs::path& input, std::string_view want, std::span<const SupportedDisc> discs) {
    const SupportedDisc* wanted = nullptr;
    if (!want.empty()) {
        for (const SupportedDisc& d : discs)
            if (d.title == want) wanted = &d;
        if (!wanted) return answer(2, "This recompiler knows no build called " + std::string(want) + ".");
    }
    const std::string for_text = wanted ? " This download is for " + describe(*wanted) + "." : std::string();
    const std::string shown = utf8_path(input);
    std::error_code ec;
    if (!fs::exists(input, ec)) return answer(2, "Can't find " + shown + ".");
    if (fs::is_directory(input, ec))
        return answer(2, shown + " is a folder, not a disc image: choose the disc's .cue, .gdi or .chd file." + for_text);
    if (!GdRom::is_image(input))
        return answer(2, utf8_path(input.filename()) + " isn't a disc image: choose the disc's .cue, .gdi or .chd file." + for_text);

    auto disc = GdRom::open(input);
    if (!disc) return answer(2, "Can't read this disc image: " + disc.error() + ".");
    auto system_area = disc->system_area();
    if (!system_area) return answer(2, "Can't read this disc's bootstrap (IP.BIN): " + system_area.error() + ".");
    auto ip = IpBin::parse(*system_area);
    if (!ip) return answer(3, "This disc image has no Dreamcast bootstrap (" + ip.error() + ")." + for_text);
    const std::string says = (ip->title.empty() ? std::string("a Dreamcast disc") : ip->title) +
                             (ip->product_number.empty() ? "" : ", product " + ip->product_number) +
                             (ip->version.empty() ? "" : ", version " + plain_version(ip->version));
    // What the input is, as far as its bootstrap says.
    const auto as_ip = [&](int code, std::string message) {
        Identity i = answer(code, std::move(message));
        i.title = ip->title;
        i.id = ip->product_number;
        i.version = plain_version(ip->version);
        return i;
    };
    const auto files = disc->files();
    if (!files) return answer(2, "Can't read this disc's files: " + files.error() + ".");

    // Each supported disc's program that this disc has, hashed once.
    const SupportedDisc* build = nullptr;
    bool any_program = false;
    std::vector<std::string_view> hashed;
    for (const SupportedDisc& d : discs) {
        if (std::ranges::contains(hashed, d.program)) continue;
        hashed.push_back(d.program);
        const auto file = std::ranges::find(*files, d.program, &GdFile::path);
        if (file == files->end()) continue;
        any_program = true;
        auto bytes = disc->read_file(*file);
        if (!bytes) return answer(2, "Can't read " + std::string(d.program) + " from this disc: " + bytes.error() + ".");
        const std::string sha = sha256_hex(*bytes);
        for (const SupportedDisc& e : discs)
            if (e.program == d.program && e.sha256 == sha) build = &e;
        if (build) break;
    }
    if (!build) {
        if (!any_program) {
            const std::string_view program = wanted ? wanted->program : discs.empty() ? std::string_view("MSR.BIN") : discs.front().program;
            return as_ip(3, "This disc (" + says + ") has no " + std::string(program) + ", the game's program." + for_text);
        }
        return as_ip(3, "This disc isn't a version this download knows. Its bootstrap says it is " + says + "." + for_text);
    }
    Identity i = answer(0, "");
    i.title = std::string(build->title);
    i.id = std::string(build->product);
    i.version = plain_version(build->version);
    if (wanted && wanted != build) {
        i.code = 3;
        i.ok = false;
        i.message = "This is " + std::string(build->title) + "." + for_text;
        return i;
    }
    i.message = "This is " + describe(*build) + ": the right disc.";
    return i;
}

std::string to_json(const Identity& i) {
    return std::string("{\"ok\": ") + (i.ok ? "true" : "false") + ", \"title\": " + json_string(i.title) + ", \"id\": " + json_string(i.id) +
           ", \"version\": " + json_string(i.version) + ", \"message\": " + json_string(i.message) + "}\n";
}

}  // namespace qilin
