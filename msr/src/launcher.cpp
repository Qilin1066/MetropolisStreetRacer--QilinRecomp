// The launcher's replacement: msr/src/launcher.hpp.
//
// Runtime code: no std::format.
#include "launcher.hpp"

#include <array>
#include <cctype>
#include <cstring>

#include "guest.hpp"
#include "system.hpp"

namespace msr {

namespace rt = qilin::rt;

namespace {

constexpr uint32_t kStartProgram = 0x8C02F68Au;  // the game's start-a-program
constexpr size_t kCommandMax = 1024;             // longer than any the game builds (0x8C01094C: about 50)

// The command line at `a` in guest memory, NUL-terminated.
std::string read_string(uint32_t a) {
    std::string s;
    for (;;) {
        const auto ch = static_cast<char>(rt::ld8(a + static_cast<uint32_t>(s.size())));
        if (ch == '\0') return s;
        if (s.size() == kCommandMax) rt::fatal("msr: the game asks to start a program with a command line over %zu bytes", kCommandMax);
        s += ch;
    }
}

std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && s[i] == ' ') ++i;
        const size_t start = i;
        while (i < s.size() && s[i] != ' ') ++i;
        if (i > start) out.push_back(s.substr(start, i - start));
    }
    return out;
}

// The program's name as the disc's file system matches it: no leading separator, upper case.
std::string file_name(std::string w) {
    while (!w.empty() && (w.front() == '\\' || w.front() == '/')) w.erase(0, 1);
    for (char& ch : w) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return w;
}

// Start-a-program (0x8C02F68A), given a command line in r4 (r5 and r6: the
// rest of what the library's transfer program takes, an empty string from the
// attract loop's call). It never returns on the console: the target is
// loaded over the game.
void start_program(rt::Context& c) {
    const std::string command = read_string(c.r[4]);
    rt::log("msr: the game starts a program: %s", command.c_str());
    std::string program;
    std::vector<std::string> args = launcher_arguments(command, program);
    if (program == "1ST_READ.BIN") throw StartGame{std::move(args)};
    if (program == "SG_DPLDR.BIN")
        rt::fatal("msr: the game starts the web browser's loader (%s): there is no browser in this port, and what "
                  "its menu entry should do instead isn't decided (docs/disc-and-boot-analysis.md)",
                  command.c_str());
    rt::fatal("msr: the game starts a program the port doesn't know: \"%s\" (only the launcher, 1st_read.bin, is "
              "replaced; docs/disc-and-boot-analysis.md)",
              command.c_str());
}

constexpr std::array<rt::Provided, 1> kNatives{{
    {kStartProgram, "launcher: start a program", start_program},
}};

}  // namespace

void install_launcher() { rt::provide(kNatives); }

std::vector<std::string> launcher_arguments(const std::string& command_line, std::string& program) {
    std::vector<std::string> w = words(command_line);
    program = w.empty() ? std::string() : file_name(w.front());
    // The launcher's main (0x8C01032C in 1ST_READ.BIN), given arguments: `msr.bin -DEMO=-1` and them.
    std::vector<std::string> args{"-DEMO=-1"};
    if (w.size() > 1) args.insert(args.end(), w.begin() + 1, w.end());
    return args;
}

bool write_argument_block(std::span<uint8_t> block, const std::vector<std::string>& args) {
    // The first string is the program's name: as the file has it on a cold start.
    static constexpr char kSelf[] = "\\manifest.bin";
    std::memset(block.data(), 0, block.size());
    size_t at = 0;
    const auto put = [&](const std::string& s) {
        // The game's argument count (0x8C02FA54) steps over the byte after each NUL, so a string
        // of one character would end the count there: none of the game's is that short.
        if (s.size() < 2 || s.find('\0') != std::string::npos) return false;
        if (at + s.size() + 1 + 1 > block.size()) return false;  // its NUL, and the empty string's
        std::memcpy(block.data() + at, s.data(), s.size());
        at += s.size() + 1;
        return true;
    };
    if (!put(kSelf)) return false;
    for (const std::string& a : args)
        if (!put(a)) return false;
    // The count reads one byte past the empty string's NUL: zero too (memset above), and inside.
    return at + 1 < block.size();
}

}  // namespace msr
