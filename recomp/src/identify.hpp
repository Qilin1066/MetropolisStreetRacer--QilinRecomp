// `qilin identify <disc image> --json [--build <title>]`: what the player's
// disc is, for the players' launcher (tools/player-kit/launcher), as the
// contract every project's recompiler keeps (ADOPTING.md there; modelled on
// Toy Commander--QilinRecomp's). One JSON object on standard output:
//
//   {"ok": true|false, "title": "...", "id": "...", "version": "...", "message": "..."}
//
// and the exit code: 0 when it is the supported build (`--build`'s, or any
// this port knows without it), 3 when it can be read but isn't (the message
// names what it is and what's needed), 2 when it can't be read at all. The
// message is a plain sentence for the player. Here: the disc's file system
// holds the game's program (MSR.BIN, which is what is lifted: the boot file
// is a launcher the port replaces), whose SHA-256 names the build
// (bootfile.cpp's known builds). IP.BIN says what the disc claims to be.
// Nothing is written.
#pragma once

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qilin {

// A disc this port can be built from: its name (game.txt's `build`), the
// product number and version IP.BIN gives, and the program on it that is
// lifted, with that file's SHA-256.
struct SupportedDisc {
    std::string_view title;    // "Metropolis Street Racer (USA)"
    std::string_view product;  // "MK-51012"
    std::string_view version;  // "V1.010"
    std::string_view program;  // "MSR.BIN": its path on the disc
    std::string_view sha256;
};

// What this port supports (from bootfile.cpp's known builds).
std::span<const SupportedDisc> supported_discs();

struct Identity {
    int code = 2;  // 0, 3 or 2, as above
    bool ok = false;
    std::string title, id, version, message;
};

// `want` is a supported disc's title, or empty for any; `discs` the discs
// this port knows (the tests give their own).
Identity identify(const std::filesystem::path& input, std::string_view want, std::span<const SupportedDisc> discs);
inline Identity identify(const std::filesystem::path& input, std::string_view want) { return identify(input, want, supported_discs()); }

std::string to_json(const Identity& identity);

}  // namespace qilin
