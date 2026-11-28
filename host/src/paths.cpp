// Where this port keeps what it writes (paths.hpp).
#include "qilin/host/paths.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <system_error>

#include "qilin/host/vulkan.hpp"

namespace qilin::host {

namespace {
std::filesystem::path user_data_base() {
#if defined(_WIN32)
    if (const wchar_t* p = _wgetenv(L"LOCALAPPDATA"); p && *p) return p;
#elif defined(__APPLE__)
    if (const char* p = std::getenv("HOME"); p && *p) return std::filesystem::path(p) / "Library" / "Application Support";
#else
    if (const char* p = std::getenv("XDG_DATA_HOME"); p && *p) return p;
    if (const char* p = std::getenv("HOME"); p && *p) return std::filesystem::path(p) / ".local" / "share";
#endif
    return {};
}

// For messages: UTF-8 everywhere (path::string() is the ANSI code page on
// Windows, which can't spell every user name).
std::string utf8(const std::filesystem::path& p) {
    const std::u8string s = p.u8string();
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

}  // namespace

std::filesystem::path user_folder() {
    static const std::filesystem::path folder = [] {
        std::filesystem::path p;
        std::error_code ec;
#if defined(_WIN32)
        if (const wchar_t* env = _wgetenv(L"QILIN_SAVE_DIR"); env && *env) {
#else
        if (const char* env = std::getenv("QILIN_SAVE_DIR"); env && *env) {
#endif
            p = env;
        } else {
            const std::filesystem::path base = user_data_base();
            if (base.empty()) die("can't find a folder for saves: no LOCALAPPDATA, HOME or XDG_DATA_HOME");
            p = base / "qilin" / QILIN_APP;
        }
        std::filesystem::create_directories(p, ec);
        if (ec) die("can't create %s: %s", utf8(p).c_str(), ec.message().c_str());
        return p;
    }();
    return folder;
}

}  // namespace qilin::host
