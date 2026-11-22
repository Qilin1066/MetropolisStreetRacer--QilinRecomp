// The disc's launcher (1ST_READ.BIN), replaced natively (decision of
// 2026-11-21; docs/disc-and-boot-analysis.md, "The hand-over"). Only the game
// (MSR.BIN) is lifted. What the launcher did, the port does here:
//
// - A cold start plays the intro movie (MSR.SFD) and starts the game with its
//   argument block as the file has it (one string, `\manifest.bin`).
// - The game's start-a-program (0x8C02F68A, replaced, not lifted) given
//   `1st_read.bin <args>` restarts the game: its image and RAM as on a first
//   start, the argument block holding `-DEMO=-1` and then those arguments.
// - Anything else it asks to start stops, saying what it was.
//
// Runtime code: no std::format.
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace msr {

// Thrown by start-a-program to main's run loop: start the game afresh with
// these arguments (after the first, the program's own name). It unwinds the
// guest frames on the way, as the console's program swap leaves none.
struct StartGame {
    std::vector<std::string> args;
};

// Registers start-a-program's native code (once).
void install_launcher();

// What the launcher passes the game on the way back: `-DEMO=-1`, then the
// arguments of the command line the game sent (its first word, the program,
// left out). Exposed for the tests.
std::vector<std::string> launcher_arguments(const std::string& command_line, std::string& program);

// Writes an argument block (512 bytes at 0x8C1101F0 in the game's image) to
// `block`: the strings NUL-separated, ending at an empty one. The first is the
// program's own name (`\manifest.bin`, as the file has it). False when they
// don't fit.
bool write_argument_block(std::span<uint8_t> block, const std::vector<std::string>& args);

// The cold start's intro movie (MSR.SFD), which the player may skip.
void play_intro();

inline constexpr uint32_t kArgumentBlock = 0x8C1101F0u;
inline constexpr uint32_t kArgumentBlockSize = 512;

}  // namespace msr
