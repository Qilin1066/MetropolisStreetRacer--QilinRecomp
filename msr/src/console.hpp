// The console's own settings (language, stereo or mono) as Shinobi's settings
// library gives them to the game: its three queries the game makes, native,
// answered from msr.ini's [console] (dreamcast::game::set_console), the way
// Toy Commander's port answers its own copy. docs/runtime.md, "The
// console's settings".
//
// Runtime code: no std::format.
#pragma once

namespace msr {

// Registers the natives for MSR.BIN's 0x8C1304BC, 0x8C13043E and
// 0x8C130408 (recomp/src/bootfile.cpp lists them as replaced).
void install_console_settings();

}  // namespace msr
