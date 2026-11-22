// The intro movie the launcher plays on a cold start, MOVIES/MSR.SFD (the
// launcher's own string `/Movies` + `MSR.SFD`). A Sofdec file is an MPEG-1
// system stream: MPEG-1 video (stream 0xE0, decoded by pl_mpeg) and CRI ADX
// sound (stream 0xC0, adx.hpp). Any of A, B, X, Y or Start skips it, as on
// the console.
//
// Runtime code: no std::format.
#pragma once

#include <cstdint>
#include <vector>

namespace msr {

// Plays it in the window (video::start first), with its sound on a playback
// stream of its own (before sound::start). Stops on a file it can't play.
void play_intro(std::vector<uint8_t> sfd);

}  // namespace msr
