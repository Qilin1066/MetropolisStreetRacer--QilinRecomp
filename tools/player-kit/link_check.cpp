// Stands in for the code qilin lift and qilin armlift make from the game's
// MSR.BIN and its sound driver (empty function tables, and the two driver
// entries the sound CPU calls by name), so the game's program can be linked
// without it: CI builds this to show that the game's library (msr_game, or
// the player kit's archive) and its system libraries are complete. (After
// Toy Commander--QilinRecomp's, which has one table.) Run, it stops at its
// usage, having loaded every library it links.
#include <cstddef>

#include "arm_guest.hpp"
#include "guest.hpp"

namespace qilin::rt {
extern const FnEntry g_functions[] = {{0, nullptr, false, false}};
extern const size_t g_function_count = 0;
extern const char g_lifted_sha256[] = "";
extern const JumpSiteEntry g_jump_sites[] = {{0, nullptr}};
extern const size_t g_jump_site_count = 0;
}  // namespace qilin::rt

namespace qilin::arm_rt {
extern const FnEntry g_functions[] = {{0, nullptr}};
extern const unsigned g_function_count = 0;
extern const char g_driver_sha256[] = "";
}  // namespace qilin::arm_rt

namespace qilin::armgen {
// The driver's reset and FIQ vectors (msr/src/sound_cpu.cpp calls them by name).
void arm_000000(arm_rt::Context&) {}
void arm_00001c(arm_rt::Context&) {}
}  // namespace qilin::armgen
