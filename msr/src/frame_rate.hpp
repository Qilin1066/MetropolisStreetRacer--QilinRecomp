// The game's frame rate: the pictures it draws a second, its own (60 in its menus, 30 in a race)
// or 60 throughout, from msr.ini's frame_rate (dreamcast::game::set_frame_rate), with the race's
// physics kept at the game's own rate and the pictures between interpolated.
// docs/runtime.md, "Frame rate".
//
// Runtime code: no std::format.
#pragma once

namespace msr {

// Registers the natives for MSR.BIN's 0x8C02D0D0 and 0x8C02D0E6, the game's two frame-rate
// setters (recomp/src/bootfile.cpp lists them as replaced), and the wrappers of its physics:
// the race's world step, a car's step and a car's update (0x8C03BAD4, 0x8C03C14E, 0x8C0AB7B8,
// wrapped).
void install_frame_rate();
// Discard interpolation and timeline state before loading a fresh game image.
void restart_frame_rate();

}  // namespace msr
