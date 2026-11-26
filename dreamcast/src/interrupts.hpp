// Interrupt delivery: the system bus's events reach the handlers the code
// installed, as the console's interrupt controller would deliver them, at
// the points where lifted code polls (guest.hpp). And the video timing: the
// vertical blank, 60 times a second of guest time unless the game's port asks
// for another rate.
#pragma once

namespace dreamcast {

// Installs the service and starts the video clock.
void start_interrupts();
// An event was raised: have the next poll look.
void interrupt_pending();
// The vertical blank's rate, in blanks a second of guest time (60, the
// console's); 0 or less: a blank at every service, a millisecond apart, for a
// game that takes them as fast as it can draw. Any thread.
void set_blank_rate(double hz);

}  // namespace dreamcast
