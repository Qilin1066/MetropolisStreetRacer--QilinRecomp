// The data folder (paths.cpp) when its name is outside the system's ANSI code
// page, as a Japanese or Cyrillic user name on an English Windows makes it:
// QILIN_SAVE_DIR is read as Unicode, and a save and a settings file written
// there read back.
#include "qilin/host/file_store.hpp"
#include "qilin/host/paths.hpp"
#include "path.hpp"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

namespace fs = std::filesystem;

int main() {
    int failures = 0;
    const auto check = [&](bool ok, const char* label) {
        if (!ok) ++failures, std::fprintf(stderr, "FAIL: %s\n", label);
    };
    const fs::path dir = fs::temp_directory_path() / fs::path(u8"qilin_data_保存_Ж_é") / "qilin" / "msr";
    std::error_code ec;
    fs::remove_all(dir.parent_path().parent_path(), ec);
#if defined(_WIN32)
    check(_wputenv_s(L"QILIN_SAVE_DIR", dir.c_str()) == 0, "set QILIN_SAVE_DIR");
#else
    check(setenv("QILIN_SAVE_DIR", dir.c_str(), 1) == 0, "set QILIN_SAVE_DIR");
#endif
    const fs::path folder = qilin::host::user_folder();
    check(folder == dir, "the data folder is QILIN_SAVE_DIR, every character kept");
    check(fs::is_directory(dir, ec), "the data folder is made");
    check(qilin::utf8_path(folder).ends_with(reinterpret_cast<const char*>(u8"qilin_data_保存_Ж_é/qilin/msr")) ||
              qilin::utf8_path(folder).ends_with(reinterpret_cast<const char*>(u8"qilin_data_保存_Ж_é\\qilin\\msr")),
          "the data folder's name in messages is UTF-8");

    std::string bytes;
    check(qilin::host::atomic_write(folder / "memory_card_a1.bin", std::string(128, '\x5A')), "write a memory card there");
    check(qilin::host::read_file(folder / "memory_card_a1.bin", bytes) && bytes == std::string(128, '\x5A'), "read it back");
    check(qilin::host::atomic_write(folder / "msr.ini", "msaa = 4\n"), "write a settings file there");
    check(qilin::host::read_file(folder / "msr.ini", bytes) && bytes == "msaa = 4\n", "read it back");

    fs::remove_all(dir.parent_path().parent_path(), ec);
    if (failures) return 1;
    std::puts("paths: ok");
    return 0;
}
