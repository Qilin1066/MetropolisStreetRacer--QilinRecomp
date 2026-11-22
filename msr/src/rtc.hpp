// The sound chip's real-time clock (0x00710000), which keeps the date and
// time: seconds since 1950-01-01 00:00, as two 16-bit halves in 32-bit
// words (high at +0, low at +4), with a write enable at +8. Here it is the
// host's clock; a time the game sets is kept as an offset from it.
//
// Runtime code: no std::format.
#pragma once

namespace msr {

void map_rtc();

}  // namespace msr
