// JSON reports for `qilin disc` and `qilin info`. Developer tool code (in
// qilin_core): std::format is fine here.
#pragma once

#include <span>
#include <string>
#include <string_view>

#include "bootfile.hpp"
#include "gdrom.hpp"

namespace qilin {

std::string json_string(std::string_view s);

// Tracks, IP.BIN's meta information and the file list of a disc image.
std::string to_json(const GdRom& disc, const IpBin& ip, std::span<const GdFile> files);

// A boot file: hash, whether it's a supported build, where it loads. `ip` is
// optional: the IP.BIN it came with.
std::string to_json(const BootFile& boot, const IpBin* ip);

}  // namespace qilin
